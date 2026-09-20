#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

}  // namespace

class HttpTransportTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

TEST_F(HttpTransportTest, PortReturnsEphemeralPortWhenBoundToZero) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 0);

    EXPECT_GT(server_transport.port(), 0);

    server_transport.close();
}

TEST_F(HttpTransportTest, PendingServerReadOwnsImplementationAfterTransportDestruction) {
    auto server = std::make_unique<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    auto read = server->read_message();
    server.reset();

    std::exception_ptr read_error;
    asio::co_spawn(io_ctx_, std::move(read),
                   [&read_error](std::exception_ptr error, std::string) { read_error = error; });
    io_ctx_.run();

    EXPECT_NE(read_error, nullptr);
}

TEST_F(HttpTransportTest, SendMessageServerReceives) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18080);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    std::string server_received;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { server_received = co_await server_transport.read_message(); },
        asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18080/mcp");

            // Send notification (no id) so server returns 202 immediately without blocking
            nlohmann::json notification_payload = {{"jsonrpc", "2.0"},
                                                   {"method", "notifications/initialized"}};
            co_await client_transport.write_message(notification_payload.dump());

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    nlohmann::json expected_notification = {{"jsonrpc", "2.0"},
                                            {"method", "notifications/initialized"}};
    EXPECT_EQ(nlohmann::json::parse(server_received), expected_notification);
}

TEST_F(HttpTransportTest, ServerWriteClientReceives) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18081);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto request_message = co_await server_transport.read_message();
            nlohmann::json response_payload = {{"jsonrpc", "2.0"}, {"result", "pong"}, {"id", 1}};
            co_await server_transport.write_message(response_payload.dump());
        },
        asio::detached);

    std::string client_received;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18081/mcp");

            nlohmann::json request_payload = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 1}};
            co_await client_transport.write_message(request_payload.dump());
            client_received = co_await client_transport.read_message();

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    nlohmann::json expected_response = {{"jsonrpc", "2.0"}, {"result", "pong"}, {"id", 1}};
    EXPECT_EQ(nlohmann::json::parse(client_received), expected_response);
}

TEST_F(HttpTransportTest, FullMcpHandshake) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18082);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    std::string server_session_id;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto initialize_request = co_await server_transport.read_message();
            auto request_json = nlohmann::json::parse(initialize_request);
            EXPECT_EQ(request_json.at("method"), "initialize");

            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", request_json.at("id")}};

            co_await server_transport.write_message(initialize_response.dump());
        },
        asio::detached);

    std::string client_received_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18082/mcp");

            nlohmann::json initialize_request = {
                {"jsonrpc", "2.0"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", 1}};

            co_await client_transport.write_message(initialize_request.dump());
            client_received_response = co_await client_transport.read_message();

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    auto response_json = nlohmann::json::parse(client_received_response);
    EXPECT_TRUE(response_json.contains("result"));
    EXPECT_TRUE(response_json.at("result").contains("protocolVersion"));
    EXPECT_EQ(response_json.at("result").at("protocolVersion"),
              std::string(mcp::g_LATEST_PROTOCOL_VERSION));
}

TEST_F(HttpTransportTest, SessionIdPropagation) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18083);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    bool second_request_received = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            // First request: initialize
            auto initialize_request = co_await server_transport.read_message();
            auto init_json = nlohmann::json::parse(initialize_request);

            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", init_json.at("id")}};

            co_await server_transport.write_message(initialize_response.dump());

            // Second request: subsequent call (should have session ID)
            auto second_request = co_await server_transport.read_message();
            second_request_received = true;

            nlohmann::json second_response = {{"jsonrpc", "2.0"},
                                              {"result", "ok"},
                                              {"id", nlohmann::json::parse(second_request).at("id")}};
            co_await server_transport.write_message(second_response.dump());
        },
        asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18083/mcp");

            // Initialize to establish session
            nlohmann::json initialize_request = {
                {"jsonrpc", "2.0"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", 1}};

            co_await client_transport.write_message(initialize_request.dump());
            co_await client_transport.read_message();

            // Subsequent request (client should include session ID automatically)
            nlohmann::json subsequent_request = {
                {"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", 2}};
            co_await client_transport.write_message(subsequent_request.dump());
            co_await client_transport.read_message();

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(second_request_received);
}

TEST_F(HttpTransportTest, ClientCloseDeletesSentToServer) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18084);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    bool session_terminated = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto initialize_request = co_await server_transport.read_message();
            auto init_json = nlohmann::json::parse(initialize_request);

            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", init_json.at("id")}};

            co_await server_transport.write_message(initialize_response.dump());
            session_terminated = true;
        },
        asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18084/mcp");

            nlohmann::json initialize_request = {
                {"jsonrpc", "2.0"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", 1}};

            co_await client_transport.write_message(initialize_request.dump());
            co_await client_transport.read_message();

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(session_terminated);
}

TEST_F(HttpTransportTest, ServerRejectsInvalidProtocolVersion) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18085);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    bool client_received_error = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                beast::tcp_stream stream(io_ctx_.get_executor());
                auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
                auto endpoints =
                    co_await resolver.async_resolve("127.0.0.1", "18085", asio::use_awaitable);
                co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

                http::request<http::string_body> request{http::verb::post, "/mcp", 11};
                request.set(http::field::host, "127.0.0.1");
                request.set(http::field::content_type, "application/json");
                request.set("MCP-Protocol-Version", "0000-00-00");
                request.body() = R"({"jsonrpc":"2.0","method":"ping","id":1})";
                request.prepare_payload();

                co_await http::async_write(stream, request, asio::use_awaitable);

                beast::flat_buffer response_buffer;
                http::response<http::string_body> response;
                co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

                if (response.result() == http::status::bad_request) {
                    client_received_error = true;
                }

                beast::error_code shutdown_error;
                stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
            } catch (...) {
            }
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(client_received_error);
}

TEST_F(HttpTransportTest, NotificationReturns202) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18086);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto notification_message = co_await server_transport.read_message();
            // Server reads notification but doesn't respond
        },
        asio::detached);

    bool received_202 = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                beast::tcp_stream stream(io_ctx_.get_executor());
                auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
                auto endpoints =
                    co_await resolver.async_resolve("127.0.0.1", "18086", asio::use_awaitable);
                co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

                http::request<http::string_body> request{http::verb::post, "/mcp", 11};
                request.set(http::field::host, "127.0.0.1");
                request.set(http::field::content_type, "application/json");
                request.set("MCP-Protocol-Version", std::string(mcp::g_LATEST_PROTOCOL_VERSION));
                // Notification: has method but no id
                request.body() = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
                request.prepare_payload();

                co_await http::async_write(stream, request, asio::use_awaitable);

                beast::flat_buffer response_buffer;
                http::response<http::string_body> response;
                co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

                if (response.result() == http::status::accepted) {
                    received_202 = true;
                }

                beast::error_code shutdown_error;
                stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
            } catch (...) {
            }
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(received_202);
}

TEST_F(HttpTransportTest, SessionRequestsAllowMissingNegotiatedProtocolHeader) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18087);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    bool tool_request_received = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto initialize_request = co_await server_transport.read_message();
            auto init_json = nlohmann::json::parse(initialize_request);

            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", "2025-06-18"},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", init_json.at("id")}};

            co_await server_transport.write_message(initialize_response.dump());

            auto second_request = co_await server_transport.read_message();
            tool_request_received = true;
            nlohmann::json second_response = {{"jsonrpc", "2.0"},
                                              {"result", {{"content", nlohmann::json::array()}}},
                                              {"id", nlohmann::json::parse(second_request).at("id")}};
            co_await server_transport.write_message(second_response.dump());
        },
        asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            beast::tcp_stream stream(io_ctx_.get_executor());
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            auto endpoints = co_await resolver.async_resolve("127.0.0.1", "18087", asio::use_awaitable);
            co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> init_request{http::verb::post, "/mcp", 11};
            init_request.set(http::field::host, "127.0.0.1");
            init_request.set(http::field::content_type, "application/json");
            init_request.set(http::field::accept, "application/json, text/event-stream");
            init_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test-client","version":"1.0.0"},"capabilities":{}}})";
            init_request.prepare_payload();
            co_await http::async_write(stream, init_request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> init_response;
            co_await http::async_read(stream, response_buffer, init_response, asio::use_awaitable);

            const auto session_id = std::string(init_response["MCP-Session-Id"]);

            http::request<http::string_body> tool_request{http::verb::post, "/mcp", 11};
            tool_request.set(http::field::host, "127.0.0.1");
            tool_request.set(http::field::content_type, "application/json");
            tool_request.set(http::field::accept, "application/json, text/event-stream");
            tool_request.set("MCP-Session-Id", session_id);
            tool_request.body() = R"({"jsonrpc":"2.0","method":"tools/list","id":2})";
            tool_request.prepare_payload();
            co_await http::async_write(stream, tool_request, asio::use_awaitable);

            http::response<http::string_body> tool_response;
            co_await http::async_read(stream, response_buffer, tool_response, asio::use_awaitable);

            EXPECT_EQ(tool_response.result(), http::status::ok);

            beast::error_code shutdown_error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(tool_request_received);
}

TEST_F(HttpTransportTest, ClientUsesNegotiatedProtocolVersionForSessionRequests) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18088);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    bool second_request_received = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto initialize_request = co_await server_transport.read_message();
            const auto initialize_json = nlohmann::json::parse(initialize_request);

            const nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", "2025-06-18"},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", initialize_json.at("id")}};
            co_await server_transport.write_message(initialize_response.dump());

            const auto second_request = co_await server_transport.read_message();
            second_request_received = true;
            const nlohmann::json second_response = {
                {"jsonrpc", "2.0"},
                {"result", nlohmann::json::object()},
                {"id", nlohmann::json::parse(second_request).at("id")}};
            co_await server_transport.write_message(second_response.dump());
        },
        asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18088/mcp");
            const nlohmann::json initialize_request = {
                {"jsonrpc", "2.0"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", "2025-06-18"},
                  {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", 1}};

            co_await client_transport.write_message(initialize_request.dump());
            co_await client_transport.read_message();

            const nlohmann::json subsequent_request = {
                {"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", 2}};
            co_await client_transport.write_message(subsequent_request.dump());
            co_await client_transport.read_message();

            client_transport.close();
        },
        [&server_transport](std::exception_ptr error) {
            EXPECT_EQ(error, nullptr);
            server_transport.close();
        });

    io_ctx_.run();

    EXPECT_TRUE(second_request_received);
}

TEST_F(HttpTransportTest, ClientCloseCancelsInFlightPostAfterSessionInitialization) {
    struct TestState {
        bool initialized{false};
        bool post_received{false};
        bool write_completed{false};
        bool write_cancelled{false};
        bool cleanup_completed{false};
        std::exception_ptr server_error;
        std::exception_ptr client_error;
    };

    auto server_transport =
        std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 18089);
    auto client_transport = std::make_shared<mcp::HttpClientTransport>(io_ctx_.get_executor(),
                                                                       "http://127.0.0.1:18089/mcp");
    auto state = std::make_shared<TestState>();
    auto post_received = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    auto write_completed = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    post_received->expires_at(std::chrono::steady_clock::time_point::max());
    write_completed->expires_at(std::chrono::steady_clock::time_point::max());

    asio::co_spawn(io_ctx_, server_transport->listen(), asio::detached);
    asio::co_spawn(
        io_ctx_,
        [](std::shared_ptr<mcp::HttpServerTransport> server, std::shared_ptr<TestState> test_state,
           std::shared_ptr<asio::steady_timer> post_signal) -> mcp::Task<void> {
            const auto initialize_message = co_await server->read_message();
            const auto initialize_request = nlohmann::json::parse(initialize_message);
            const nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", initialize_request.at("id")}};
            co_await server->write_message(initialize_response.dump());

            const auto initialized_message = co_await server->read_message();
            EXPECT_EQ(nlohmann::json::parse(initialized_message).at("method"),
                      "notifications/initialized");

            const auto pending_message = co_await server->read_message();
            EXPECT_EQ(nlohmann::json::parse(pending_message).at("method"), "tools/list");
            test_state->post_received = true;
            post_signal->cancel();
        }(server_transport, state, post_received),
        [state, post_received, server_transport](std::exception_ptr error) {
            state->server_error = error;
            if (error) {
                post_received->cancel();
                server_transport->close();
            }
        });

    asio::co_spawn(
        io_ctx_,
        [](std::shared_ptr<mcp::HttpClientTransport> client,
           std::shared_ptr<mcp::HttpServerTransport> server, std::shared_ptr<TestState> test_state,
           std::shared_ptr<asio::steady_timer> post_signal,
           std::shared_ptr<asio::steady_timer> write_signal) -> mcp::Task<void> {
            const nlohmann::json initialize_request = {
                {"jsonrpc", "2.0"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", 1}};
            co_await client->write_message(initialize_request.dump());
            const auto initialize_response = nlohmann::json::parse(co_await client->read_message());
            EXPECT_TRUE(initialize_response.contains("result"));
            EXPECT_FALSE(client->session_id().empty());
            test_state->initialized = true;

            const nlohmann::json initialized_notification = {{"jsonrpc", "2.0"},
                                                             {"method", "notifications/initialized"}};
            co_await client->write_message(initialized_notification.dump());

            const nlohmann::json pending_request = {
                {"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", 2}};
            asio::co_spawn(write_signal->get_executor(), client->write_message(pending_request.dump()),
                           [client, test_state, write_signal](std::exception_ptr error) {
                               test_state->write_completed = true;
                               if (error) {
                                   try {
                                       std::rethrow_exception(error);
                                   } catch (const boost::system::system_error& system_error) {
                                       test_state->write_cancelled =
                                           system_error.code() == asio::error::operation_aborted;
                                   } catch (...) {
                                   }
                               }
                               write_signal->cancel();
                           });

            if (!test_state->post_received) {
                try {
                    co_await post_signal->async_wait(asio::use_awaitable);
                } catch (const boost::system::system_error& error) {
                    if (error.code() != asio::error::operation_aborted) {
                        throw;
                    }
                }
            }
            if (test_state->server_error) {
                std::rethrow_exception(test_state->server_error);
            }

            client->close();
            if (!test_state->write_completed) {
                try {
                    co_await write_signal->async_wait(asio::use_awaitable);
                } catch (const boost::system::system_error& error) {
                    if (error.code() != asio::error::operation_aborted) {
                        throw;
                    }
                }
            }

            asio::steady_timer cleanup_poll(write_signal->get_executor());
            while (!client->session_id().empty()) {
                cleanup_poll.expires_after(std::chrono::milliseconds(1));
                co_await cleanup_poll.async_wait(asio::use_awaitable);
            }

            server->close();
            test_state->cleanup_completed = true;
        }(client_transport, server_transport, state, post_received, write_completed),
        [state, client_transport, server_transport](std::exception_ptr error) {
            state->client_error = error;
            if (error) {
                client_transport->close();
                server_transport->close();
            }
        });

    // Bound the test even if cancellation or listener cleanup regresses.
    io_ctx_.run_for(std::chrono::seconds(5));
    const bool event_loop_drained = io_ctx_.stopped();
    if (!event_loop_drained) {
        client_transport->close();
        server_transport->close();
        io_ctx_.stop();
    }

    EXPECT_EQ(state->server_error, nullptr);
    EXPECT_EQ(state->client_error, nullptr);
    EXPECT_TRUE(state->initialized);
    EXPECT_TRUE(state->post_received);
    EXPECT_TRUE(state->write_completed);
    EXPECT_TRUE(state->write_cancelled);
    EXPECT_TRUE(state->cleanup_completed);
    EXPECT_TRUE(event_loop_drained);
}

TEST_F(HttpTransportTest, PendingOperationsOwnImplementationAfterTransportDestruction) {
    auto transport = std::make_unique<mcp::HttpClientTransport>(io_ctx_.get_executor(),
                                                                "http://127.0.0.1:18111/mcp");
    const nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};

    auto write = transport->write_message(notification.dump());
    auto read = transport->read_message();
    transport.reset();

    std::exception_ptr write_error;
    std::exception_ptr read_error;
    asio::co_spawn(io_ctx_, std::move(write),
                   [&write_error](std::exception_ptr error) { write_error = error; });
    asio::co_spawn(io_ctx_, std::move(read),
                   [&read_error](std::exception_ptr error, std::string) { read_error = error; });

    io_ctx_.run();

    EXPECT_NE(write_error, nullptr);
    EXPECT_NE(read_error, nullptr);
}

TEST_F(HttpTransportTest, SerializesConcurrentWritesAcrossTwoIoThreads) {
    constexpr int message_count = 64;
    constexpr auto timeout = std::chrono::seconds(5);

    asio::io_context server_io;
    auto server =
        std::make_shared<mcp::HttpServerTransport>(server_io.get_executor(), "127.0.0.1", 18110);
    auto server_deadline = std::make_shared<asio::steady_timer>(server_io.get_executor());
    server_deadline->expires_after(timeout);
    auto received = std::make_shared<std::vector<bool>>(message_count, false);
    std::exception_ptr server_error;

    asio::co_spawn(server_io, server->listen(), asio::detached);
    asio::co_spawn(
        server_io,
        [server, server_deadline, received]() -> mcp::Task<void> {
            for (int index = 0; index < message_count; ++index) {
                const auto message = nlohmann::json::parse(co_await server->read_message());
                if (message.at("method") != "notifications/test" || !message.contains("params") ||
                    !message.at("params").contains("sequence")) {
                    throw std::runtime_error("Unexpected concurrent-write test message");
                }
                const auto sequence = message.at("params").at("sequence").get<int>();
                if (sequence < 0 || sequence >= message_count || (*received)[sequence]) {
                    throw std::runtime_error("Invalid or duplicate concurrent-write sequence");
                }
                (*received)[sequence] = true;
            }
            server_deadline->cancel();
            server->close();
        },
        [&server_error, server](std::exception_ptr error) {
            server_error = error;
            server->close();
        });
    server_deadline->async_wait([server](const boost::system::error_code& error) {
        if (!error) {
            server->close();
        }
    });
    std::thread server_thread([&server_io] { server_io.run(); });

    auto client = std::make_shared<mcp::HttpClientTransport>(io_ctx_.get_executor(),
                                                             "http://127.0.0.1:18110/mcp");
    auto coordination_strand = asio::make_strand(io_ctx_);
    auto remaining = std::make_shared<std::atomic<int>>(message_count);
    auto completion_signal = std::make_shared<asio::steady_timer>(coordination_strand);
    completion_signal->expires_at(std::chrono::steady_clock::time_point::max());
    auto client_deadline = std::make_shared<asio::steady_timer>(coordination_strand);
    client_deadline->expires_after(timeout);
    auto errors = std::make_shared<std::vector<std::exception_ptr>>();
    auto errors_mutex = std::make_shared<std::mutex>();

    for (int index = 0; index < message_count; ++index) {
        const nlohmann::json notification = {
            {"jsonrpc", "2.0"}, {"method", "notifications/test"}, {"params", {{"sequence", index}}}};
        asio::co_spawn(io_ctx_, client->write_message(notification.dump()),
                       [remaining, completion_signal, errors, errors_mutex](std::exception_ptr error) {
                           if (error) {
                               std::lock_guard lock(*errors_mutex);
                               errors->push_back(error);
                           }
                           if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                               asio::post(completion_signal->get_executor(),
                                          [completion_signal] { completion_signal->cancel(); });
                           }
                       });
    }

    asio::co_spawn(
        coordination_strand,
        [client, completion_signal, client_deadline]() -> mcp::Task<void> {
            try {
                co_await completion_signal->async_wait(asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != asio::error::operation_aborted) {
                    throw;
                }
            }
            client_deadline->cancel();
            client->close();
        },
        [client](std::exception_ptr) { client->close(); });
    client_deadline->async_wait([client, completion_signal](const boost::system::error_code& error) {
        if (!error) {
            client->close();
            completion_signal->cancel();
        }
    });

    std::thread first_client_thread([this] { io_ctx_.run(); });
    std::thread second_client_thread([this] { io_ctx_.run(); });
    first_client_thread.join();
    second_client_thread.join();
    server_thread.join();

    EXPECT_EQ(server_error, nullptr);
    EXPECT_TRUE(errors->empty());
    EXPECT_EQ(remaining->load(std::memory_order_acquire), 0);
    EXPECT_TRUE(std::all_of(received->begin(), received->end(), [](bool value) { return value; }));
}

TEST_F(HttpTransportTest, CloseCancelsIdleKeepAliveAndRejectsFurtherRequests) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    auto client = std::make_shared<beast::tcp_stream>(io_ctx_.get_executor());
    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(2));

    std::atomic<int> authorization_calls{0};
    server->set_bearer_token_validator([&authorization_calls](std::string_view token) {
        authorization_calls.fetch_add(1, std::memory_order_relaxed);
        return token == "valid-token";
    });

    bool first_request_completed = false;
    bool connection_cancelled = false;
    bool post_close_response_received = false;
    bool timed_out = false;
    std::exception_ptr client_error;

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);
    asio::co_spawn(
        io_ctx_,
        [server, client, deadline, &first_request_completed, &connection_cancelled,
         &post_close_response_received]() -> mcp::Task<void> {
            asio::ip::tcp::resolver resolver(client->get_executor());
            auto endpoints = co_await resolver.async_resolve(
                "127.0.0.1", std::to_string(server->port()), asio::use_awaitable);
            co_await client->async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> first_request{http::verb::post, "/mcp", 11};
            first_request.set(http::field::host, "127.0.0.1");
            first_request.set(http::field::authorization, "Bearer valid-token");
            first_request.set(http::field::content_type, "application/json");
            first_request.keep_alive(true);
            first_request.body() = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
            first_request.prepare_payload();
            co_await http::async_write(*client, first_request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> first_response;
            co_await http::async_read(*client, response_buffer, first_response, asio::use_awaitable);
            first_request_completed = first_response.result() == http::status::accepted;

            server->close();

            http::request<http::string_body> second_request{http::verb::post, "/mcp", 11};
            second_request.set(http::field::host, "127.0.0.1");
            second_request.set(http::field::authorization, "Bearer valid-token");
            second_request.set(http::field::content_type, "application/json");
            second_request.keep_alive(true);
            second_request.body() = R"({"jsonrpc":"2.0","method":"notifications/post-close"})";
            second_request.prepare_payload();

            try {
                co_await http::async_write(*client, second_request, asio::use_awaitable);
                http::response<http::string_body> second_response;
                co_await http::async_read(*client, response_buffer, second_response,
                                          asio::use_awaitable);
                post_close_response_received = true;
            } catch (const boost::system::system_error&) {
                connection_cancelled = true;
            }
            deadline->cancel();
        },
        [&client_error, deadline, server](std::exception_ptr error) {
            client_error = std::move(error);
            deadline->cancel();
            server->close();
        });
    deadline->async_wait([client, server, &timed_out](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server->close();
        boost::system::error_code ignored;
        (void)client->socket().close(ignored);
    });

    io_ctx_.run();

    EXPECT_EQ(client_error, nullptr);
    EXPECT_FALSE(timed_out);
    EXPECT_TRUE(first_request_completed);
    EXPECT_TRUE(connection_cancelled);
    EXPECT_FALSE(post_close_response_received);
    EXPECT_EQ(authorization_calls.load(std::memory_order_relaxed), 1);
}

TEST_F(HttpTransportTest, SessionlessDiscoverSucceedsAfterSessionEstablished) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18200);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(10));
    bool timed_out = false;

    bool discover_request_received = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto initialize_request = co_await server_transport.read_message();
            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", nlohmann::json::object()}}},
                {"id", nlohmann::json::parse(initialize_request).at("id")}};
            co_await server_transport.write_message(initialize_response.dump());

            const auto discover_request = co_await server_transport.read_message();
            discover_request_received = true;
            nlohmann::json discover_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"supportedVersions",
                   nlohmann::json::array({std::string(mcp::g_PROTOCOL_VERSION_2026_07_28)})}}},
                {"id", nlohmann::json::parse(discover_request).at("id")}};
            co_await server_transport.write_message(discover_response.dump());
        },
        [](std::exception_ptr) {});

    auto discover_status = http::status::unknown;
    std::string discover_body;
    std::string established_session_id;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18200", asio::use_awaitable);

            beast::tcp_stream init_stream(io_ctx_.get_executor());
            co_await init_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> init_request{http::verb::post, "/mcp", 11};
            init_request.set(http::field::host, "127.0.0.1");
            init_request.set(http::field::content_type, "application/json");
            init_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":")" +
                std::string(mcp::g_LATEST_PROTOCOL_VERSION) +
                R"(","clientInfo":{"name":"test-client","version":"1.0.0"},"capabilities":{}}})";
            init_request.prepare_payload();
            co_await http::async_write(init_stream, init_request, asio::use_awaitable);

            beast::flat_buffer init_buffer;
            http::response<http::string_body> init_response;
            co_await http::async_read(init_stream, init_buffer, init_response, asio::use_awaitable);
            established_session_id = std::string(init_response["MCP-Session-Id"]);

            beast::error_code init_shutdown_error;
            init_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, init_shutdown_error);

            // Fresh connection, no MCP-Session-Id header: server/discover must stay reachable
            // even though the transport now holds an established session.
            beast::tcp_stream discover_stream(io_ctx_.get_executor());
            co_await discover_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> discover_request{http::verb::post, "/mcp", 11};
            discover_request.set(http::field::host, "127.0.0.1");
            discover_request.set(http::field::content_type, "application/json");
            discover_request.body() = R"({"jsonrpc":"2.0","method":"server/discover","id":2})";
            discover_request.prepare_payload();
            co_await http::async_write(discover_stream, discover_request, asio::use_awaitable);

            beast::flat_buffer discover_buffer;
            http::response<http::string_body> discover_response;
            co_await http::async_read(discover_stream, discover_buffer, discover_response,
                                      asio::use_awaitable);
            discover_status = discover_response.result();
            discover_body = discover_response.body();

            beast::error_code discover_shutdown_error;
            discover_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both,
                                              discover_shutdown_error);

            deadline->cancel();
            server_transport.close();
        },
        [](std::exception_ptr) {});

    deadline->async_wait([&timed_out, &server_transport](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server_transport.close();
    });

    io_ctx_.run();

    EXPECT_FALSE(timed_out);
    EXPECT_FALSE(established_session_id.empty());
    EXPECT_TRUE(discover_request_received);
    EXPECT_EQ(discover_status, http::status::ok) << "body: " << discover_body;
    // The caller must get its own id back. A sessionless request is carried internally under a
    // transport-private id so it cannot squat the session's id space; that is an implementation
    // detail the peer must never see.
    EXPECT_EQ(nlohmann::json::parse(discover_body).at("id"), 2) << "body: " << discover_body;
    // State the negative directly rather than inferring it from the id above: no part of the
    // response a peer can read may carry the internal id, whatever shape it takes.
    EXPECT_EQ(discover_body.find("mcp-pregate"), std::string::npos) << "body: " << discover_body;
}

TEST_F(HttpTransportTest, SessionlessNonDiscoverRequestRejectedAfterSessionEstablished) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18201);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(10));
    bool timed_out = false;

    bool tools_list_reached_server = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto initialize_request = co_await server_transport.read_message();
            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", nlohmann::json::object()}}},
                {"id", nlohmann::json::parse(initialize_request).at("id")}};
            co_await server_transport.write_message(initialize_response.dump());

            co_await server_transport.read_message();
            tools_list_reached_server = true;
        },
        [](std::exception_ptr) {});

    auto tools_list_status = http::status::unknown;
    std::string tools_list_body;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18201", asio::use_awaitable);

            beast::tcp_stream init_stream(io_ctx_.get_executor());
            co_await init_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> init_request{http::verb::post, "/mcp", 11};
            init_request.set(http::field::host, "127.0.0.1");
            init_request.set(http::field::content_type, "application/json");
            init_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":")" +
                std::string(mcp::g_LATEST_PROTOCOL_VERSION) +
                R"(","clientInfo":{"name":"test-client","version":"1.0.0"},"capabilities":{}}})";
            init_request.prepare_payload();
            co_await http::async_write(init_stream, init_request, asio::use_awaitable);

            beast::flat_buffer init_buffer;
            http::response<http::string_body> init_response;
            co_await http::async_read(init_stream, init_buffer, init_response, asio::use_awaitable);

            beast::error_code init_shutdown_error;
            init_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, init_shutdown_error);

            // Same shape as the discover probe above, but a non-pre-gate method: the session
            // gate must still reject it, pinning the discover exemption as method-specific.
            beast::tcp_stream tools_stream(io_ctx_.get_executor());
            co_await tools_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> tools_request{http::verb::post, "/mcp", 11};
            tools_request.set(http::field::host, "127.0.0.1");
            tools_request.set(http::field::content_type, "application/json");
            tools_request.body() = R"({"jsonrpc":"2.0","method":"tools/list","id":2})";
            tools_request.prepare_payload();
            co_await http::async_write(tools_stream, tools_request, asio::use_awaitable);

            beast::flat_buffer tools_buffer;
            http::response<http::string_body> tools_response;
            co_await http::async_read(tools_stream, tools_buffer, tools_response, asio::use_awaitable);
            tools_list_status = tools_response.result();
            tools_list_body = tools_response.body();

            beast::error_code tools_shutdown_error;
            tools_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, tools_shutdown_error);

            deadline->cancel();
            server_transport.close();
        },
        [](std::exception_ptr) {});

    deadline->async_wait([&timed_out, &server_transport](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server_transport.close();
    });

    io_ctx_.run();

    EXPECT_FALSE(timed_out);
    EXPECT_FALSE(tools_list_reached_server);
    EXPECT_EQ(tools_list_status, http::status::bad_request) << "body: " << tools_list_body;
    EXPECT_NE(tools_list_body.find("Session active"), std::string::npos) << "body: " << tools_list_body;
}

TEST_F(HttpTransportTest, DiscoverWithWrongSessionHeaderIsStillRejected) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18203);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(10));
    bool timed_out = false;

    bool discover_reached_server = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto initialize_request = co_await server_transport.read_message();
            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", nlohmann::json::object()}}},
                {"id", nlohmann::json::parse(initialize_request).at("id")}};
            co_await server_transport.write_message(initialize_response.dump());

            co_await server_transport.read_message();
            discover_reached_server = true;
        },
        [](std::exception_ptr) {});

    auto discover_status = http::status::unknown;
    std::string discover_body;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18203", asio::use_awaitable);

            beast::tcp_stream init_stream(io_ctx_.get_executor());
            co_await init_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> init_request{http::verb::post, "/mcp", 11};
            init_request.set(http::field::host, "127.0.0.1");
            init_request.set(http::field::content_type, "application/json");
            init_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":")" +
                std::string(mcp::g_LATEST_PROTOCOL_VERSION) +
                R"(","clientInfo":{"name":"test-client","version":"1.0.0"},"capabilities":{}}})";
            init_request.prepare_payload();
            co_await http::async_write(init_stream, init_request, asio::use_awaitable);

            beast::flat_buffer init_buffer;
            http::response<http::string_body> init_response;
            co_await http::async_read(init_stream, init_buffer, init_response, asio::use_awaitable);
            const auto established_session_id = std::string(init_response["MCP-Session-Id"]);
            EXPECT_FALSE(established_session_id.empty());

            beast::error_code init_shutdown_error;
            init_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, init_shutdown_error);

            // The exemption is conditional on the session header being ABSENT. A discover
            // request that presents a header, and presents the wrong one, must keep going
            // through validate_post_session and be rejected exactly as before.
            beast::tcp_stream discover_stream(io_ctx_.get_executor());
            co_await discover_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> discover_request{http::verb::post, "/mcp", 11};
            discover_request.set(http::field::host, "127.0.0.1");
            discover_request.set(http::field::content_type, "application/json");
            discover_request.set("MCP-Session-Id", established_session_id + "-tampered");
            discover_request.body() = R"({"jsonrpc":"2.0","method":"server/discover","id":2})";
            discover_request.prepare_payload();
            co_await http::async_write(discover_stream, discover_request, asio::use_awaitable);

            beast::flat_buffer discover_buffer;
            http::response<http::string_body> discover_response;
            co_await http::async_read(discover_stream, discover_buffer, discover_response,
                                      asio::use_awaitable);
            discover_status = discover_response.result();
            discover_body = discover_response.body();

            beast::error_code discover_shutdown_error;
            discover_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both,
                                              discover_shutdown_error);

            deadline->cancel();
            server_transport.close();
        },
        [](std::exception_ptr) {});

    deadline->async_wait([&timed_out, &server_transport](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server_transport.close();
    });

    io_ctx_.run();

    EXPECT_FALSE(timed_out);
    EXPECT_FALSE(discover_reached_server);
    EXPECT_EQ(discover_status, http::status::bad_request) << "body: " << discover_body;
    EXPECT_NE(discover_body.find("Invalid MCP-Session-Id header"), std::string::npos)
        << "body: " << discover_body;
}

TEST_F(HttpTransportTest, SessionlessDiscoverNotificationIsStillRejected) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18204);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(10));
    bool timed_out = false;

    bool notification_reached_server = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto initialize_request = co_await server_transport.read_message();
            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", nlohmann::json::object()}}},
                {"id", nlohmann::json::parse(initialize_request).at("id")}};
            co_await server_transport.write_message(initialize_response.dump());

            co_await server_transport.read_message();
            notification_reached_server = true;
        },
        [](std::exception_ptr) {});

    auto notification_status = http::status::unknown;
    std::string notification_body;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18204", asio::use_awaitable);

            beast::tcp_stream init_stream(io_ctx_.get_executor());
            co_await init_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> init_request{http::verb::post, "/mcp", 11};
            init_request.set(http::field::host, "127.0.0.1");
            init_request.set(http::field::content_type, "application/json");
            init_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":")" +
                std::string(mcp::g_LATEST_PROTOCOL_VERSION) +
                R"(","clientInfo":{"name":"test-client","version":"1.0.0"},"capabilities":{}}})";
            init_request.prepare_payload();
            co_await http::async_write(init_stream, init_request, asio::use_awaitable);

            beast::flat_buffer init_buffer;
            http::response<http::string_body> init_response;
            co_await http::async_read(init_stream, init_buffer, init_response, asio::use_awaitable);

            beast::error_code init_shutdown_error;
            init_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, init_shutdown_error);

            // A sessionless server/discover with no "id" is a notification, not a request. It
            // must NOT take the exemption, because doing so would push its body onto the
            // unbounded incoming queue and answer 202 without the session gate ever running.
            beast::tcp_stream notification_stream(io_ctx_.get_executor());
            co_await notification_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> notification_request{http::verb::post, "/mcp", 11};
            notification_request.set(http::field::host, "127.0.0.1");
            notification_request.set(http::field::content_type, "application/json");
            notification_request.body() = R"({"jsonrpc":"2.0","method":"server/discover"})";
            notification_request.prepare_payload();
            co_await http::async_write(notification_stream, notification_request, asio::use_awaitable);

            beast::flat_buffer notification_buffer;
            http::response<http::string_body> notification_response;
            co_await http::async_read(notification_stream, notification_buffer, notification_response,
                                      asio::use_awaitable);
            notification_status = notification_response.result();
            notification_body = notification_response.body();

            beast::error_code notification_shutdown_error;
            notification_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both,
                                                  notification_shutdown_error);

            deadline->cancel();
            server_transport.close();
        },
        [](std::exception_ptr) {});

    deadline->async_wait([&timed_out, &server_transport](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server_transport.close();
    });

    io_ctx_.run();

    EXPECT_FALSE(timed_out);
    EXPECT_FALSE(notification_reached_server);
    EXPECT_EQ(notification_status, http::status::bad_request) << "body: " << notification_body;
    EXPECT_NE(notification_body.find("Session active"), std::string::npos)
        << "body: " << notification_body;
}

// Sessionless server/discover is unauthenticated by construction, so its responses must never
// be appended to the replay EventStore. That store is a bounded ring shared with the
// established session: a flood of sessionless requests would evict the session's replay
// history, and its next Last-Event-ID resume would fail with 410 Gone, losing messages
// unrecoverably. The store is sized to 2 here so that three sessionless requests WOULD evict
// event "1" if they were stored, mirroring HttpResumabilityTest.GetWithEvictedEventIdReturns410
// which produces exactly that 410 using the session's own traffic.
TEST_F(HttpTransportTest, SessionlessDiscoverDoesNotEvictSessionReplayHistory) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18205, 2);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(10));
    bool timed_out = false;

    constexpr int k_sessionless_request_count = 3;

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto initialize_request = co_await server_transport.read_message();
            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", nlohmann::json::object()}}},
                {"id", nlohmann::json::parse(initialize_request).at("id")}};
            co_await server_transport.write_message(initialize_response.dump());

            for (int i = 0; i < k_sessionless_request_count; ++i) {
                const auto discover_request = co_await server_transport.read_message();
                nlohmann::json discover_response = {
                    {"jsonrpc", "2.0"},
                    {"result",
                     {{"supportedVersions",
                       nlohmann::json::array({std::string(mcp::g_PROTOCOL_VERSION_2026_07_28)})}}},
                    {"id", nlohmann::json::parse(discover_request).at("id")}};
                co_await server_transport.write_message(discover_response.dump());
            }
        },
        [](std::exception_ptr) {});

    int resume_status = 0;
    int sessionless_ok_count = 0;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18205", asio::use_awaitable);

            beast::tcp_stream init_stream(io_ctx_.get_executor());
            co_await init_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> init_request{http::verb::post, "/mcp", 11};
            init_request.set(http::field::host, "127.0.0.1");
            init_request.set(http::field::content_type, "application/json");
            init_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":")" +
                std::string(mcp::g_LATEST_PROTOCOL_VERSION) +
                R"(","clientInfo":{"name":"test-client","version":"1.0.0"},"capabilities":{}}})";
            init_request.prepare_payload();
            co_await http::async_write(init_stream, init_request, asio::use_awaitable);

            beast::flat_buffer init_buffer;
            http::response<http::string_body> init_response;
            co_await http::async_read(init_stream, init_buffer, init_response, asio::use_awaitable);
            const auto established_session_id = std::string(init_response["MCP-Session-Id"]);

            beast::error_code init_shutdown_error;
            init_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, init_shutdown_error);

            // Unauthenticated traffic: none of these carry an MCP-Session-Id header.
            for (int i = 0; i < k_sessionless_request_count; ++i) {
                beast::tcp_stream discover_stream(io_ctx_.get_executor());
                co_await discover_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

                http::request<http::string_body> discover_request{http::verb::post, "/mcp", 11};
                discover_request.set(http::field::host, "127.0.0.1");
                discover_request.set(http::field::content_type, "application/json");
                discover_request.body() = R"({"jsonrpc":"2.0","method":"server/discover","id":)" +
                                          std::to_string(101 + i) + "}";
                discover_request.prepare_payload();
                co_await http::async_write(discover_stream, discover_request, asio::use_awaitable);

                beast::flat_buffer discover_buffer;
                http::response<http::string_body> discover_response;
                co_await http::async_read(discover_stream, discover_buffer, discover_response,
                                          asio::use_awaitable);
                if (discover_response.result() == http::status::ok) {
                    ++sessionless_ok_count;
                }

                beast::error_code discover_shutdown_error;
                discover_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both,
                                                  discover_shutdown_error);
            }

            // The established session resumes from the event id it already holds.
            beast::tcp_stream resume_stream(io_ctx_.get_executor());
            co_await resume_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::empty_body> resume_request{http::verb::get, "/mcp", 11};
            resume_request.set(http::field::host, "127.0.0.1");
            resume_request.set(http::field::accept, "text/event-stream");
            resume_request.set("MCP-Protocol-Version", std::string(mcp::g_LATEST_PROTOCOL_VERSION));
            resume_request.set("MCP-Session-Id", established_session_id);
            resume_request.set("Last-Event-ID", "1");
            co_await http::async_write(resume_stream, resume_request, asio::use_awaitable);

            beast::flat_buffer resume_buffer;
            http::response<http::string_body> resume_response;
            co_await http::async_read(resume_stream, resume_buffer, resume_response,
                                      asio::use_awaitable);
            resume_status = static_cast<int>(resume_response.result_int());

            beast::error_code resume_shutdown_error;
            resume_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both,
                                            resume_shutdown_error);

            deadline->cancel();
            server_transport.close();
        },
        [](std::exception_ptr) {});

    deadline->async_wait([&timed_out, &server_transport](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server_transport.close();
    });

    io_ctx_.run();

    EXPECT_FALSE(timed_out);
    // All three were served, so a pass here is not an artifact of rejecting them.
    EXPECT_EQ(sessionless_ok_count, k_sessionless_request_count);
    EXPECT_EQ(resume_status, 200);
    // Only the initialize response belongs in the store.
    EXPECT_EQ(server_transport.event_store().size(), 1u);
}

TEST_F(HttpTransportTest, DiscoverAcceptsDiscoverableOnlyProtocolVersionHeader) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18202);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(10));
    bool timed_out = false;

    bool discover_request_received = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto discover_request = co_await server_transport.read_message();
            discover_request_received = true;
            nlohmann::json discover_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"supportedVersions",
                   nlohmann::json::array({std::string(mcp::g_PROTOCOL_VERSION_2026_07_28)})}}},
                {"id", nlohmann::json::parse(discover_request).at("id")}};
            co_await server_transport.write_message(discover_response.dump());
        },
        [](std::exception_ptr) {});

    auto discover_status = http::status::unknown;
    std::string discover_body;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18202", asio::use_awaitable);

            beast::tcp_stream stream(io_ctx_.get_executor());
            co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> discover_request{http::verb::post, "/mcp", 11};
            discover_request.set(http::field::host, "127.0.0.1");
            discover_request.set(http::field::content_type, "application/json");
            // 2026-07-28 is discoverable but not in g_SUPPORTED_PROTOCOL_VERSIONS, so the
            // negotiated-version check would reject it for any non-pre-gate method.
            discover_request.set("MCP-Protocol-Version",
                                 std::string(mcp::g_PROTOCOL_VERSION_2026_07_28));
            discover_request.body() = R"({"jsonrpc":"2.0","method":"server/discover","id":1})";
            discover_request.prepare_payload();
            co_await http::async_write(stream, discover_request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> discover_response;
            co_await http::async_read(stream, response_buffer, discover_response, asio::use_awaitable);
            discover_status = discover_response.result();
            discover_body = discover_response.body();

            beast::error_code shutdown_error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);

            deadline->cancel();
            server_transport.close();
        },
        [](std::exception_ptr) {});

    deadline->async_wait([&timed_out, &server_transport](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server_transport.close();
    });

    io_ctx_.run();

    EXPECT_FALSE(timed_out);
    EXPECT_TRUE(discover_request_received);
    EXPECT_EQ(discover_status, http::status::ok) << "body: " << discover_body;
}

TEST_F(HttpTransportTest, SessionlessDiscoverCannotSquatSessionRequestId) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18206);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(10));
    bool timed_out = false;

    // Signalled once initialize has produced a session, so the sessionless prober runs while a
    // session is live.
    auto session_ready = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    session_ready->expires_at(std::chrono::steady_clock::time_point::max());
    // Signalled once the sessionless discover has reached the server. Registration precedes the
    // enqueue, so a discover that the server can read is a discover already holding its id.
    auto discover_registered = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    discover_registered->expires_at(std::chrono::steady_clock::time_point::max());

    bool discover_reached_server = false;
    bool session_request_reached_server = false;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto initialize_request = co_await server_transport.read_message();
            nlohmann::json initialize_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", nlohmann::json::object()}}},
                {"id", nlohmann::json::parse(initialize_request).at("id")}};
            co_await server_transport.write_message(initialize_response.dump());

            // The sessionless discover is deliberately left unanswered: it holds its pending
            // entry for the whole test, which is what gives the session request something to
            // collide with.
            const auto discover_request = co_await server_transport.read_message();
            discover_reached_server = true;
            discover_registered->cancel();

            const auto session_request = co_await server_transport.read_message();
            session_request_reached_server = true;
            nlohmann::json session_response = {{"jsonrpc", "2.0"},
                                               {"result", {{"tools", nlohmann::json::array()}}},
                                               {"id", nlohmann::json::parse(session_request).at("id")}};
            co_await server_transport.write_message(session_response.dump());
        },
        [](std::exception_ptr) {});

    // The sessionless prober. It writes its discover and never reads the reply, so the pending
    // entry it claims stays claimed.
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                co_await session_ready->async_wait(asio::use_awaitable);
            } catch (const boost::system::system_error&) {
                // Cancelled: the session is up.
            }

            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18206", asio::use_awaitable);

            auto probe_stream = std::make_shared<beast::tcp_stream>(io_ctx_.get_executor());
            co_await probe_stream->async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> discover_request{http::verb::post, "/mcp", 11};
            discover_request.set(http::field::host, "127.0.0.1");
            discover_request.set(http::field::content_type, "application/json");
            discover_request.body() = R"({"jsonrpc":"2.0","method":"server/discover","id":7})";
            discover_request.prepare_payload();
            co_await http::async_write(*probe_stream, discover_request, asio::use_awaitable);

            // Hold the connection open for the rest of the test.
            try {
                co_await deadline->async_wait(asio::use_awaitable);
            } catch (const boost::system::system_error&) {
                // Cancelled at teardown.
            }
        },
        [](std::exception_ptr) {});

    auto collision_status = http::status::unknown;
    std::string collision_body;
    std::string established_session_id;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            const auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", "18206", asio::use_awaitable);

            beast::tcp_stream session_stream(io_ctx_.get_executor());
            co_await session_stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> init_request{http::verb::post, "/mcp", 11};
            init_request.set(http::field::host, "127.0.0.1");
            init_request.set(http::field::content_type, "application/json");
            init_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":")" +
                std::string(mcp::g_LATEST_PROTOCOL_VERSION) +
                R"(","clientInfo":{"name":"test-client","version":"1.0.0"},"capabilities":{}}})";
            init_request.prepare_payload();
            co_await http::async_write(session_stream, init_request, asio::use_awaitable);

            beast::flat_buffer init_buffer;
            http::response<http::string_body> init_response;
            co_await http::async_read(session_stream, init_buffer, init_response, asio::use_awaitable);
            established_session_id = std::string(init_response["MCP-Session-Id"]);

            session_ready->cancel();
            try {
                co_await discover_registered->async_wait(asio::use_awaitable);
            } catch (const boost::system::system_error&) {
                // Cancelled: the sessionless discover holds id 7.
            }

            // A legitimate, session-authenticated request that happens to reuse id 7. The
            // sessionless prober picked that id out of a space the session owns, so this must
            // still be served.
            http::request<http::string_body> tools_request{http::verb::post, "/mcp", 11};
            tools_request.set(http::field::host, "127.0.0.1");
            tools_request.set(http::field::content_type, "application/json");
            tools_request.set("MCP-Session-Id", established_session_id);
            tools_request.body() = R"({"jsonrpc":"2.0","method":"tools/list","id":7})";
            tools_request.prepare_payload();
            co_await http::async_write(session_stream, tools_request, asio::use_awaitable);

            beast::flat_buffer tools_buffer;
            http::response<http::string_body> tools_response;
            co_await http::async_read(session_stream, tools_buffer, tools_response,
                                      asio::use_awaitable);
            collision_status = tools_response.result();
            collision_body = tools_response.body();

            beast::error_code shutdown_error;
            session_stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);

            deadline->cancel();
            server_transport.close();
        },
        [](std::exception_ptr) {});

    deadline->async_wait([&timed_out, &server_transport](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        server_transport.close();
    });

    io_ctx_.run();

    EXPECT_FALSE(timed_out);
    EXPECT_FALSE(established_session_id.empty());
    EXPECT_TRUE(discover_reached_server);
    EXPECT_TRUE(session_request_reached_server)
        << "the session request never reached the server: its id was squatted";
    EXPECT_EQ(collision_status, http::status::ok) << "body: " << collision_body;
    EXPECT_NE(collision_body.find("\"result\""), std::string::npos) << "body: " << collision_body;
    // Replay-store classification must survive the containment. sessionless_request_ids decides
    // replay-store exclusion, so if the prober's chosen id were still the key, the session's OWN
    // response -- which carries that same id 7 -- would be misclassified as sessionless and silently
    // dropped from the replay store. Both the initialize response and the id-7 session response
    // belong there; only the unanswered sessionless discover does not.
    EXPECT_EQ(server_transport.event_store().size(), 2u);
}

TEST_F(HttpTransportTest, NonAtomicConfigurationLocksWhenListeningStarts) {
    mcp::HttpServerTransport server(io_ctx_.get_executor(), "127.0.0.1", 0);
    auto listener = server.listen();

    EXPECT_NO_THROW(server.set_json_only(true));
    EXPECT_THROW(server.set_allowed_origins({"https://trusted.example"}), std::logic_error);
    EXPECT_THROW(server.set_allow_all_origins(true), std::logic_error);
    EXPECT_THROW(server.set_bearer_token_validator({}), std::logic_error);
    EXPECT_THROW(server.set_bearer_challenge({}), std::logic_error);
    EXPECT_THROW(server.set_protected_resource_metadata({}), std::logic_error);
    EXPECT_THROW(server.set_unauthenticated_paths({"/health"}), std::logic_error);
    EXPECT_THROW(server.set_async_bearer_token_validator({}), std::logic_error);
    EXPECT_THROW(server.set_max_request_body_bytes(4096), std::logic_error);

    server.close();
    asio::co_spawn(io_ctx_, std::move(listener), asio::detached);
    io_ctx_.run();
}

// A bearer provider installed after write_message() returns belongs to the next request, not to the
// one that call already started. HttpClientTransport pins the provider when the request begins, the
// way OAuthHttpClient::make_exchange() pins its per-exchange state, so the property holds without
// any timing window to hit: the swap below happens strictly after write_message() has returned and
// strictly before the request reaches the wire.
TEST_F(HttpTransportTest, WriteMessagePinsBearerProviderAtRequestStart) {
    asio::ip::tcp::acceptor acceptor(io_ctx_,
                                     asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();

    std::string observed_authorization;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            beast::tcp_stream stream(co_await acceptor.async_accept(asio::use_awaitable));

            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            co_await http::async_read(stream, buffer, request, asio::use_awaitable);
            observed_authorization = std::string(request[http::field::authorization]);

            http::response<http::string_body> response{http::status::accepted, 11};
            response.prepare_payload();
            co_await http::async_write(stream, response, asio::use_awaitable);

            beast::error_code ignored;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        },
        asio::detached);

    mcp::HttpClientTransport client(io_ctx_.get_executor(),
                                    "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    client.set_bearer_token_provider([]() { return std::string("pinned-at-start"); });

    nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    auto write = client.write_message(notification.dump());

    client.set_bearer_token_provider([]() { return std::string("swapped-after-start"); });

    std::exception_ptr write_error;
    asio::co_spawn(io_ctx_, std::move(write),
                   [&write_error](std::exception_ptr error) { write_error = error; });
    io_ctx_.run();

    ASSERT_EQ(write_error, nullptr);
    EXPECT_EQ(observed_authorization, "Bearer pinned-at-start");
}

// ===========================================================================
// WWW-Authenticate challenge rendering
// ===========================================================================

TEST(BearerChallengeTest, EmptyConfigRendersBareBearer) {
    EXPECT_EQ(mcp::format_www_authenticate(mcp::BearerChallengeConfig{}), "Bearer");
}

TEST(BearerChallengeTest, EachParameterRendersOnItsOwn) {
    mcp::BearerChallengeConfig realm_only;
    realm_only.realm = "mcp";
    EXPECT_EQ(mcp::format_www_authenticate(realm_only), R"(Bearer realm="mcp")");

    mcp::BearerChallengeConfig error_only;
    error_only.error = "invalid_token";
    EXPECT_EQ(mcp::format_www_authenticate(error_only), R"(Bearer error="invalid_token")");

    mcp::BearerChallengeConfig scope_only;
    scope_only.scope = "mcp:read mcp:write";
    EXPECT_EQ(mcp::format_www_authenticate(scope_only), R"(Bearer scope="mcp:read mcp:write")");

    mcp::BearerChallengeConfig metadata_only;
    metadata_only.resource_metadata = "http://127.0.0.1:9000/.well-known/oauth-protected-resource/mcp";
    EXPECT_EQ(
        mcp::format_www_authenticate(metadata_only),
        R"(Bearer resource_metadata="http://127.0.0.1:9000/.well-known/oauth-protected-resource/mcp")");
}

TEST(BearerChallengeTest, AllParametersRenderInDocumentedOrder) {
    mcp::BearerChallengeConfig challenge;
    challenge.resource_metadata = "http://127.0.0.1:9000/.well-known/oauth-protected-resource/mcp";
    challenge.scope = "mcp:read";
    challenge.realm = "mcp";
    challenge.error = "invalid_token";

    EXPECT_EQ(mcp::format_www_authenticate(challenge),
              R"(Bearer realm="mcp", error="invalid_token", scope="mcp:read", )"
              R"(resource_metadata="http://127.0.0.1:9000/.well-known/oauth-protected-resource/mcp")");
}

TEST(BearerChallengeTest, OrderIsIndependentOfAssignmentOrder) {
    mcp::BearerChallengeConfig assigned_forwards;
    assigned_forwards.realm = "r";
    assigned_forwards.scope = "s";

    mcp::BearerChallengeConfig assigned_backwards;
    assigned_backwards.scope = "s";
    assigned_backwards.realm = "r";

    EXPECT_EQ(mcp::format_www_authenticate(assigned_forwards), R"(Bearer realm="r", scope="s")");
    EXPECT_EQ(mcp::format_www_authenticate(assigned_backwards),
              mcp::format_www_authenticate(assigned_forwards));
}

TEST(BearerChallengeTest, BackslashAndQuoteAreEscaped) {
    mcp::BearerChallengeConfig challenge;
    challenge.realm = R"(a"b\c)";

    EXPECT_EQ(mcp::format_www_authenticate(challenge), R"(Bearer realm="a\"b\\c")");
}

TEST(BearerChallengeTest, UnquotableValueIsRejected) {
    mcp::BearerChallengeConfig carriage_return;
    carriage_return.realm = "mcp\r\nX-Injected: 1";
    EXPECT_THROW(mcp::format_www_authenticate(carriage_return), std::invalid_argument);

    mcp::BearerChallengeConfig non_ascii;
    non_ascii.scope =
        "mcp:r\xc3\xa9"
        "ad";
    EXPECT_THROW(mcp::format_www_authenticate(non_ascii), std::invalid_argument);
}

TEST(HttpRequestPathTest, QueryAndFragmentAreStripped) {
    EXPECT_EQ(mcp::http_request_path("/health"), "/health");
    EXPECT_EQ(mcp::http_request_path("/health?probe=1"), "/health");
    EXPECT_EQ(mcp::http_request_path("/health#frag"), "/health");
    EXPECT_EQ(mcp::http_request_path("/health?a=1#frag"), "/health");
}

TEST(ProtectedResourceMetadataTest, PathInsertsTheWellKnownSegmentBeforeTheResourcePath) {
    // RFC 9728 3.1: a resource with a path is described under that path, not at the bare
    // well-known location. Publishing at the bare path would leave clients looking elsewhere.
    mcp::ProtectedResourceMetadataConfig with_path;
    with_path.resource = "https://h/mcp";
    EXPECT_EQ(mcp::protected_resource_metadata_path(with_path),
              "/.well-known/oauth-protected-resource/mcp");
    EXPECT_EQ(mcp::protected_resource_metadata_url(with_path),
              "https://h/.well-known/oauth-protected-resource/mcp");

    mcp::ProtectedResourceMetadataConfig nested;
    nested.resource = "https://h/a/b";
    EXPECT_EQ(mcp::protected_resource_metadata_path(nested),
              "/.well-known/oauth-protected-resource/a/b");
}

TEST(ProtectedResourceMetadataTest, PathIsBareOnlyForAResourceAtTheOriginRoot) {
    mcp::ProtectedResourceMetadataConfig root;
    root.resource = "https://h";
    EXPECT_EQ(mcp::protected_resource_metadata_path(root), "/.well-known/oauth-protected-resource");

    mcp::ProtectedResourceMetadataConfig trailing_slash;
    trailing_slash.resource = "https://h/";
    EXPECT_EQ(mcp::protected_resource_metadata_path(trailing_slash),
              "/.well-known/oauth-protected-resource");
}

TEST(ProtectedResourceMetadataTest, AnExplicitPathOverridesTheDerivation) {
    mcp::ProtectedResourceMetadataConfig overridden;
    overridden.resource = "https://h/mcp";
    overridden.path = "/custom-metadata";

    EXPECT_EQ(mcp::protected_resource_metadata_path(overridden), "/custom-metadata");
    EXPECT_EQ(mcp::protected_resource_metadata_url(overridden), "https://h/custom-metadata");
}

TEST(ProtectedResourceMetadataTest, DocumentUrlIgnoresQueryAndFragmentOnTheResource) {
    mcp::ProtectedResourceMetadataConfig noisy;
    noisy.resource = "https://h/mcp?x=1#frag";

    EXPECT_EQ(mcp::protected_resource_metadata_path(noisy),
              "/.well-known/oauth-protected-resource/mcp");
}

TEST(ProtectedResourceMetadataTest, DocumentOmitsEmptyLists) {
    mcp::ProtectedResourceMetadataConfig metadata;
    metadata.resource = "http://127.0.0.1:9000/mcp";

    const auto document = nlohmann::json::parse(mcp::format_protected_resource_metadata(metadata));

    EXPECT_EQ(document.at("resource"), "http://127.0.0.1:9000/mcp");
    EXPECT_FALSE(document.contains("authorization_servers"));
    EXPECT_FALSE(document.contains("scopes_supported"));
}

// ===========================================================================
// Server-side OAuth challenge, metadata route and unauthenticated paths
// ===========================================================================

namespace {

struct ChallengeProbeResult {
    unsigned int status{0};
    std::string www_authenticate;
    std::string content_type;
    std::string body;
};

/// Fire one HTTP request at the transport and report the parts the challenge tests assert on.
mcp::Task<ChallengeProbeResult> probe(const asio::any_io_executor& executor, unsigned short port,
                                      http::verb method, const std::string& target,
                                      const std::string& body = {},
                                      const std::string& bearer_token = {}) {
    beast::tcp_stream stream(executor);
    asio::ip::tcp::resolver resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    if (!bearer_token.empty()) {
        request.set(http::field::authorization, "Bearer " + bearer_token);
    }
    request.body() = body;
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer response_buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

    ChallengeProbeResult result;
    result.status = response.result_int();
    result.www_authenticate = std::string(response[http::field::www_authenticate]);
    result.content_type = std::string(response[http::field::content_type]);
    result.body = response.body();

    beast::error_code shutdown_error;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
    co_return result;
}

constexpr std::string_view g_notification_body = R"({"jsonrpc":"2.0","method":"notifications/x"})";

}  // namespace

// The challenge URL must come from `resource` alone. Behind a TLS terminator, a reverse proxy or a
// container port mapping the listener's own origin is not the one clients can reach, so a URL
// inferred from it would be unfetchable. The listener below is deliberately bound to an ephemeral
// port that has nothing to do with the advertised resource.
TEST_F(HttpTransportTest, ChallengeMetadataUrlComesFromTheResourceNotTheListener) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    const auto listener_port = server->port();

    mcp::ProtectedResourceMetadataConfig metadata;
    metadata.resource = "https://mcp.example.com/mcp";
    server->set_protected_resource_metadata(metadata);

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult denied;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            denied = co_await probe(io_ctx_.get_executor(), listener_port, http::verb::post, "/mcp",
                                    std::string(g_notification_body));
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    EXPECT_EQ(
        denied.www_authenticate,
        R"(Bearer resource_metadata="https://mcp.example.com/.well-known/oauth-protected-resource/mcp")");
    EXPECT_EQ(denied.www_authenticate.find("127.0.0.1"), std::string::npos)
        << "the challenge leaked the listener address: " << denied.www_authenticate;
    EXPECT_EQ(denied.www_authenticate.find(std::to_string(listener_port)), std::string::npos)
        << "the challenge leaked the listener port: " << denied.www_authenticate;
}

TEST_F(HttpTransportTest, UnconfiguredTransportSendsBareBearerChallenge) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    const auto port = server->port();

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult denied;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            denied = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                    std::string(g_notification_body));
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    EXPECT_EQ(denied.status, 401);
    EXPECT_EQ(denied.www_authenticate, "Bearer");
}

TEST_F(HttpTransportTest, ConfiguredChallengeIsSentOnUnauthorized) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });

    mcp::BearerChallengeConfig challenge;
    challenge.realm = "mcp";
    challenge.scope = "mcp:read";
    challenge.resource_metadata = "http://127.0.0.1:9000/.well-known/oauth-protected-resource/mcp";
    server->set_bearer_challenge(challenge);
    const auto port = server->port();

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult denied;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            denied = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                    std::string(g_notification_body));
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    EXPECT_EQ(denied.status, 401);
    EXPECT_EQ(denied.www_authenticate,
              R"(Bearer realm="mcp", scope="mcp:read", )"
              R"(resource_metadata="http://127.0.0.1:9000/.well-known/oauth-protected-resource/mcp")");
}

TEST_F(HttpTransportTest, ProtectedResourceMetadataIsReadableWithoutAToken) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    const auto port = server->port();

    mcp::ProtectedResourceMetadataConfig metadata;
    metadata.resource = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    metadata.authorization_servers = {"http://127.0.0.1:9000"};
    metadata.scopes_supported = {"mcp:read"};
    server->set_protected_resource_metadata(metadata);

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult document;
    ChallengeProbeResult denied;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            document = co_await probe(io_ctx_.get_executor(), port, http::verb::get,
                                      "/.well-known/oauth-protected-resource/mcp");
            denied = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                    std::string(g_notification_body));
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    ASSERT_EQ(document.status, 200);
    EXPECT_EQ(document.content_type, "application/json");
    const auto parsed = nlohmann::json::parse(document.body);
    EXPECT_EQ(parsed.at("resource"), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    EXPECT_EQ(parsed.at("authorization_servers"), nlohmann::json::array({"http://127.0.0.1:9000"}));
    EXPECT_EQ(parsed.at("scopes_supported"), nlohmann::json::array({"mcp:read"}));

    EXPECT_EQ(denied.status, 401);
    EXPECT_EQ(denied.www_authenticate, R"(Bearer resource_metadata="http://127.0.0.1:)" +
                                           std::to_string(port) +
                                           R"(/.well-known/oauth-protected-resource/mcp")");
}

TEST_F(HttpTransportTest, ExplicitChallengeMetadataUrlSurvivesMetadataConfiguration) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    const auto port = server->port();

    mcp::BearerChallengeConfig challenge;
    challenge.resource_metadata = "http://gateway.example/.well-known/oauth-protected-resource";
    server->set_bearer_challenge(challenge);

    mcp::ProtectedResourceMetadataConfig metadata;
    metadata.resource = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    server->set_protected_resource_metadata(metadata);

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult denied;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            denied = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                    std::string(g_notification_body));
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    EXPECT_EQ(
        denied.www_authenticate,
        R"(Bearer resource_metadata="http://gateway.example/.well-known/oauth-protected-resource")");
}

TEST_F(HttpTransportTest, UnauthenticatedPathsAreExemptFromAuthAndExcludedFromDispatch) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    server->set_unauthenticated_paths({"/health"});
    const auto port = server->port();

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult exempt;
    ChallengeProbeResult exempt_with_query;
    ChallengeProbeResult guarded;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            exempt = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/health",
                                    std::string(g_notification_body));
            exempt_with_query = co_await probe(io_ctx_.get_executor(), port, http::verb::post,
                                               "/health?probe=1", std::string(g_notification_body));
            guarded = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/healthy",
                                     std::string(g_notification_body));
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    // Exempt from the bearer check, and excluded from MCP dispatch: nothing claimed the path, so
    // it is 404 rather than an unauthenticated 202.
    EXPECT_EQ(exempt.status, 404);
    EXPECT_EQ(exempt_with_query.status, 404);
    EXPECT_EQ(guarded.status, 401);
}

// The catastrophic misconfiguration: exempting the path MCP is served on. It must fail loudly.
TEST_F(HttpTransportTest, ExemptingTheMcpPathRefusesToServeMcpUnauthenticated) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    server->set_unauthenticated_paths({"/mcp"});
    const auto port = server->port();

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult dispatched;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            dispatched = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                        std::string(g_notification_body));
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    EXPECT_EQ(dispatched.status, 404);
    EXPECT_NE(dispatched.status, 202) << "an exempt path must never reach unauthenticated dispatch";
}

TEST_F(HttpTransportTest, AnExemptPathTheMetadataRouteClaimsIsStillServed) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    server->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    const auto port = server->port();

    mcp::ProtectedResourceMetadataConfig metadata;
    metadata.resource = "https://mcp.example.com/mcp";
    server->set_protected_resource_metadata(metadata);
    server->set_unauthenticated_paths({"/.well-known/oauth-protected-resource/mcp"});

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult document;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            document = co_await probe(io_ctx_.get_executor(), port, http::verb::get,
                                      "/.well-known/oauth-protected-resource/mcp");
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    ASSERT_EQ(document.status, 200);
    EXPECT_EQ(nlohmann::json::parse(document.body).at("resource"), "https://mcp.example.com/mcp");
}

TEST_F(HttpTransportTest, AsyncBearerValidatorDecidesWithoutBlocking) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    std::atomic<int> validator_calls{0};
    server->set_async_bearer_token_validator([&validator_calls](std::string token) -> mcp::Task<bool> {
        validator_calls.fetch_add(1, std::memory_order_relaxed);
        // Suspend, the way a real introspection call would, before deciding.
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
        co_return token == "good";
    });
    const auto port = server->port();

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    ChallengeProbeResult denied;
    ChallengeProbeResult accepted;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            denied = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                    std::string(g_notification_body), "bad");
            accepted = co_await probe(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                      std::string(g_notification_body), "good");
            server->close();
        },
        asio::detached);
    io_ctx_.run();

    EXPECT_EQ(denied.status, 401);
    EXPECT_EQ(denied.www_authenticate, "Bearer");
    EXPECT_EQ(accepted.status, 202);
    EXPECT_EQ(validator_calls.load(std::memory_order_relaxed), 2);
}

TEST_F(HttpTransportTest, OnlyOneBearerValidatorMayBeInstalled) {
    mcp::HttpServerTransport sync_first(io_ctx_.get_executor(), "127.0.0.1", 0);
    sync_first.set_bearer_token_validator([](std::string_view) { return true; });
    EXPECT_THROW(sync_first.set_async_bearer_token_validator(
                     [](std::string) -> mcp::Task<bool> { co_return true; }),
                 std::logic_error);
    sync_first.close();

    mcp::HttpServerTransport async_first(io_ctx_.get_executor(), "127.0.0.1", 0);
    async_first.set_async_bearer_token_validator(
        [](std::string) -> mcp::Task<bool> { co_return true; });
    EXPECT_THROW(async_first.set_bearer_token_validator([](std::string_view) { return true; }),
                 std::logic_error);
    async_first.close();
}

TEST_F(HttpTransportTest, RequestBodyBeyondTheLimitIsRejected) {
    auto server = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    const auto port = server->port();

    asio::co_spawn(io_ctx_, server->listen(), asio::detached);

    unsigned int status = 0;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            // Announcing the length is enough: the parser rejects the request before the body is
            // sent, which is the point of the limit.
            const std::string header =
                "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                "Content-Length: " +
                std::to_string(mcp::constants::g_default_max_request_body_bytes + 1) + "\r\n\r\n";

            beast::tcp_stream stream(io_ctx_.get_executor());
            asio::ip::tcp::resolver resolver(io_ctx_.get_executor());
            auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
            co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);
            co_await asio::async_write(stream, asio::buffer(header), asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> response;
            co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);
            status = response.result_int();

            beast::error_code shutdown_error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
        },
        [server](const std::exception_ptr&) { server->close(); });
    io_ctx_.run();

    EXPECT_EQ(status, 413);
}

TEST_F(HttpTransportTest, ProtectedResourceMetadataRequiresAnAbsoluteResourceUrl) {
    mcp::HttpServerTransport server(io_ctx_.get_executor(), "127.0.0.1", 0);

    EXPECT_THROW(server.set_protected_resource_metadata({}), std::invalid_argument);

    mcp::ProtectedResourceMetadataConfig relative;
    relative.resource = "/mcp";
    EXPECT_THROW(server.set_protected_resource_metadata(relative), std::invalid_argument);

    server.close();
}
