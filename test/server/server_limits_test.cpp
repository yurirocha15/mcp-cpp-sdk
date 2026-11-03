#include "../test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

mcp::Task<void> wait_until(std::function<bool()> condition) {
    auto executor = co_await boost::asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    if (!condition()) {
        ADD_FAILURE() << "gave up waiting";
    }
}

json call(int id, const std::string& tool) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "tools/call"},
            {"params", {{"name", tool}, {"arguments", json::object()}}}};
}

class ServerLimitsTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_;
    std::shared_ptr<ScriptedTransport> transport_ =
        std::make_shared<ScriptedTransport>(io_.get_executor());
    std::vector<json> written_;
    std::unique_ptr<mcp::Server> server_;
    std::atomic<bool> release_{false};
    std::atomic<int> running_{0};

    void SetUp() override {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
        server_ = std::make_unique<mcp::Server>(mcp::Implementation{"limits", "1.0"}, caps);
        server_->add_tool<json, json>("held", "Returns once released", json{{"type", "object"}},
                                      [this](json /*args*/) -> mcp::Task<json> {
                                          running_.fetch_add(1);
                                          co_await wait_until([this] { return release_.load(); });
                                          co_return json::object();
                                      });
        server_->add_tool<json, json>("ask", "Asks the client", json{{"type", "object"}},
                                      [this](json /*args*/) -> mcp::Task<json> {
                                          co_return co_await server_->send_request("roots/list",
                                                                                   std::nullopt);
                                      });
        transport_->set_on_write(
            [this](std::string_view text) { written_.push_back(json::parse(text)); });
        transport_->enqueue_message(make_initialize_request("i").dump());
    }

    void run(std::function<mcp::Task<void>()> exchange) {
        boost::asio::co_spawn(
            io_, [this]() -> mcp::Task<void> { co_await server_->run(transport_, io_.get_executor()); },
            boost::asio::detached);
        boost::asio::co_spawn(
            io_,
            [this, exchange = std::move(exchange)]() -> mcp::Task<void> {
                // A failed exchange still ends the session, or the test would never return.
                try {
                    co_await exchange();
                } catch (const std::exception& error) {
                    ADD_FAILURE() << error.what();
                }
                transport_->close();
            },
            boost::asio::detached);
        io_.run();
    }

    const json* answer_to(const json& id) const {
        for (const auto& message : written_) {
            if (message.contains("id") && message["id"] == id && !message.contains("method")) {
                return &message;
            }
        }
        return nullptr;
    }

    /// The session is ready and nothing of the handshake is still dispatching: a request answered
    /// after notifications/initialized was read shows the first, and the pause the second.
    mcp::Task<void> initialized() {
        co_await wait_until([this] { return answer_to("i") != nullptr; });
        transport_->enqueue_message(make_initialized_notification().dump());
        transport_->enqueue_message(
            json{{"jsonrpc", "2.0"}, {"id", "ready"}, {"method", "tools/list"}}.dump());
        co_await wait_until([this] { return answer_to("ready") != nullptr; });
        boost::asio::steady_timer settle(io_.get_executor(), std::chrono::milliseconds(5));
        co_await settle.async_wait(boost::asio::use_awaitable);
    }
};

}  // namespace

TEST_F(ServerLimitsTest, RequestPastTheLimitIsRefusedAndTheSessionGoesOn) {
    server_->set_max_in_flight_requests(2);
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->enqueue_message(call(1, "held").dump());
        co_await wait_until([this] { return running_.load() == 1; });
        transport_->enqueue_message(call(2, "held").dump());
        co_await wait_until([this] { return running_.load() == 2; });
        transport_->enqueue_message(call(3, "held").dump());
        co_await wait_until([this] { return answer_to(3) != nullptr; });
        release_.store(true);
        co_await wait_until([this] { return answer_to(1) != nullptr && answer_to(2) != nullptr; });
        transport_->enqueue_message(call(4, "held").dump());
        co_await wait_until([this] { return answer_to(4) != nullptr; });
    });
    ASSERT_NE(answer_to(3), nullptr);
    EXPECT_EQ((*answer_to(3))["error"]["code"], mcp::g_INTERNAL_ERROR);
    EXPECT_EQ(running_.load(), 3);
    ASSERT_NE(answer_to(4), nullptr);
    EXPECT_TRUE(answer_to(4)->contains("result"));
}

// The handler holds a place until the client answers, so the answer must still be read at the limit.
TEST_F(ServerLimitsTest, AnswerFromTheClientIsReadAtTheLimit) {
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        server_->set_max_in_flight_requests(1);
        transport_->enqueue_message(call(1, "ask").dump());
        co_await wait_until([this] {
            return !written_.empty() && written_.back().value("method", "") == "roots/list";
        });
        const auto reverse_id = written_.back().at("id");
        transport_->enqueue_message(
            json{{"jsonrpc", "2.0"}, {"id", reverse_id}, {"result", {{"roots", json::array()}}}}
                .dump());
        co_await wait_until([this] { return answer_to(1) != nullptr; });
    });
    ASSERT_NE(answer_to(1), nullptr);
    // A handler that failed is answered with a result too, marked isError.
    EXPECT_EQ((*answer_to(1))["result"]["structuredContent"], (json{{"roots", json::array()}}))
        << *answer_to(1);
}

TEST_F(ServerLimitsTest, ClientThatNeverAnswersFailsTheHandlerAtTheTimeout) {
    server_->set_reverse_request_timeout(std::chrono::milliseconds(50));
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->enqueue_message(call(1, "ask").dump());
        co_await wait_until([this] { return answer_to(1) != nullptr; });
    });
    ASSERT_NE(answer_to(1), nullptr);
    EXPECT_NE(answer_to(1)->dump().find("did not answer in time"), std::string::npos) << *answer_to(1);
    // The client is told the server no longer wants the answer.
    const auto asked = std::find_if(written_.begin(), written_.end(), [](const json& message) {
        return message.value("method", "") == "roots/list";
    });
    ASSERT_NE(asked, written_.end());
    const auto told = std::find_if(written_.begin(), written_.end(), [&asked](const json& message) {
        return message.value("method", "") == "notifications/cancelled" &&
               message["params"]["requestId"] == (*asked)["id"];
    });
    EXPECT_NE(told, written_.end());
}

// A cancel read before the handler asks the client has no wait to end yet; the ask must not go out.
TEST_F(ServerLimitsTest, CancelReadBeforeTheHandlerAsksEndsItsAsk) {
    std::string failure;
    server_->add_tool<json, json>("ask-later", "Asks the client once released",
                                  json{{"type", "object"}},
                                  [&](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                      running_.fetch_add(1);
                                      co_await wait_until([this] { return release_.load(); });
                                      try {
                                          co_await ctx.request_roots();
                                      } catch (const std::exception& error) {
                                          failure = error.what();
                                      }
                                      co_return json::object();
                                  });
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->enqueue_message(call(1, "ask-later").dump());
        co_await wait_until([this] { return running_.load() == 1; });
        transport_->enqueue_message(json{
            {"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 1}}}}
                                        .dump());
        // Answered after the cancel was read: both are dispatched in order on the session strand.
        transport_->enqueue_message(json{{"jsonrpc", "2.0"}, {"id", "p"}, {"method", "ping"}}.dump());
        co_await wait_until([this] { return answer_to("p") != nullptr; });
        release_.store(true);
        co_await wait_until([&failure] { return !failure.empty(); });
    });
    EXPECT_NE(failure.find("cancelled"), std::string::npos) << failure;
    for (const auto& message : written_) {
        EXPECT_NE(message.value("method", ""), "roots/list") << message;
    }
}

// An error the server cannot decode still answers its request, which fails at once.
TEST_F(ServerLimitsTest, MalformedErrorFromTheClientFailsTheHandler) {
    std::vector<json> errors = {"boom", json{{"code", "-32000"}, {"message", "m"}},
                                json{{"message", "m"}}, json{{"code", -32000}, {"message", 5}}};
    std::vector<std::string> failures;
    server_->add_tool<json, json>("ask-through-context", "Asks the client", json{{"type", "object"}},
                                  [&](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                      try {
                                          co_await ctx.request_roots();
                                          failures.emplace_back("answered");
                                      } catch (const std::exception& error) {
                                          failures.emplace_back(error.what());
                                      }
                                      co_return json::object();
                                  });
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        for (std::size_t i = 0; i < errors.size(); ++i) {
            const auto asked_before =
                std::count_if(written_.begin(), written_.end(),
                              [](const json& m) { return m.value("method", "") == "roots/list"; });
            transport_->enqueue_message(
                json{{"jsonrpc", "2.0"},
                     {"id", static_cast<int>(i) + 10},
                     {"method", "tools/call"},
                     {"params", {{"name", "ask-through-context"}, {"arguments", json::object()}}}}
                    .dump());
            co_await wait_until([this, asked_before] {
                return std::count_if(written_.begin(), written_.end(), [](const json& m) {
                           return m.value("method", "") == "roots/list";
                       }) > asked_before;
            });
            const auto reverse_id = written_.back().at("id");
            transport_->enqueue_message(
                json{{"jsonrpc", "2.0"}, {"id", reverse_id}, {"error", errors[i]}}.dump());
            co_await wait_until([&failures, i] { return failures.size() == i + 1; });
        }
    });
    ASSERT_EQ(failures.size(), errors.size());
    for (const auto& failure : failures) {
        EXPECT_NE(failure.find("Malformed"), std::string::npos) << failure;
    }
}

TEST_F(ServerLimitsTest, CancellingAHandlerEndsItsWaitForTheClient) {
    std::string failure;
    server_->add_tool<json, json>("ask-through-context", "Asks the client", json{{"type", "object"}},
                                  [&](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                      try {
                                          co_await ctx.request_roots();
                                      } catch (const std::exception& error) {
                                          failure = error.what();
                                      }
                                      co_return json::object();
                                  });
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->enqueue_message(
            json{{"jsonrpc", "2.0"},
                 {"id", 1},
                 {"method", "tools/call"},
                 {"params", {{"name", "ask-through-context"}, {"arguments", json::object()}}}}
                .dump());
        co_await wait_until([this] {
            return !written_.empty() && written_.back().value("method", "") == "roots/list";
        });
        transport_->enqueue_message(json{
            {"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 1}}}}
                                        .dump());
        co_await wait_until([&failure] { return !failure.empty(); });
    });
    EXPECT_NE(failure.find("cancelled"), std::string::npos) << failure;
}

TEST_F(ServerLimitsTest, AResultThatCannotBeEncodedDoesNotExposeTheLibraryText) {
    server_->add_tool<json, json>(
        "not-text", "Returns bytes", json{{"type", "object"}},
        [](json /*args*/) -> mcp::Task<json> { co_return json{{"bytes", std::string("\xff\xfe")}}; });
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->enqueue_message(call(1, "not-text").dump());
        co_await wait_until([this] { return answer_to(1) != nullptr; });
    });
    ASSERT_NE(answer_to(1), nullptr);
    EXPECT_EQ(answer_to(1)->dump().find("json.exception"), std::string::npos) << *answer_to(1);
    EXPECT_EQ((*answer_to(1))["error"]["code"], mcp::g_INTERNAL_ERROR) << *answer_to(1);
}

TEST_F(ServerLimitsTest, SettersRefuseValuesThatCannotWork) {
    EXPECT_THROW(server_->set_max_in_flight_requests(0), std::invalid_argument);
    EXPECT_THROW(server_->set_reverse_request_timeout(std::chrono::milliseconds(0)),
                 std::invalid_argument);
}

TEST_F(ServerLimitsTest, SubscriptionsOfASessionAreLimited) {
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        for (int id = 1; id <= 4097; ++id) {
            transport_->enqueue_message(json{{"jsonrpc", "2.0"},
                                             {"id", id},
                                             {"method", "resources/subscribe"},
                                             {"params", {{"uri", "mem://" + std::to_string(id)}}}}
                                            .dump());
            if (id % 256 == 0 || id == 4097) {
                co_await wait_until([this, id] { return answer_to(id) != nullptr; });
            }
        }
    });
    ASSERT_NE(answer_to(4096), nullptr);
    EXPECT_TRUE(answer_to(4096)->contains("result"));
    ASSERT_NE(answer_to(4097), nullptr);
    EXPECT_EQ((*answer_to(4097))["error"]["code"], mcp::g_INVALID_PARAMS);
}

namespace {

/// A server that answers the handshake, then sends pings and takes no answers.
class FloodingPeer final : public mcp::ITransport {
   public:
    explicit FloodingPeer(const boost::asio::any_io_executor& executor) : stalled_(executor) {
        stalled_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            co_await boost::asio::post(co_await boost::asio::this_coro::executor,
                                       boost::asio::use_awaitable);
            if (!replies_.empty()) {
                auto reply = std::move(replies_.front());
                replies_.pop_front();
                co_return reply;
            }
            if (flooding_) {
                ++pings_read;
                co_return json{{"jsonrpc", "2.0"}, {"id", pings_read}, {"method", "ping"}}.dump();
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        const auto sent = json::parse(message);
        if (sent.value("method", "") == "initialize") {
            replies_.push_back(json{{"jsonrpc", "2.0"},
                                    {"id", sent.at("id")},
                                    {"result",
                                     {{"protocolVersion", "2025-11-25"},
                                      {"capabilities", json::object()},
                                      {"serverInfo", {{"name", "flood"}, {"version", "1"}}}}}}
                                   .dump());
            co_return;
        }
        if (sent.value("method", "") == "server/discover") {
            replies_.push_back(json{{"jsonrpc", "2.0"},
                                    {"id", sent.at("id")},
                                    {"error", {{"code", -32601}, {"message", "Method not found"}}}}
                                   .dump());
            co_return;
        }
        if (sent.contains("method")) {
            flooding_ = true;
            co_return;
        }
        if (!closed_) {
            boost::system::error_code ignored;
            co_await stalled_.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }
    }

    void close() override {
        closed_ = true;
        stalled_.cancel();
    }

    int pings_read{0};

   private:
    boost::asio::steady_timer stalled_;
    std::deque<std::string> replies_;
    bool flooding_{false};
    bool closed_{false};
};

}  // namespace

// Each request of the server holds memory until its answer is written, so the client stops reading.
TEST(ClientLimits, ServerThatTakesNoAnswersCannotFloodTheClient) {
    boost::asio::io_context io;
    auto peer = std::make_shared<FloodingPeer>(io.get_executor());
    mcp::Client client(peer, io.get_executor());

    int settled = 0;
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("limits", "1.0");
            co_await wait_until([&] { return peer->pings_read >= 2000 || peer->pings_read == 1024; });
            boost::asio::steady_timer settle(io.get_executor(), std::chrono::milliseconds(20));
            co_await settle.async_wait(boost::asio::use_awaitable);
            settled = peer->pings_read;
            client.close();
        },
        boost::asio::detached);
    io.run();

    EXPECT_EQ(settled, 1024);
}
