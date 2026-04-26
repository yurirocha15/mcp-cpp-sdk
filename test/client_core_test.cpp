#include "test_utils.hpp"

#include "mcp/client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace {

nlohmann::json make_initialize_result() {
    return {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
            {"capabilities", nlohmann::json::object()},
            {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}};
}

}  // namespace

class ClientCoreTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ClientCoreTest, SendRequestAddsToMapAndReturnsResult) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                raw_transport->enqueue_message(make_result_response(id, {{"answer", 42}}).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    nlohmann::json result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            nlohmann::json req_params = {{"key", "value"}};
            result = co_await client.send_request("test/method", std::move(req_params));
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result["answer"], 42);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, ConnectHandshakeFollowsMcpOrder) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    mcp::InitializeResult init_result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            init_result = co_await client.connect(std::move(info), mcp::ClientCapabilities{});
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(raw_transport->written().size(), 2);

    auto first = nlohmann::json::parse(raw_transport->written()[0]);
    EXPECT_EQ(first["method"], "initialize");
    EXPECT_TRUE(first.contains("id"));
    EXPECT_EQ(first["jsonrpc"], "2.0");
    EXPECT_EQ(first["params"]["protocolVersion"], std::string(mcp::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(first["params"]["clientInfo"]["name"], "test-client");
    EXPECT_EQ(first["params"]["clientInfo"]["version"], "0.1");

    auto second = nlohmann::json::parse(raw_transport->written()[1]);
    EXPECT_EQ(second["method"], "notifications/initialized");
    EXPECT_FALSE(second.contains("id"));
    EXPECT_EQ(second["jsonrpc"], "2.0");

    EXPECT_EQ(init_result.protocolVersion, std::string(mcp::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(init_result.serverInfo.name, "test-server");
    EXPECT_EQ(init_result.serverInfo.version, "1.0");
}

TEST_F(ClientCoreTest, SendRequestErrorResponseThrows) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id") && json_msg["method"] == "initialize") {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(
                make_error_response(id, mcp::g_METHOD_NOT_FOUND, "method not found").dump());
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    std::exception_ptr captured_ex;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            try {
                co_await client.send_request("nonexistent/method", std::nullopt);
            } catch (...) {
                captured_ex = std::current_exception();
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_NE(captured_ex, nullptr);
    try {
        std::rethrow_exception(captured_ex);
    } catch (const std::runtime_error& err) {
        std::string msg = err.what();
        EXPECT_TRUE(msg.find(std::to_string(mcp::g_METHOD_NOT_FOUND)) != std::string::npos);
        EXPECT_TRUE(msg.find("method not found") != std::string::npos);
    }
}

TEST_F(ClientCoreTest, SendNotificationDoesNotExpectResponse) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            nlohmann::json progress_params = {{"token", "abc"}};
            co_await client.send_notification("notifications/progress", std::move(progress_params));
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(raw_transport->written().size(), 3);
    auto notif = nlohmann::json::parse(raw_transport->written()[2]);
    EXPECT_EQ(notif["method"], "notifications/progress");
    EXPECT_FALSE(notif.contains("id"));
    EXPECT_EQ(notif["params"]["token"], "abc");

    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, MultipleRequestsDispatchCorrectly) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                raw_transport->enqueue_message(
                    make_result_response(id, {{"method_echo", method}}).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    nlohmann::json result_a;
    nlohmann::json result_b;

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result_a = co_await client.send_request("tools/list", std::nullopt);
            result_b = co_await client.send_request("resources/list", std::nullopt);
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result_a["method_echo"], "tools/list");
    EXPECT_EQ(result_b["method_echo"], "resources/list");
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, ReadLoopIgnoresServerNotifications) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                nlohmann::json server_notif = {{"jsonrpc", "2.0"},
                                               {"method", "notifications/tools/list_changed"}};
                raw_transport->enqueue_message(server_notif.dump());
                raw_transport->enqueue_message(make_result_response(id, {{"status", "ok"}}).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    nlohmann::json result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            nlohmann::json call_params = {{"name", "test"}};
            result = co_await client.send_request("tools/call", std::move(call_params));
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result["status"], "ok");
}

TEST_F(ClientCoreTest, PendingRequestCountReflectsInFlightRequests) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    std::size_t count_during_flight = 0;

    mcp::Client client(transport, io_ctx_.get_executor());

    raw_transport->set_on_write([raw_transport, &count_during_flight, &client](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                count_during_flight = client.pending_request_count();
                raw_transport->enqueue_message(
                    make_result_response(id, {{"tools", nlohmann::json::array()}}).dump());
            }
        }
    });

    EXPECT_EQ(client.pending_request_count(), 0);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            co_await client.send_request("tools/list", std::nullopt);

            EXPECT_EQ(client.pending_request_count(), 0);
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(count_during_flight, 1);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, ConnectReturnsServerCapabilities) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    auto init_result_json = make_initialize_result();
    init_result_json["capabilities"]["tools"] = {{"listChanged", true}};
    init_result_json["instructions"] = "Welcome to the test server";

    raw_transport->set_on_write(
        [raw_transport, init_result_json = std::move(init_result_json)](std::string_view msg) {
            auto json_msg = nlohmann::json::parse(msg);
            if (json_msg.contains("id")) {
                auto id = json_msg["id"].get<std::string>();
                raw_transport->enqueue_message(make_result_response(id, init_result_json).dump());
            }
        });

    mcp::Client client(transport, io_ctx_.get_executor());

    mcp::InitializeResult init_result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            init_result = co_await client.connect(std::move(info), mcp::ClientCapabilities{});
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(init_result.protocolVersion, std::string(mcp::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(init_result.serverInfo.name, "test-server");
    ASSERT_TRUE(init_result.instructions.has_value());
    EXPECT_EQ(*init_result.instructions, "Welcome to the test server");
    ASSERT_TRUE(init_result.capabilities.tools.has_value());
    ASSERT_TRUE(init_result.capabilities.tools->listChanged.has_value());
    EXPECT_TRUE(*init_result.capabilities.tools->listChanged);
}

TEST_F(ClientCoreTest, PingSendsRequestAndReceivesResponse) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else if (method == "ping") {
                raw_transport->enqueue_message(
                    make_result_response(id, nlohmann::json::object()).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    bool ping_completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            co_await client.ping();
            ping_completed = true;
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(ping_completed);
    EXPECT_EQ(client.pending_request_count(), 0);
}
