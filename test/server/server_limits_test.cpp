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

// A client at its own limit stops reading until the answers it is writing are read. The server
// must go on reading them while a refusal it made waits to be written, or both wait for good.
TEST_F(ServerLimitsTest, AnswerFromTheClientIsReadWhileARefusalWaitsToBeWritten) {
    std::atomic<bool> answered{false};
    server_->add_tool<json, json>("ask_and_mark", "Asks the client", json{{"type", "object"}},
                                  [this, &answered](json /*args*/) -> mcp::Task<json> {
                                      auto roots =
                                          co_await server_->send_request("roots/list", std::nullopt);
                                      answered.store(true);
                                      co_return roots;
                                  });
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        server_->set_max_in_flight_requests(1);
        transport_->enqueue_message(call(1, "ask_and_mark").dump());
        co_await wait_until([this] {
            return !written_.empty() && written_.back().value("method", "") == "roots/list";
        });
        const auto reverse_id = written_.back().at("id");
        transport_->hold_writes(true);
        transport_->enqueue_message(
            json{{"jsonrpc", "2.0"}, {"id", "refused"}, {"method", "ping"}}.dump());
        transport_->enqueue_message(
            json{{"jsonrpc", "2.0"}, {"id", reverse_id}, {"result", {{"roots", json::array()}}}}
                .dump());
        co_await wait_until([&answered] { return answered.load(); });
        transport_->hold_writes(false);
        co_await wait_until(
            [this] { return answer_to(1) != nullptr && answer_to("refused") != nullptr; });
    });
    EXPECT_TRUE(answered.load());
    ASSERT_NE(answer_to("refused"), nullptr);
    EXPECT_EQ((*answer_to("refused"))["error"]["code"], mcp::g_INTERNAL_ERROR);
    ASSERT_NE(answer_to(1), nullptr);
    EXPECT_EQ((*answer_to(1))["result"]["structuredContent"], (json{{"roots", json::array()}}));
}

// The answer to a message that is not JSON is queued like a refusal, so the client's answer read
// after it reaches the handler while that answer waits to be written.
TEST_F(ServerLimitsTest, AnswerFromTheClientIsReadWhileAParseErrorWaitsToBeWritten) {
    std::atomic<bool> answered{false};
    server_->add_tool<json, json>("ask_and_mark", "Asks the client", json{{"type", "object"}},
                                  [this, &answered](json /*args*/) -> mcp::Task<json> {
                                      auto roots =
                                          co_await server_->send_request("roots/list", std::nullopt);
                                      answered.store(true);
                                      co_return roots;
                                  });
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->enqueue_message(call(1, "ask_and_mark").dump());
        co_await wait_until([this] {
            return !written_.empty() && written_.back().value("method", "") == "roots/list";
        });
        const auto reverse_id = written_.back().at("id");
        transport_->hold_writes(true);
        transport_->enqueue_message("not json");
        transport_->enqueue_message(
            json{{"jsonrpc", "2.0"}, {"id", reverse_id}, {"result", {{"roots", json::array()}}}}
                .dump());
        co_await wait_until([&answered] { return answered.load(); });
        transport_->hold_writes(false);
        co_await wait_until([this] { return answer_to(1) != nullptr; });
    });
    EXPECT_TRUE(answered.load());
    ASSERT_NE(answer_to(1), nullptr);
    EXPECT_EQ((*answer_to(1))["result"]["structuredContent"], (json{{"roots", json::array()}}));
}

// Refusals the read loop makes are queued without waiting only while what is queued stays within
// the byte limit. A client that sends requests and never reads must still stop being read.
TEST_F(ServerLimitsTest, RefusalsPastTheByteLimitStopTheReading) {
    std::size_t unread = 0;
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        server_->set_max_in_flight_requests(1);
        server_->set_max_in_flight_bytes(4096);
        transport_->enqueue_message(call(1, "held").dump());
        co_await wait_until([this] { return running_.load() == 1; });
        transport_->hold_writes(true);
        for (int id = 100; id < 1100; ++id) {
            transport_->enqueue_message(
                json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}}.dump());
        }
        boost::asio::steady_timer settle(io_.get_executor(), std::chrono::milliseconds(100));
        co_await settle.async_wait(boost::asio::use_awaitable);
        unread = transport_->unread();
        release_.store(true);
        transport_->hold_writes(false);
        co_await wait_until([this] { return answer_to(1099) != nullptr; });
    });
    // Refusals are queued while they count within 4096 bytes, each with what it holds besides its
    // text; the rest wait to be read.
    EXPECT_GT(unread, 800U);
    ASSERT_NE(answer_to(1099), nullptr);
}

// A queued answer counts with what the queue holds for it besides its text, so many small parse
// errors stop the reading within the byte limit. Counted by text alone, 1 MiB would hold about
// 14,000 of them; counted with what each holds, about 1,800.
TEST_F(ServerLimitsTest, QueuedParseErrorsCountWhatTheyHold) {
    std::size_t unread = 0;
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        server_->set_max_in_flight_bytes(1024 * 1024);
        transport_->hold_writes(true);
        for (int line = 0; line < 5000; ++line) {
            transport_->enqueue_message("x");
        }
        co_await wait_until([this] { return transport_->unread() < 5000; });
        boost::asio::steady_timer settle(io_.get_executor(), std::chrono::milliseconds(100));
        co_await settle.async_wait(boost::asio::use_awaitable);
        unread = transport_->unread();
        transport_->hold_writes(false);
        co_await wait_until([this] { return transport_->unread() == 0; });
    });
    EXPECT_GT(unread, 2500U);
}

// What a request holds is given back once it is answered, so a session goes on taking requests
// one after another however much they held in all.
TEST_F(ServerLimitsTest, RequestMemoryIsGivenBackWhenAnswered) {
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        server_->set_max_in_flight_bytes(1024 * 1024);
        for (int id = 1; id <= 40; ++id) {
            transport_->enqueue_message(json{{"jsonrpc", "2.0"},
                                             {"id", id},
                                             {"method", "ping"},
                                             {"params", {{"p", json(2000, json::object())}}}}
                                            .dump());
            co_await wait_until([this, id] { return answer_to(id) != nullptr; });
        }
    });
    for (int id = 1; id <= 40; ++id) {
        ASSERT_NE(answer_to(id), nullptr) << id;
        EXPECT_TRUE(answer_to(id)->contains("result")) << *answer_to(id);
    }
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

// An error the client answers with ends the request on its side, whatever its code, so the client
// is not told to cancel it.
TEST_F(ServerLimitsTest, ClientAnsweringWithACancelOrTimeoutIsNotToldToCancel) {
    const std::vector<int> codes{mcp::g_REQUEST_CANCELLED, mcp::g_REQUEST_TIMEOUT};
    std::vector<json> asked;
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        for (std::size_t i = 0; i < codes.size(); ++i) {
            const int id = static_cast<int>(i) + 1;
            transport_->enqueue_message(call(id, "ask").dump());
            co_await wait_until([this, &asked, i] {
                asked.clear();
                for (const auto& message : written_) {
                    if (message.value("method", "") == "roots/list") {
                        asked.push_back(message["id"]);
                    }
                }
                return asked.size() > i;
            });
            transport_->enqueue_message(
                json{{"jsonrpc", "2.0"},
                     {"id", asked[i]},
                     {"error", {{"code", codes[i]}, {"message", "client side"}}}}
                    .dump());
            co_await wait_until([this, id] { return answer_to(id) != nullptr; });
        }
    });
    ASSERT_EQ(asked.size(), codes.size());
    for (int id = 1; id <= 2; ++id) {
        ASSERT_NE(answer_to(id), nullptr) << id;
    }
    for (const auto& message : written_) {
        EXPECT_NE(message.value("method", ""), "notifications/cancelled") << message;
    }
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

// A request is counted as its parsed JSON takes memory: an array of empty objects takes far more than
// its text.
TEST_F(ServerLimitsTest, RequestsAreCountedByTheMemoryTheyTake) {
    server_->set_max_in_flight_bytes(std::size_t{8} * 1024 * 1024);
    const auto padded = [](int id) {
        auto message = call(id, "held");
        message["params"]["arguments"]["p"] = json(40000, json::object());
        return message;
    };
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->enqueue_message(padded(1).dump());
        transport_->enqueue_message(padded(2).dump());
        co_await wait_until([this] { return running_.load() == 2; });
        transport_->enqueue_message(padded(3).dump());
        co_await wait_until([this] { return answer_to(3) != nullptr; });
        release_.store(true);
        co_await wait_until([this] { return answer_to(1) != nullptr && answer_to(2) != nullptr; });
    });
    ASSERT_NE(answer_to(3), nullptr);
    ASSERT_TRUE(answer_to(3)->contains("error")) << *answer_to(3);
    EXPECT_EQ((*answer_to(3))["error"]["code"], mcp::g_INTERNAL_ERROR) << *answer_to(3);
    EXPECT_EQ(running_.load(), 2);
}

// Answers the peer has not taken count too, so a peer that stops reading stops the session from
// starting more work.
TEST_F(ServerLimitsTest, AnswersNotTakenCountAgainstTheMemoryLimit) {
    std::atomic<int> answered{0};
    // The answer carries the value twice, as structuredContent and as text: about 300 KB.
    server_->add_tool<json, json>("big", "Returns 150 KB", json{{"type", "object"}},
                                  [&answered](json /*args*/) -> mcp::Task<json> {
                                      answered.fetch_add(1);
                                      co_return json{{"text", std::string(150000, 'x')}};
                                  });
    server_->set_max_in_flight_bytes(std::size_t{1} * 1024 * 1024);
    int while_held = 0;
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        transport_->hold_writes(true);
        // One at a time, each read once the answer before it waits to be written. Each answer
        // takes about 300 KB, so the fourth request is read with 900 KB waiting, within 1 MiB.
        for (int id = 1; id <= 4; ++id) {
            transport_->enqueue_message(call(id, "big").dump());
            co_await wait_until([&answered, id] { return answered.load() >= id; });
            boost::asio::steady_timer settle(io_.get_executor(), std::chrono::milliseconds(50));
            co_await settle.async_wait(boost::asio::use_awaitable);
        }
        // The fifth finds 1.2 MB waiting: it is refused, and reading stops while that refusal
        // waits behind the answers.
        for (int id = 5; id <= 10; ++id) {
            transport_->enqueue_message(call(id, "big").dump());
        }
        co_await wait_until([this] { return transport_->unread() == 5; });
        boost::asio::steady_timer settle(io_.get_executor(), std::chrono::milliseconds(50));
        co_await settle.async_wait(boost::asio::use_awaitable);
        while_held = answered.load();
        transport_->hold_writes(false);
        co_await wait_until([this] {
            for (int id = 1; id <= 10; ++id) {
                if (answer_to(id) == nullptr) {
                    return false;
                }
            }
            return true;
        });
    });
    EXPECT_EQ(while_held, 4);
}

TEST_F(ServerLimitsTest, SettersRefuseValuesThatCannotWork) {
    EXPECT_THROW(server_->set_max_in_flight_bytes(0), std::invalid_argument);
    EXPECT_THROW(server_->set_max_in_flight_requests(0), std::invalid_argument);
    EXPECT_THROW(server_->set_reverse_request_timeout(std::chrono::milliseconds(0)),
                 std::invalid_argument);
}

// The URIs of a session's subscriptions are limited in bytes as well as in number.
TEST_F(ServerLimitsTest, SubscriptionUrisAreLimitedInBytes) {
    const auto subscribe = [](int id) {
        return json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"method", "resources/subscribe"},
                    {"params", {{"uri", "mem://" + std::to_string(id) + std::string(1048576, 'u')}}}};
    };
    run([&]() -> mcp::Task<void> {
        co_await initialized();
        for (int id = 1; id <= 8; ++id) {
            transport_->enqueue_message(subscribe(id).dump());
            co_await wait_until([this, id] { return answer_to(id) != nullptr; });
        }
        // Dropping one makes room again.
        transport_->enqueue_message(json{{"jsonrpc", "2.0"},
                                         {"id", 20},
                                         {"method", "resources/unsubscribe"},
                                         {"params", {{"uri", subscribe(1)["params"]["uri"]}}}}
                                        .dump());
        transport_->enqueue_message(subscribe(9).dump());
        co_await wait_until([this] { return answer_to(9) != nullptr; });
    });
    for (int id = 1; id <= 7; ++id) {
        ASSERT_NE(answer_to(id), nullptr);
        EXPECT_TRUE(answer_to(id)->contains("result")) << id;
    }
    ASSERT_NE(answer_to(8), nullptr);
    ASSERT_TRUE(answer_to(8)->contains("error"));
    EXPECT_EQ((*answer_to(8))["error"]["code"], mcp::g_INVALID_PARAMS);
    ASSERT_NE(answer_to(9), nullptr);
    EXPECT_TRUE(answer_to(9)->contains("result")) << *answer_to(9);
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
                ++reads_after_close;
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
                json ping{{"jsonrpc", "2.0"}, {"id", pings_read}, {"method", method}};
                if (padding > 0) {
                    ping["params"]["p"] = json(padding, json::object());
                }
                co_return ping.dump();
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
        if (take_answers) {
            ++answers_taken;
            co_return;
        }
        if ((!closed_ || answers_outlast_close) && !held_answers_taken_) {
            boost::system::error_code ignored;
            co_await stalled_.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }
        ++answers_taken;
    }

    void close() override {
        closed_ = true;
        if (!answers_outlast_close) {
            stalled_.cancel();
        }
    }

    /// Lets the answers waiting to be written, and every later one, be written.
    void take_held_answers() {
        held_answers_taken_ = true;
        stalled_.cancel();
    }

    /// Holds the answers written from now on, as before take_held_answers().
    void hold_answers_again() { held_answers_taken_ = false; }

    int pings_read{0};
    /// Answers the client wrote and this peer took.
    int answers_taken{0};
    /// Reads asked of the closed transport.
    int reads_after_close{0};
    /// Whether answers still wait to be written after close(), until take_held_answers().
    bool answers_outlast_close{false};
    /// Whether answers are taken, as a peer that reads does.
    bool take_answers{false};
    /// Empty objects each ping carries: little text, much memory once parsed.
    std::size_t padding{0};
    /// The method of the requests sent after the handshake.
    std::string method{"ping"};

   private:
    boost::asio::steady_timer stalled_;
    std::deque<std::string> replies_;
    bool flooding_{false};
    bool closed_{false};
    bool held_answers_taken_{false};
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

// Requests are counted by the memory they take parsed, not by their text.
TEST(ClientLimits, ServerRequestsAreLimitedByTheMemoryTheyTake) {
    boost::asio::io_context io;
    auto peer = std::make_shared<FloodingPeer>(io.get_executor());
    peer->padding = 20000;
    mcp::ClientOptions options;
    options.max_incoming_request_bytes = std::size_t{4} * 1024 * 1024;
    mcp::Client client(peer, io.get_executor(), options);

    int settled = 0;
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("limits", "1.0");
            co_await wait_until([&] { return peer->pings_read >= 3; });
            boost::asio::steady_timer settle(io.get_executor(), std::chrono::milliseconds(50));
            co_await settle.async_wait(boost::asio::use_awaitable);
            settled = peer->pings_read;
            client.close();
        },
        boost::asio::detached);
    io.run();

    // 20,000 empty objects take more than 1 MiB parsed, and about 60 KB as text.
    EXPECT_GE(settled, 3);
    EXPECT_LE(settled, 4);
}

// A request of the server stops counting once it is answered, so a client whose answers are taken
// keeps reading past the limit.
TEST(ClientLimits, AnsweredRequestsLeaveRoomForMore) {
    boost::asio::io_context io;
    auto peer = std::make_shared<FloodingPeer>(io.get_executor());
    peer->take_answers = true;
    mcp::Client client(peer, io.get_executor());
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("limits", "1.0");
            co_await wait_until([&] { return peer->pings_read >= 3000; });
            client.close();
        },
        boost::asio::detached);
    io.run();
    EXPECT_GE(peer->pings_read, 3000);
}

// A request gives back the memory it held once it is answered, so a client that answered many
// still has the room to read several at once.
TEST(ClientLimits, AnsweredRequestsGiveBackTheirMemory) {
    boost::asio::io_context io;
    auto peer = std::make_shared<FloodingPeer>(io.get_executor());
    peer->padding = 20000;
    mcp::ClientOptions options;
    options.max_incoming_request_bytes = std::size_t{8} * 1024 * 1024;
    mcp::Client client(peer, io.get_executor(), options);
    int unanswered = 0;
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("limits", "1.0");
            co_await wait_until([&] { return peer->pings_read >= 3; });
            peer->take_held_answers();
            // Each ping takes more than 1 MiB parsed, so these hold far more than the limit in all.
            co_await wait_until([&] { return peer->pings_read >= 40; });
            peer->hold_answers_again();
            // Requests read and not yet answered: with the memory given back, several at once.
            co_await wait_until([&] { return peer->pings_read - peer->answers_taken >= 4; });
            unanswered = peer->pings_read - peer->answers_taken;
            client.close();
        },
        boost::asio::detached);
    io.run();
    EXPECT_GE(unanswered, 4);
}

// The client's answers count until they are written, so large answers the server does not take
// stop the client reading, however small the requests.
TEST(ClientLimits, UnwrittenAnswersCountAgainstTheLimit) {
    boost::asio::io_context io;
    auto peer = std::make_shared<FloodingPeer>(io.get_executor());
    peer->method = "test/large";
    mcp::ClientOptions options;
    options.max_incoming_request_bytes = std::size_t{256} * 1024;
    mcp::Client client(peer, io.get_executor(), options);
    client.on_request("test/large", [](const json&) -> mcp::Task<json> {
        co_return json{{"text", std::string(std::size_t{64} * 1024, 'x')}};
    });
    int settled = 0;
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("limits", "1.0");
            co_await wait_until([&] { return peer->pings_read >= 3; });
            boost::asio::steady_timer settle(io.get_executor(), std::chrono::milliseconds(100));
            co_await settle.async_wait(boost::asio::use_awaitable);
            settled = peer->pings_read;
            client.close();
        },
        boost::asio::detached);
    io.run();
    // Four answers of 64 KiB fill the limit. Requests read before their handlers queue an answer
    // add a few; counted by the requests alone, the client would read 1024.
    EXPECT_GE(settled, 3);
    EXPECT_LT(settled, 64);
}

// A client at its limit reads again once its answers are written and its requests end.
TEST(ClientLimits, ReadingResumesWhenRequestsEnd) {
    boost::asio::io_context io;
    auto peer = std::make_shared<FloodingPeer>(io.get_executor());
    mcp::Client client(peer, io.get_executor());
    int at_the_limit = 0;
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("limits", "1.0");
            co_await wait_until([&] { return peer->pings_read >= 1024; });
            boost::asio::steady_timer settle(io.get_executor(), std::chrono::milliseconds(20));
            co_await settle.async_wait(boost::asio::use_awaitable);
            at_the_limit = peer->pings_read;
            peer->take_held_answers();
            co_await wait_until([&] { return peer->pings_read >= 2000; });
            client.close();
        },
        boost::asio::detached);
    io.run();
    EXPECT_EQ(at_the_limit, 1024);
    EXPECT_GE(peer->pings_read, 2000);
}

// The client waits for its answers to be written before it reads more. Closing it ends that wait
// at once, even while the answers are still waiting.
TEST(ClientLimits, CloseEndsTheWaitForRoomToRead) {
    boost::asio::io_context io;
    auto peer = std::make_shared<FloodingPeer>(io.get_executor());
    peer->answers_outlast_close = true;
    mcp::Client client(peer, io.get_executor());
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("limits", "1.0");
            co_await wait_until([&] { return peer->pings_read >= 1024; });
            client.close();
            co_await wait_until([&] { return peer->reads_after_close > 0; });
            peer->take_held_answers();
        },
        boost::asio::detached);
    io.run();
    EXPECT_GT(peer->reads_after_close, 0);
}

TEST(ClientLimits, IncomingRequestMemoryLimitMustBePositive) {
    boost::asio::io_context io;
    mcp::ClientOptions options;
    options.max_incoming_request_bytes = 0;
    EXPECT_THROW(
        mcp::Client(std::make_shared<FloodingPeer>(io.get_executor()), io.get_executor(), options),
        std::invalid_argument);
}
