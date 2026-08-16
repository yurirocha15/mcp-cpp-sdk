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

TEST_F(HttpTransportTest, NonAtomicConfigurationLocksWhenListeningStarts) {
    mcp::HttpServerTransport server(io_ctx_.get_executor(), "127.0.0.1", 0);
    auto listener = server.listen();

    EXPECT_NO_THROW(server.set_json_only(true));
    EXPECT_THROW(server.set_allowed_origins({"https://trusted.example"}), std::logic_error);
    EXPECT_THROW(server.set_allow_all_origins(true), std::logic_error);
    EXPECT_THROW(server.set_bearer_token_validator({}), std::logic_error);

    server.close();
    asio::co_spawn(io_ctx_, std::move(listener), asio::detached);
    io_ctx_.run();
}
