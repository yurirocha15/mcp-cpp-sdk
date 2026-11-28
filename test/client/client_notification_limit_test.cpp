// The notification rate limit. The token bucket is internal, so the unit tests include its header;
// the client tests use a scripted 2026-07-28 peer that floods progress and log notifications.
#include "mcp/detail/protocol/notification_budget.hpp"

#include "support/hang_guard.hpp"
#include "support/test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace client_notification_limit_test {

namespace {

using mcp::NotificationRateLimit;
using mcp::detail::TokenBucket;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

json notification(std::string method, json params) {
    return {{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

/// A client on a scripted transport whose 2026-07-28 peer answers tools/call with @p floods
/// progress notifications for the call's token and as many log messages before its result.
class Harness {
   public:
    Harness(mcp::ClientOptions options, int floods)
        : transport(std::make_shared<ScriptedTransport>(io.get_executor())),
          guard(io),
          floods_(floods) {
        options.protocol = mcp::ClientProtocol::pinned("2026-07-28");
        client = std::make_unique<mcp::Client>(transport, io.get_executor(), std::move(options));
        transport->set_on_write([this](std::string_view text) { on_write(json::parse(text)); });
    }

    void run(std::function<mcp::Task<void>()> body) {
        guard.expires_after(g_hang_guard);
        guard.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                guard_fired = true;
                io.stop();
            }
        });
        mcp_test::spawn_detached(io, [this, body = std::move(body)]() -> mcp::Task<void> {
            try {
                co_await client->connect("limit-test", "1.0");
                co_await body();
            } catch (const std::exception& error) {
                ADD_FAILURE() << "unexpected exception: " << error.what();
            }
            client->close();
            guard.cancel();
        });
        io.run();
        EXPECT_FALSE(guard_fired) << "the test did not finish";
    }

    boost::asio::io_context io;
    std::shared_ptr<ScriptedTransport> transport;
    std::unique_ptr<mcp::Client> client;
    boost::asio::steady_timer guard;
    bool guard_fired{false};

   private:
    void deliver(const json& message) { transport->enqueue_message(message.dump()); }

    void on_write(const json& message) {
        if (!message.contains("method") || !message.contains("id")) {
            return;
        }
        const auto method = message.at("method").get<std::string>();
        if (method == "server/discover") {
            deliver({{"jsonrpc", "2.0"},
                     {"id", message.at("id")},
                     {"result",
                      {{"resultType", "complete"},
                       {"supportedVersions", json::array({"2026-07-28"})},
                       {"capabilities", {{"tools", json::object()}}}}}});
            return;
        }
        if (method != "tools/call") {
            return;
        }
        const auto& token = message.at("params").at("_meta").at("progressToken");
        for (int index = 0; index < floods_; ++index) {
            deliver(notification("notifications/progress",
                                 {{"progressToken", token}, {"progress", index}}));
            deliver(notification("notifications/message", {{"level", "info"}, {"data", index}}));
        }
        deliver({{"jsonrpc", "2.0"},
                 {"id", message.at("id")},
                 {"result", {{"resultType", "complete"}, {"content", json::array()}}}});
    }

    int floods_;
};

}  // namespace

TEST(TokenBucketTest, LetsTheBurstThroughThenTheRate) {
    const auto start = Clock::now();
    TokenBucket bucket(NotificationRateLimit{2, 10.0}, start);
    EXPECT_TRUE(bucket.take(start));
    EXPECT_TRUE(bucket.take(start));
    EXPECT_FALSE(bucket.take(start));
    // A tenth of a second at 10 per second earns one more.
    EXPECT_TRUE(bucket.take(start + std::chrono::milliseconds(100)));
    EXPECT_FALSE(bucket.take(start + std::chrono::milliseconds(100)));
    // A long pause refills no more than the burst.
    const auto later = start + std::chrono::seconds(60);
    EXPECT_TRUE(bucket.take(later));
    EXPECT_TRUE(bucket.take(later));
    EXPECT_FALSE(bucket.take(later));
}

TEST(TokenBucketTest, NoBurstMeansNoLimitAndNoRateMeansTheBurstOnly) {
    const auto start = Clock::now();
    TokenBucket unlimited(NotificationRateLimit{}, start);
    for (int index = 0; index < 10'000; ++index) {
        ASSERT_TRUE(unlimited.take(start));
    }
    TokenBucket capped(NotificationRateLimit{1, 0.0}, start);
    EXPECT_TRUE(capped.take(start));
    EXPECT_FALSE(capped.take(start + std::chrono::hours(1)));
}

TEST(TokenBucketTest, RefusesARateThatIsNegativeOrNotANumber) {
    EXPECT_THROW(mcp::detail::check_rate_limit(NotificationRateLimit{1, -1.0}), std::invalid_argument);
    EXPECT_THROW(mcp::detail::check_rate_limit(NotificationRateLimit{1, std::nan("")}),
                 std::invalid_argument);
    EXPECT_NO_THROW(mcp::detail::check_rate_limit(NotificationRateLimit{1, 0.0}));
}

TEST(ClientNotificationLimit, IsOffUnlessSet) {
    Harness harness(mcp::ClientOptions{}, 50);
    int progress = 0;
    std::atomic<int> logs{0};
    harness.client->on_notification("notifications/message", [&logs](const json&) { ++logs; });
    harness.run([&]() -> mcp::Task<void> {
        mcp::RequestOptions options;
        options.on_progress = [&progress](const mcp::ProgressNotificationParams&) { ++progress; };
        co_await harness.client->call_tool("flood", json::object(), options);
    });
    EXPECT_EQ(progress, 50);
    EXPECT_EQ(logs.load(), 50);
    EXPECT_EQ(harness.client->dropped_notifications(), 0U);
}

// progress.mdx: "Both parties SHOULD implement rate limiting to prevent flooding".
TEST(ClientNotificationLimit, DropsWhatGoesOverAndCountsIt) {
    mcp::ClientOptions options;
    options.notification_rate_limit = NotificationRateLimit{3, 0.0};
    Harness harness(options, 10);
    int progress = 0;
    std::atomic<int> logs{0};
    harness.client->on_notification("notifications/message", [&logs](const json&) { ++logs; });
    harness.run([&]() -> mcp::Task<void> {
        mcp::RequestOptions request_options;
        request_options.on_progress = [&progress](const mcp::ProgressNotificationParams&) {
            ++progress;
        };
        co_await harness.client->call_tool("flood", json::object(), request_options);
    });
    EXPECT_EQ(progress, 3);
    EXPECT_EQ(logs.load(), 3);
    EXPECT_EQ(harness.client->dropped_notifications(), 14U);
}

TEST(ClientNotificationLimit, RefusesARateThatIsNegative) {
    boost::asio::io_context io;
    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    mcp::ClientOptions options;
    options.notification_rate_limit = NotificationRateLimit{1, -2.0};
    EXPECT_THROW(mcp::Client(transport, io.get_executor(), options), std::invalid_argument);
}

}  // namespace client_notification_limit_test
