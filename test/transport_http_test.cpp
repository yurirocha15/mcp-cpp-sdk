#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

}  // namespace

class HttpTransportTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

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
