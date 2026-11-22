// The response cache of the client. The cache class is internal, so the unit tests include its
// header; the client tests use a scripted 2026-07-28 peer that counts the requests reaching it.
#include "../../src/detail/response_cache.hpp"

#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "../support/spawn.hpp"
#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace client_response_cache_test {

namespace {

using mcp::CacheMode;
using mcp::detail::ResponseCache;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

json response_to(const json& request, json result) {
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}};
}

mcp::ResponseCacheOptions cache_on(std::size_t max_entries = 512) {
    mcp::ResponseCacheOptions options;
    options.enabled = true;
    options.max_entries = max_entries;
    return options;
}

/// A client on a scripted transport whose peer is a 2026-07-28 server. The peer answers the
/// cacheable methods with results that carry the `ttlMs` of `ttl` (none for a method it does not
/// list) and a `served` count, and records every request it gets.
class Harness {
   public:
    explicit Harness(mcp::ClientOptions options = {})
        : transport(std::make_shared<ScriptedTransport>(io.get_executor())), guard(io) {
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
                co_await client->connect("cache-test", "1.0");
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

    void deliver(const json& message) { transport->enqueue_message(message.dump()); }

    /// The requests of @p method the peer got.
    [[nodiscard]] std::size_t count(const std::string& method) const {
        const auto found = requests.find(method);
        return found == requests.end() ? 0 : found->second;
    }

    std::map<std::string, std::int64_t> ttl{{"tools/list", 60000},
                                            {"resources/read", 60000},
                                            {"resources/templates/list", 60000},
                                            {"prompts/list", 0}};
    /// URIs whose read is answered first with an input_required result.
    std::vector<std::string> asks_first;
    /// Cursors the peer refuses with -32602, as a server does with a cursor it no longer honors.
    std::vector<std::string> refused_cursors;
    std::map<std::string, std::size_t> requests;

    boost::asio::io_context io;
    std::shared_ptr<ScriptedTransport> transport;
    std::unique_ptr<mcp::Client> client;
    boost::asio::steady_timer guard;
    bool guard_fired{false};

   private:
    void on_write(const json& message) {
        if (!message.contains("method") || !message.contains("id")) {
            return;
        }
        const auto method = message.at("method").get<std::string>();
        if (method == "server/discover") {
            deliver(response_to(
                message,
                {{"resultType", "complete"},
                 {"supportedVersions", json::array({"2026-07-28"})},
                 {"capabilities",
                  {{"tools", json::object()},
                   {"resources", json::object()},
                   {"prompts", json::object()}}},
                 {"_meta",
                  {{"io.modelcontextprotocol/serverInfo", {{"name", "peer"}, {"version", "1"}}}}}}));
            return;
        }
        const auto served = ++requests[method];
        const auto& params = message.at("params");
        if (const auto cursor = params.find("cursor");
            cursor != params.end() && cursor->is_string() &&
            std::find(refused_cursors.begin(), refused_cursors.end(), cursor->get<std::string>()) !=
                refused_cursors.end()) {
            deliver({{"jsonrpc", "2.0"},
                     {"id", message.at("id")},
                     {"error", {{"code", -32602}, {"message", "Invalid pagination cursor"}}}});
            return;
        }
        json result = {{"resultType", "complete"}, {"served", served}};
        if (method == "tools/list") {
            result["tools"] = json::array();
        } else if (method == "prompts/list") {
            result["prompts"] = json::array();
        } else if (method == "resources/list") {
            result["resources"] = json::array();
        } else if (method == "resources/templates/list") {
            result["resourceTemplates"] = json::array();
        } else if (method == "resources/read") {
            const auto uri = params.at("uri").get<std::string>();
            if (std::find(asks_first.begin(), asks_first.end(), uri) != asks_first.end() &&
                !params.contains("requestState")) {
                deliver(response_to(message,
                                    {{"resultType", "input_required"}, {"requestState", "step-2"}}));
                return;
            }
            result["contents"] = json::array({{{"uri", uri}, {"text", "v" + std::to_string(served)}}});
        } else {
            return;
        }
        if (const auto found = ttl.find(method); found != ttl.end()) {
            result["ttlMs"] = found->second;
            result["cacheScope"] = "private";
        }
        deliver(response_to(message, std::move(result)));
    }
};

/// The text of the first content of a read.
std::string text_of(const mcp::ReadResourceResult& result) {
    return json(result).at("contents").at(0).at("text").get<std::string>();
}

mcp::RequestOptions with_mode(CacheMode mode) {
    mcp::RequestOptions options;
    options.cache_mode = mode;
    return options;
}

json notification(std::string method, json params = json::object()) {
    return {{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

/// Waits, a millisecond at a time, until @p condition holds or the hang guard would fire.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await boost::asio::this_coro::executor;
    const auto give_up = Clock::now() + g_hang_guard;
    while (!condition() && Clock::now() < give_up) {
        boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return condition();
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// The cache

TEST(ResponseCacheTest, KeysOnTheMethodAndTheParameters) {
    EXPECT_EQ(ResponseCache::key("tools/list", std::nullopt),
              ResponseCache::key("tools/list", std::nullopt));
    EXPECT_NE(ResponseCache::key("tools/list", std::nullopt),
              ResponseCache::key("prompts/list", std::nullopt));
    EXPECT_NE(ResponseCache::key("tools/list", std::nullopt),
              ResponseCache::key("tools/list", json{{"cursor", "2"}}));
    EXPECT_NE(ResponseCache::key("resources/read", json{{"uri", "a"}}),
              ResponseCache::key("resources/read", json{{"uri", "b"}}));
}

TEST(ResponseCacheTest, KeepsAResultForItsTtlCappedByTheLimit) {
    auto options = cache_on();
    options.max_ttl = std::chrono::seconds(10);
    ResponseCache cache(options);
    const auto start = Clock::now();
    const auto key = ResponseCache::key("tools/list", std::nullopt);

    cache.store(key, "tools/list", std::nullopt, json{{"ttlMs", 1000}, {"n", 1}}, cache.generation(),
                start);
    EXPECT_EQ(cache.find(key, start + std::chrono::milliseconds(999)),
              (json{{"ttlMs", 1000}, {"n", 1}}));
    EXPECT_FALSE(cache.find(key, start + std::chrono::milliseconds(1000)));

    cache.store(key, "tools/list", std::nullopt, json{{"ttlMs", 3600000}}, cache.generation(), start);
    EXPECT_TRUE(cache.find(key, start + std::chrono::seconds(9)));
    EXPECT_FALSE(cache.find(key, start + std::chrono::seconds(10)));

    cache.store(key, "tools/list", std::nullopt, json{{"ttlMs", std::uint64_t{1} << 63U}},
                cache.generation(), start);
    EXPECT_TRUE(cache.find(key, start + std::chrono::seconds(9)));
}

TEST(ResponseCacheTest, KeepsNothingWithoutAPositiveTtl) {
    ResponseCache cache(cache_on());
    const auto key = ResponseCache::key("tools/list", std::nullopt);
    for (const auto& result : {json{{"ttlMs", 0}}, json{{"ttlMs", -5}}, json::object(),
                               json{{"ttlMs", "60000"}}, json::array()}) {
        cache.store(key, "tools/list", std::nullopt, result, cache.generation());
        EXPECT_FALSE(cache.find(key)) << result;
    }
    // A fresh answer with no lifetime replaces the one kept.
    cache.store(key, "tools/list", std::nullopt, json{{"ttlMs", 60000}}, cache.generation());
    cache.store(key, "tools/list", std::nullopt, json{{"ttlMs", 0}}, cache.generation());
    EXPECT_FALSE(cache.find(key));
}

TEST(ResponseCacheTest, MakesRoomByDroppingTheResultClosestToStale) {
    ResponseCache cache(cache_on(2));
    const auto start = Clock::now();
    const auto key_of = [](const std::string& uri) {
        return ResponseCache::key("resources/read", json{{"uri", uri}});
    };
    for (const auto& [uri, ttl] :
         std::vector<std::pair<std::string, int>>{{"a", 5000}, {"b", 1000}, {"c", 3000}}) {
        cache.store(key_of(uri), "resources/read", json{{"uri", uri}}, json{{"ttlMs", ttl}},
                    cache.generation(), start);
    }
    EXPECT_EQ(cache.size(), 2U);
    EXPECT_TRUE(cache.find(key_of("a"), start));
    EXPECT_FALSE(cache.find(key_of("b"), start));
    EXPECT_TRUE(cache.find(key_of("c"), start));
}

TEST(ResponseCacheTest, NotificationsDropWhatTheyName) {
    ResponseCache cache(cache_on());
    const auto store = [&cache](const std::string& method, const std::optional<json>& params) {
        cache.store(ResponseCache::key(method, params), method, params, json{{"ttlMs", 60000}},
                    cache.generation());
    };
    const auto kept = [&cache](const std::string& method, const std::optional<json>& params) {
        return cache.find(ResponseCache::key(method, params)).has_value();
    };
    store("tools/list", std::nullopt);
    store("tools/list", json{{"cursor", "2"}});
    store("prompts/list", std::nullopt);
    store("resources/list", std::nullopt);
    store("resources/templates/list", std::nullopt);
    store("resources/read", json{{"uri", "a"}});
    store("resources/read", json{{"uri", "b"}});

    cache.invalidate("notifications/tools/list_changed", json::object());
    EXPECT_FALSE(kept("tools/list", std::nullopt));
    EXPECT_FALSE(kept("tools/list", json{{"cursor", "2"}}));
    EXPECT_TRUE(kept("prompts/list", std::nullopt));

    cache.invalidate("notifications/resources/list_changed", json::object());
    EXPECT_FALSE(kept("resources/list", std::nullopt));
    EXPECT_FALSE(kept("resources/templates/list", std::nullopt));
    EXPECT_TRUE(kept("resources/read", json{{"uri", "a"}}));

    cache.invalidate("notifications/resources/updated", json{{"params", {{"uri", "a"}}}});
    EXPECT_FALSE(kept("resources/read", json{{"uri", "a"}}));
    EXPECT_TRUE(kept("resources/read", json{{"uri", "b"}}));

    // An update that names no URI drops nothing.
    for (const auto& message : {json::object(), json{{"params", "a"}}, json{{"params", json::object()}},
                                json{{"params", {{"uri", 7}}}}}) {
        cache.invalidate("notifications/resources/updated", message);
        EXPECT_TRUE(kept("resources/read", json{{"uri", "b"}})) << message;
    }

    cache.invalidate("notifications/message", json::object());
    EXPECT_TRUE(kept("prompts/list", std::nullopt));
    cache.invalidate("notifications/prompts/list_changed", json::object());
    EXPECT_FALSE(kept("prompts/list", std::nullopt));
}

TEST(ResponseCacheTest, ANotificationDuringACallKeepsItsResultOut) {
    ResponseCache cache(cache_on());
    const auto key = ResponseCache::key("tools/list", std::nullopt);
    const auto before = cache.generation();
    cache.invalidate("notifications/tools/list_changed", json::object());
    cache.store(key, "tools/list", std::nullopt, json{{"ttlMs", 60000}}, before);
    EXPECT_FALSE(cache.find(key));
}

// ------------------------------------------------------------------------------------------------
// The client

TEST(ClientResponseCache, IsOffUnlessEnabled) {
    Harness harness;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_tools();
        co_await harness.client->list_tools();
    });
    EXPECT_EQ(harness.count("tools/list"), 2U);
}

TEST(ClientResponseCache, AnswersAFreshResultWithoutAskingTheServer) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    std::vector<std::string> texts;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_tools();
        co_await harness.client->list_tools();
        co_await harness.client->list_tools(std::string("2"));
        texts.push_back(text_of(co_await harness.client->read_resource("file:///a")));
        texts.push_back(text_of(co_await harness.client->read_resource("file:///a")));
        texts.push_back(text_of(co_await harness.client->read_resource("file:///b")));
        co_await harness.client->list_resource_templates();
        co_await harness.client->list_resource_templates();
    });
    EXPECT_EQ(harness.count("tools/list"), 2U);
    EXPECT_EQ(harness.count("resources/read"), 2U);
    EXPECT_EQ(harness.count("resources/templates/list"), 1U);
    EXPECT_EQ(texts, (std::vector<std::string>{"v1", "v1", "v2"}));
}

TEST(ClientResponseCache, KeepsNoResultWithoutAPositiveTtl) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_prompts();  // ttlMs 0
        co_await harness.client->list_prompts();
        co_await harness.client->list_resources();  // no ttlMs
        co_await harness.client->list_resources();
    });
    EXPECT_EQ(harness.count("prompts/list"), 2U);
    EXPECT_EQ(harness.count("resources/list"), 2U);
}

TEST(ClientResponseCache, CallsCanRefreshOrBypassTheCache) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    std::vector<std::string> texts;
    harness.run([&]() -> mcp::Task<void> {
        const std::string uri = "file:///a";
        texts.push_back(text_of(co_await harness.client->read_resource(uri)));
        texts.push_back(
            text_of(co_await harness.client->read_resource(uri, with_mode(CacheMode::eRefresh))));
        texts.push_back(text_of(co_await harness.client->read_resource(uri)));
        texts.push_back(
            text_of(co_await harness.client->read_resource(uri, with_mode(CacheMode::eBypass))));
        texts.push_back(text_of(co_await harness.client->read_resource(uri)));
    });
    EXPECT_EQ(harness.count("resources/read"), 3U);
    EXPECT_EQ(texts, (std::vector<std::string>{"v1", "v2", "v2", "v3", "v2"}));
}

TEST(ClientResponseCache, NotificationsMakeTheResultsTheyNameStale) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_tools();
        co_await harness.client->read_resource("file:///a");
        co_await harness.client->read_resource("file:///b");

        harness.deliver(notification("notifications/tools/list_changed"));
        harness.deliver(notification("notifications/resources/updated", {{"uri", "file:///a"}}));
        // Notifications are read in order, so once this one is seen the others were too.
        bool seen = false;
        harness.client->on_notification("notifications/message", [&seen](const json&) { seen = true; });
        harness.deliver(notification("notifications/message", {{"level", "info"}, {"data", "after"}}));
        EXPECT_TRUE(co_await wait_until([&seen] { return seen; }));

        co_await harness.client->list_tools();
        co_await harness.client->read_resource("file:///a");
        co_await harness.client->read_resource("file:///b");
    });
    EXPECT_EQ(harness.count("tools/list"), 2U);
    EXPECT_EQ(harness.count("resources/read"), 3U);
}

TEST(ClientResponseCache, AResultAfterInputRoundsIsNotKept) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    harness.asks_first = {"file:///asks"};
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->read_resource("file:///asks");
        co_await harness.client->read_resource("file:///asks");
    });
    // Each read is two rounds: the question and the retry that carries the request state.
    EXPECT_EQ(harness.count("resources/read"), 4U);
}

TEST(ClientResponseCache, AStaleResultIsFetchedAgain) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    harness.ttl["tools/list"] = 1;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_tools();
        boost::asio::steady_timer pause(harness.io, std::chrono::milliseconds(20));
        co_await pause.async_wait(boost::asio::use_awaitable);
        co_await harness.client->list_tools();
    });
    EXPECT_EQ(harness.count("tools/list"), 2U);
}

TEST(ClientResponseCache, ClearingDropsEveryResult) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_tools();
        co_await harness.client->read_resource("file:///a");
        harness.client->clear_response_cache();
        co_await harness.client->list_tools();
        co_await harness.client->read_resource("file:///a");
    });
    EXPECT_EQ(harness.count("tools/list"), 2U);
    EXPECT_EQ(harness.count("resources/read"), 2U);
}

// caching.mdx: "If a cursor becomes invalid ... the client SHOULD discard all cached pages and
// re-fetch from the beginning."
TEST(ClientResponseCache, ARefusedCursorDropsEveryPageOfItsList) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    harness.refused_cursors = {"stale"};
    std::optional<int> code;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_tools();
        co_await harness.client->list_tools(std::string("2"));
        co_await harness.client->read_resource("file:///a");
        try {
            co_await harness.client->list_tools(std::string("stale"));
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
        co_await harness.client->list_tools();
        co_await harness.client->list_tools(std::string("2"));
        co_await harness.client->read_resource("file:///a");
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_PARAMS);
    // Two pages, the refused one, then both pages again; the read stays kept.
    EXPECT_EQ(harness.count("tools/list"), 5U);
    EXPECT_EQ(harness.count("resources/read"), 1U);
}

TEST(ClientResponseCache, AClosedClientDoesNotAnswerFromTheCache) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    std::optional<int> code;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->list_tools();
        harness.client->close();
        try {
            co_await harness.client->list_tools();
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(harness.count("tools/list"), 1U);
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
}

TEST(ClientResponseCache, ACallBeforeConnectIsRefused) {
    mcp::ClientOptions options;
    options.response_cache = cache_on();
    Harness harness(options);
    std::optional<int> code;
    mcp_test::spawn_detached(harness.io, [&]() -> mcp::Task<void> {
        try {
            co_await harness.client->list_tools();
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    harness.io.run();
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_REQUEST);
    EXPECT_EQ(harness.count("tools/list"), 0U);
}

TEST(ClientResponseCache, RefusesLimitsThatKeepNothing) {
    boost::asio::io_context io;
    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    mcp::ClientOptions no_entries;
    no_entries.response_cache = cache_on(0);
    EXPECT_THROW(mcp::Client(transport, io.get_executor(), no_entries), std::invalid_argument);
    mcp::ClientOptions no_lifetime;
    no_lifetime.response_cache = cache_on();
    no_lifetime.response_cache.max_ttl = std::chrono::milliseconds::zero();
    EXPECT_THROW(mcp::Client(transport, io.get_executor(), no_lifetime), std::invalid_argument);
}

}  // namespace client_response_cache_test
