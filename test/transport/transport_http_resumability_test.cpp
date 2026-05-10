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
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

}  // namespace

class EventStoreTest : public ::testing::Test {};

TEST_F(EventStoreTest, AppendAndRetrieve) {
    mcp::EventStore store(10);

    auto id1 = store.append(R"({"jsonrpc":"2.0","id":1,"result":"a"})");
    auto id2 = store.append(R"({"jsonrpc":"2.0","id":2,"result":"b"})");
    auto id3 = store.append(R"({"jsonrpc":"2.0","id":3,"result":"c"})");

    EXPECT_EQ(store.size(), 3);

    auto after_id1 = store.events_after(id1);
    ASSERT_TRUE(after_id1.has_value());
    ASSERT_EQ(after_id1->size(), 2);
    EXPECT_EQ((*after_id1)[0].first, id2);
    EXPECT_EQ((*after_id1)[1].first, id3);

    auto after_id3 = store.events_after(id3);
    ASSERT_TRUE(after_id3.has_value());
    EXPECT_TRUE(after_id3->empty());
}

TEST_F(EventStoreTest, EvictionOnOverflow) {
    mcp::EventStore store(3);

    auto id1 = store.append("event-1");
    store.append("event-2");
    store.append("event-3");
    store.append("event-4");

    EXPECT_EQ(store.size(), 3);

    auto result = store.events_after(id1);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EventStoreTest, AllEvents) {
    mcp::EventStore store(10);

    store.append("a");
    store.append("b");
    store.append("c");

    auto all = store.all_events();
    ASSERT_EQ(all.size(), 3);
    EXPECT_EQ(all[0].second, "a");
    EXPECT_EQ(all[1].second, "b");
    EXPECT_EQ(all[2].second, "c");
}

TEST_F(EventStoreTest, ClearResetsState) {
    mcp::EventStore store(10);

    store.append("x");
    store.append("y");
    EXPECT_EQ(store.size(), 2);

    store.clear();
    EXPECT_EQ(store.size(), 0);

    auto all = store.all_events();
    EXPECT_TRUE(all.empty());
}

TEST_F(EventStoreTest, UnknownEventIdReturnsNullopt) {
    mcp::EventStore store(10);

    store.append("data");

    auto result = store.events_after("nonexistent-id");
    EXPECT_FALSE(result.has_value());
}

class HttpResumabilityTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

TEST_F(HttpResumabilityTest, PostResponseIncludesEventIdInSse) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18090);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto request_message = co_await server_transport.read_message();
            auto req_json = nlohmann::json::parse(request_message);
            nlohmann::json response_payload = {
                {"jsonrpc", "2.0"}, {"result", "pong"}, {"id", req_json.at("id")}};
            co_await server_transport.write_message(response_payload.dump());
        },
        asio::detached);

    bool has_event_id = false;
    std::string client_received;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            beast::tcp_stream stream(io_ctx_.get_executor());
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            auto endpoints = co_await resolver.async_resolve("127.0.0.1", "18090", asio::use_awaitable);
            co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> request{http::verb::post, "/mcp", 11};
            request.set(http::field::host, "127.0.0.1");
            request.set(http::field::content_type, "application/json");
            request.set(http::field::accept, "text/event-stream");
            request.set("MCP-Protocol-Version", std::string(mcp::g_LATEST_PROTOCOL_VERSION));
            request.body() = R"({"jsonrpc":"2.0","method":"ping","id":1})";
            request.prepare_payload();

            co_await http::async_write(stream, request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> response;
            co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

            auto content_type = std::string(response[http::field::content_type]);
            if (content_type.find("text/event-stream") != std::string::npos) {
                auto body = response.body();
                has_event_id = body.find("id: ") != std::string::npos;
                auto data_pos = body.find("data: ");
                if (data_pos != std::string::npos) {
                    auto data_start = data_pos + 6;
                    auto data_end = body.find('\n', data_start);
                    client_received = body.substr(data_start, data_end - data_start);
                }
            }

            beast::error_code shutdown_error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(has_event_id);
    auto parsed = nlohmann::json::parse(client_received);
    EXPECT_EQ(parsed["result"], "pong");
}

TEST_F(HttpResumabilityTest, EventStorePopulatedOnWrite) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18091, 100);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto request_message = co_await server_transport.read_message();
            auto req_json = nlohmann::json::parse(request_message);
            nlohmann::json response_payload = {
                {"jsonrpc", "2.0"}, {"result", "hello"}, {"id", req_json.at("id")}};
            co_await server_transport.write_message(response_payload.dump());
        },
        asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18091/mcp");

            nlohmann::json request_payload = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 1}};
            co_await client_transport.write_message(request_payload.dump());
            co_await client_transport.read_message();

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_GE(server_transport.event_store().size(), 1);
}

TEST_F(HttpResumabilityTest, GetWithLastEventIdReplaysEvents) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18092, 100);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    std::string first_event_id;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init_msg = co_await server_transport.read_message();
            auto init_json = nlohmann::json::parse(init_msg);
            nlohmann::json init_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", init_json.at("id")}};
            co_await server_transport.write_message(init_response.dump());

            auto req1_msg = co_await server_transport.read_message();
            auto req1_json = nlohmann::json::parse(req1_msg);
            nlohmann::json resp1 = {
                {"jsonrpc", "2.0"}, {"result", "first"}, {"id", req1_json.at("id")}};
            co_await server_transport.write_message(resp1.dump());

            auto req2_msg = co_await server_transport.read_message();
            auto req2_json = nlohmann::json::parse(req2_msg);
            nlohmann::json resp2 = {
                {"jsonrpc", "2.0"}, {"result", "second"}, {"id", req2_json.at("id")}};
            co_await server_transport.write_message(resp2.dump());
        },
        asio::detached);

    bool replay_received = false;
    std::string replay_body;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18092/mcp");

            nlohmann::json init_request = {
                {"jsonrpc", "2.0"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", 1}};
            co_await client_transport.write_message(init_request.dump());
            co_await client_transport.read_message();

            nlohmann::json req1 = {{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", 2}};
            co_await client_transport.write_message(req1.dump());
            co_await client_transport.read_message();

            nlohmann::json req2 = {{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", 3}};
            co_await client_transport.write_message(req2.dump());
            co_await client_transport.read_message();

            first_event_id = "1";

            beast::tcp_stream stream(io_ctx_.get_executor());
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            auto endpoints = co_await resolver.async_resolve("127.0.0.1", "18092", asio::use_awaitable);
            co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::empty_body> get_request{http::verb::get, "/mcp", 11};
            get_request.set(http::field::host, "127.0.0.1");
            get_request.set(http::field::accept, "text/event-stream");
            get_request.set("MCP-Protocol-Version", std::string(mcp::g_LATEST_PROTOCOL_VERSION));
            get_request.set("MCP-Session-Id", client_transport.session_id());
            get_request.set("Last-Event-ID", first_event_id);

            co_await http::async_write(stream, get_request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> response;
            co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

            if (response.result() == http::status::ok) {
                replay_body = response.body();
                replay_received = !replay_body.empty();
            }

            beast::error_code shutdown_error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(replay_received);
    EXPECT_NE(replay_body.find("id: 2"), std::string::npos);
    EXPECT_NE(replay_body.find("id: 3"), std::string::npos);
    EXPECT_NE(replay_body.find("\"result\":\"first\""), std::string::npos);
    EXPECT_NE(replay_body.find("\"result\":\"second\""), std::string::npos);
}

TEST_F(HttpResumabilityTest, GetWithEvictedEventIdReturns410) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18093, 2);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init_msg = co_await server_transport.read_message();
            auto init_json = nlohmann::json::parse(init_msg);
            nlohmann::json init_response = {
                {"jsonrpc", "2.0"},
                {"result",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"serverInfo", {{"name", "test-server"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", init_json.at("id")}};
            co_await server_transport.write_message(init_response.dump());

            for (int i = 2; i <= 4; ++i) {
                auto req_msg = co_await server_transport.read_message();
                auto req_json = nlohmann::json::parse(req_msg);
                nlohmann::json resp = {{"jsonrpc", "2.0"},
                                       {"result", "resp-" + std::to_string(i)},
                                       {"id", req_json.at("id")}};
                co_await server_transport.write_message(resp.dump());
            }
        },
        asio::detached);

    int get_status_code = 0;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18093/mcp");

            nlohmann::json init_request = {
                {"jsonrpc", "2.0"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                  {"capabilities", {}}}},
                {"id", 1}};
            co_await client_transport.write_message(init_request.dump());
            co_await client_transport.read_message();

            for (int i = 2; i <= 4; ++i) {
                nlohmann::json req = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", i}};
                co_await client_transport.write_message(req.dump());
                co_await client_transport.read_message();
            }

            beast::tcp_stream stream(io_ctx_.get_executor());
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            auto endpoints = co_await resolver.async_resolve("127.0.0.1", "18093", asio::use_awaitable);
            co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::empty_body> get_request{http::verb::get, "/mcp", 11};
            get_request.set(http::field::host, "127.0.0.1");
            get_request.set(http::field::accept, "text/event-stream");
            get_request.set("MCP-Protocol-Version", std::string(mcp::g_LATEST_PROTOCOL_VERSION));
            get_request.set("MCP-Session-Id", client_transport.session_id());
            get_request.set("Last-Event-ID", "1");

            co_await http::async_write(stream, get_request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> response;
            co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

            get_status_code = static_cast<int>(response.result_int());

            beast::error_code shutdown_error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(get_status_code, 410);
}

TEST_F(HttpResumabilityTest, ClientParsesEventIdFromSseResponse) {
    mcp::HttpServerTransport server_transport(io_ctx_.get_executor(), "127.0.0.1", 18094);

    asio::co_spawn(io_ctx_, server_transport.listen(), asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto request_message = co_await server_transport.read_message();
            auto req_json = nlohmann::json::parse(request_message);
            nlohmann::json response_payload = {
                {"jsonrpc", "2.0"}, {"result", "ok"}, {"id", req_json.at("id")}};
            co_await server_transport.write_message(response_payload.dump());
        },
        asio::detached);

    std::string client_received;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::HttpClientTransport client_transport(io_ctx_.get_executor(),
                                                      "http://127.0.0.1:18094/mcp");

            nlohmann::json request_payload = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 1}};
            co_await client_transport.write_message(request_payload.dump());
            client_received = co_await client_transport.read_message();

            client_transport.close();
            server_transport.close();
        },
        asio::detached);

    io_ctx_.run();

    auto parsed = nlohmann::json::parse(client_received);
    EXPECT_EQ(parsed["result"], "ok");
}
