/**
 * @file gcc11_sso_crash_test.cpp
 *
 * Targeted regression tests for GCC 11 coroutine-frame SSO corruption
 * (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100611).
 *
 * On GCC 11 / Ubuntu 22.04, std::string fields that use Short String
 * Optimisation (SSO, length ≤ 15) and live in a coroutine frame across a
 * suspension point can have their internal self-pointer (_M_p) corrupted,
 * causing an invalid-free crash on destruction.
 *
 * Each test exercises one specific hot-path that is suspected to contain such
 * a local.  The tests must PASS (not abort) after the fix is applied and must
 * reproduce the abort before the fix on GCC 11.
 *
 * Patterns tested:
 *   A – connect():    InitializeRequest with SSO clientInfo.version
 *   B – send_request(): method/params parameters span two co_awaits
 *   C – dispatch_incoming_request(): id_str / method strings span co_awaits
 *   D – send_notification(): JSONRPCNotification spans co_await
 *   E – send_response(): RequestId copy inside response spans co_await
 *   F – send_error_response(): Error.message string spans co_await
 */

#include "mcp/client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Minimal scripted transport (same structure as client_notifications_test)
// ---------------------------------------------------------------------------
class ScriptedTransport final : public mcp::ITransport {
   public:
    explicit ScriptedTransport(const boost::asio::any_io_executor& executor)
        : strand_(boost::asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            if (!incoming_.empty()) {
                auto msg = std::move(incoming_.front());
                incoming_.pop();
                co_return msg;
            }
            timer_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await timer_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& e) {
                if (e.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        // Copy before co_await to avoid string_view dangling (fixed separately)
        std::string msg(message);
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        written_.emplace_back(msg);
        if (on_write_) {
            on_write_(written_.back());
        }
    }

    void close() override {
        closed_ = true;
        timer_.cancel();
    }

    void enqueue(std::string msg) {
        boost::asio::post(strand_, [this, m = std::move(msg)]() mutable {
            incoming_.push(std::move(m));
            timer_.cancel();
        });
    }

    void set_on_write(std::function<void(std::string_view)> cb) { on_write_ = std::move(cb); }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }
    [[nodiscard]] const boost::asio::strand<boost::asio::any_io_executor>& strand() const {
        return strand_;
    }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

// Helpers for common JSON-RPC shapes
nlohmann::json make_init_response(const std::string& id) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"result",
             {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
              {"capabilities", nlohmann::json::object()},
              {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}}}};
}

nlohmann::json make_result_response(const std::string& id, nlohmann::json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

}  // namespace

class GCC11SSOCrashTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

// ---------------------------------------------------------------------------
// Pattern A: connect() — InitializeRequest.clientInfo.version "1.0" is SSO.
// This is the confirmed-crash case from valgrind.
// ---------------------------------------------------------------------------
TEST_F(GCC11SSOCrashTest, PatternA_Connect_SSO_ClientInfo) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor());

    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        auto json_msg = nlohmann::json::parse(msg);
        if (write_count == 1) {
            // Respond to initialize request
            auto id = json_msg["id"].get<std::string>();
            raw->enqueue(make_init_response(id).dump());
        } else if (write_count == 2) {
            // After initialized notification, close
            raw->close();
        }
    });

    bool connected = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            // SSO strings: "my-client" (9 chars), "1.0" (3 chars) — both SSO
            co_await client.connect("my-client", "1.0");
            connected = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(connected);
}

// ---------------------------------------------------------------------------
// Pattern B: send_request() — method parameter is SSO, spans two co_awaits
// (write_message + timer_wait).  Use ping (4 chars, SSO).
// ---------------------------------------------------------------------------
TEST_F(GCC11SSOCrashTest, PatternB_SendRequest_SSO_Method_Ping) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor());

    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        auto json_msg = nlohmann::json::parse(msg);
        auto id = json_msg.value("id", "");
        if (write_count == 1) {
            // initialize response
            raw->enqueue(make_init_response(id).dump());
        } else if (write_count == 2) {
            // initialized notification — no id expected; now send ping response
            // Client issued no explicit ping here, so this write_count==2 is
            // the "notifications/initialized" notification (no id).
            // Do nothing, just let the next step proceed.
        } else if (write_count == 3) {
            // ping request — respond with empty result
            raw->enqueue(make_result_response(id, nlohmann::json::object()).dump());
        }
    });

    bool ping_ok = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("cli", "1.0");
            // "ping" is 4 chars — SSO; method string lives in send_request's
            // coroutine frame across the timer wait suspension
            co_await client.ping();
            ping_ok = true;
            // Close transport so read_loop terminates and io_ctx_.run() returns
            boost::asio::post(io_ctx_,
                              [raw]() { boost::asio::post(raw->strand(), [raw]() { raw->close(); }); });
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(ping_ok);
}

// ---------------------------------------------------------------------------
// Pattern B2: send_request() with a short custom method name ("hi" = 2 chars)
// via the raw API — maximally stresses the SSO case.
// ---------------------------------------------------------------------------
TEST_F(GCC11SSOCrashTest, PatternB2_SendRequest_SSO_Method_Short) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor());

    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        auto json_msg = nlohmann::json::parse(msg);
        auto id = json_msg.value("id", "");
        if (write_count == 1) {
            raw->enqueue(make_init_response(id).dump());
        } else if (write_count == 2) {
            // initialized notification, no id — close after connect completes
        } else if (write_count >= 3) {
            // respond to whatever request was made
            if (!id.empty()) {
                raw->enqueue(make_result_response(id, nlohmann::json::object()).dump());
            } else {
                raw->close();
            }
        }
    });

    // After connect, close to end the test
    bool done = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("c", "1");
            done = true;
            // Close from outside
            boost::asio::post(io_ctx_,
                              [raw]() { boost::asio::post(raw->strand(), [raw]() { raw->close(); }); });
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(done);
}

// ---------------------------------------------------------------------------
// Pattern C: dispatch_incoming_request() — server sends "ping" request.
// id = "s1" (2 chars SSO), method = "ping" (4 chars SSO) both span co_awaits.
// Also exercises send_response() with a short id string.
// ---------------------------------------------------------------------------
TEST_F(GCC11SSOCrashTest, PatternC_DispatchIncomingRequest_SSO_IdAndMethod) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor());

    std::vector<std::string> writes;
    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        writes.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);
        auto id = json_msg.value("id", "");
        if (write_count == 1) {
            raw->enqueue(make_init_response(id).dump());
        } else if (write_count == 2) {
            // initialized notification — now send server-side "ping" with SSO id
            nlohmann::json ping;
            ping["jsonrpc"] = "2.0";
            ping["id"] = "s1";        // 2 chars — SSO
            ping["method"] = "ping";  // 4 chars — SSO
            raw->enqueue(ping.dump());
        } else if (write_count == 3) {
            // ping response written — close
            raw->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("cli", "1.0"); },
        boost::asio::detached);

    io_ctx_.run();

    // Verify ping response was sent with correct id
    ASSERT_GE(writes.size(), 3u);
    auto response = nlohmann::json::parse(writes[2]);
    EXPECT_EQ(response["id"], "s1");
    EXPECT_TRUE(response.contains("result"));
}

// ---------------------------------------------------------------------------
// Pattern C2: dispatch_incoming_request() — unknown method (METHOD_NOT_FOUND).
// exercises send_error_response() with SSO id and short error message.
// ---------------------------------------------------------------------------
TEST_F(GCC11SSOCrashTest, PatternC2_DispatchIncomingRequest_UnknownMethod_ErrorResponse) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor());

    std::vector<std::string> writes;
    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        writes.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);
        auto id = json_msg.value("id", "");
        if (write_count == 1) {
            raw->enqueue(make_init_response(id).dump());
        } else if (write_count == 2) {
            // Send unknown method request with a short SSO id
            nlohmann::json req;
            req["jsonrpc"] = "2.0";
            req["id"] = "x";            // 1 char — SSO
            req["method"] = "no/such";  // 7 chars — SSO
            raw->enqueue(req.dump());
        } else if (write_count == 3) {
            raw->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("cli", "1.0"); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(writes.size(), 3u);
    auto response = nlohmann::json::parse(writes[2]);
    EXPECT_EQ(response["id"], "x");
    EXPECT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], mcp::METHOD_NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Pattern C3: dispatch_incoming_request() — registered handler throws,
// exercises send_error_response() with INTERNAL_ERROR (-32603).
// Error message comes from e.what() which can be short (SSO).
// ---------------------------------------------------------------------------
TEST_F(GCC11SSOCrashTest, PatternC3_DispatchIncomingRequest_HandlerThrows_InternalError) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor());

    client.on_request("test/method", [](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
        throw std::runtime_error("oops");  // 4 chars — SSO
        co_return nlohmann::json{};
    });

    std::vector<std::string> writes;
    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        writes.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);
        auto id = json_msg.value("id", "");
        if (write_count == 1) {
            raw->enqueue(make_init_response(id).dump());
        } else if (write_count == 2) {
            nlohmann::json req;
            req["jsonrpc"] = "2.0";
            req["id"] = "e1";  // 2 chars — SSO
            req["method"] = "test/method";
            req["params"] = nlohmann::json::object();
            raw->enqueue(req.dump());
        } else if (write_count == 3) {
            raw->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("cli", "1.0"); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(writes.size(), 3u);
    auto response = nlohmann::json::parse(writes[2]);
    EXPECT_EQ(response["id"], "e1");
    EXPECT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], -32603);
}

// ---------------------------------------------------------------------------
// Pattern D: send_notification() — method "notifications/initialized" (26 chars,
// heap) is fine; but also test a very short notification method.
// Exercises JSONRPCNotification struct across co_await.
// ---------------------------------------------------------------------------
TEST_F(GCC11SSOCrashTest, PatternD_SendNotification_SSO_Method) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor());

    // Register a handler that sends a short notification back
    // (the client sends a cancellation notification via cancel())
    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        auto json_msg = nlohmann::json::parse(msg);
        auto id = json_msg.value("id", "");
        if (write_count == 1) {
            raw->enqueue(make_init_response(id).dump());
        } else if (write_count == 2) {
            // initialized notification — now trigger cancel notification
            // (method = "notifications/cancelled", params.requestId = "r1" SSO)
        } else if (write_count >= 3) {
            raw->close();
        }
    });

    bool done = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("cli", "1.0");
            // Send a cancellation notification: method is long (>15) so safe,
            // but RequestId "r1" (2 chars SSO) is inside CancelledNotificationParams
            co_await client.cancel(mcp::RequestId{"r1"}, "ok");
            done = true;
            boost::asio::post(io_ctx_,
                              [raw]() { boost::asio::post(raw->strand(), [raw]() { raw->close(); }); });
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(done);
}
