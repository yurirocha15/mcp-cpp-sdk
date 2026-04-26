#include "mcp/detail/signal.hpp"
#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

nlohmann::json make_initialize_request(std::string_view id) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

nlohmann::json make_tool_call_request(std::string_view id, const std::string& tool_name,
                                      const nlohmann::json& args) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "tools/call"},
            {"params", {{"name", tool_name}, {"arguments", args}}}};
}

nlohmann::json greet_schema() {
    return {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"name"})},
    };
}

http::response<http::string_body> send_json_rpc(beast::tcp_stream& stream,
                                                const nlohmann::json& payload,
                                                const std::string& session_id = "") {
    http::request<http::string_body> req{http::verb::post, "/mcp", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    req.set("MCP-Protocol-Version", mcp::g_LATEST_PROTOCOL_VERSION);
    if (!session_id.empty()) {
        req.set("MCP-Session-Id", session_id);
    }
    req.body() = payload.dump();
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buf;
    http::response<http::string_body> resp;
    http::read(stream, buf, resp);
    return resp;
}

}  // namespace

class ServerHttpTest : public ::testing::Test {
   protected:
    ServerHttpTest() {
        mcp::Implementation info;
        info.name = "test-http-server";
        info.version = "1.0";

        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};

        server_ = std::make_unique<mcp::Server>(std::move(info), std::move(caps));

        server_->add_tool<nlohmann::json, nlohmann::json>(
            "greet", "Greets the user", greet_schema(),
            [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                co_return nlohmann::json{{"greeting", "Hello, " + args.at("name").get<std::string>()}};
            });
    }

    std::unique_ptr<mcp::Server> server_;
};

TEST_F(ServerHttpTest, RunHttpInitializesAndResponds) {
    constexpr uint16_t port = 18070;

    std::thread server_thread([&] { server_->run_http("127.0.0.1", port); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    asio::io_context client_ctx;
    asio::ip::tcp::resolver resolver(client_ctx);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));

    beast::tcp_stream stream(client_ctx);
    stream.connect(endpoints);

    auto init_resp = send_json_rpc(stream, make_initialize_request("1"));
    auto init_json = nlohmann::json::parse(init_resp.body());

    std::string session_id;
    auto session_it = init_resp.find("MCP-Session-Id");
    if (session_it != init_resp.end()) {
        session_id = std::string(session_it->value());
    }

    auto tool_resp =
        send_json_rpc(stream, make_tool_call_request("2", "greet", {{"name", "HTTP"}}), session_id);
    auto tool_json = nlohmann::json::parse(tool_resp.body());

    beast::error_code ec;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);

#ifdef _WIN32
    std::raise(SIGINT);
#else
    mcp::detail::trigger_shutdown_signal();
#endif
    server_thread.join();

    EXPECT_EQ(init_json["id"], "1");
    ASSERT_TRUE(init_json.contains("result"));
    EXPECT_EQ(init_json["result"]["serverInfo"]["name"], "test-http-server");

    EXPECT_EQ(tool_json["id"], "2");
    ASSERT_TRUE(tool_json.contains("result"));
    EXPECT_EQ(tool_json["result"]["greeting"], "Hello, HTTP");
}

TEST_F(ServerHttpTest, RunHttpInvalidAddressThrows) {
    EXPECT_THROW(server_->run_http("not-a-valid-address", 18071), std::runtime_error);
}

TEST_F(ServerHttpTest, RunHttpShutdownOnSignal) {
    constexpr uint16_t port = 18072;

    std::thread server_thread([&] { server_->run_http("127.0.0.1", port); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

#ifdef _WIN32
    std::raise(SIGINT);
#else
    mcp::detail::trigger_shutdown_signal();
#endif

    server_thread.join();
}
