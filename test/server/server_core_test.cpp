#include "../test_utils.hpp"

#include "mcp/core/context.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class StrandCloseProbeTransport final : public mcp::ITransport {
   public:
    explicit StrandCloseProbeTransport(
        boost::asio::strand<boost::asio::any_io_executor> expected_strand)
        : expected_strand_(std::move(expected_strand)) {}

    mcp::Task<std::string> read_message() override {
        if (reads_.fetch_add(1, std::memory_order_relaxed) == 0) {
            co_return nlohmann::json{{"jsonrpc", "2.0"}, {"id", "ping"}, {"method", "ping"}}.dump();
        }
        throw std::runtime_error("script complete");
    }

    mcp::Task<void> write_message(std::string_view) override { co_return; }

    void close() override {
        close_calls_.fetch_add(1, std::memory_order_relaxed);
        if (!expected_strand_.running_in_this_thread()) {
            off_strand_closes_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::size_t close_calls() const { return close_calls_.load(); }
    [[nodiscard]] std::size_t off_strand_closes() const { return off_strand_closes_.load(); }

   private:
    boost::asio::strand<boost::asio::any_io_executor> expected_strand_;
    std::atomic_size_t reads_{0};
    std::atomic_size_t close_calls_{0};
    std::atomic_size_t off_strand_closes_{0};
};

void run_server_io_on_thread_pool(boost::asio::io_context& io_context, std::size_t thread_count = 4) {
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&io_context]() { io_context.run(); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

mcp::Task<void> verify_memory_transport_parse_recovery(
    std::shared_ptr<mcp::ITransport> client_transport, std::vector<nlohmann::json>* responses) {
    const auto malformed_wire = std::make_shared<const std::string>("{");
    co_await client_transport->write_message(*malformed_wire);
    {
        auto response_wire = co_await client_transport->read_message();
        responses->push_back(nlohmann::json::parse(response_wire));
    }

    const auto ping_wire = std::make_shared<const std::string>(
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}}.dump());
    co_await client_transport->write_message(*ping_wire);
    {
        auto response_wire = co_await client_transport->read_message();
        responses->push_back(nlohmann::json::parse(response_wire));
    }
    client_transport->close();
}

}  // namespace

class ServerCoreTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerCoreTest, InitializeReturnsServerCapabilities) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    tools_cap.listChanged = true;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(server_info), std::move(caps));

    nlohmann::json response;
    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json init_req = make_initialize_request("1");
    raw_transport->enqueue_message(init_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(response.contains("result"));
    ASSERT_TRUE(response.contains("id"));
    EXPECT_EQ(response["id"], "1");
    EXPECT_EQ(response["jsonrpc"], "2.0");

    auto result = response["result"];
    EXPECT_EQ(result["protocolVersion"], std::string(mcp::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(result["serverInfo"]["name"], "test-server");
    EXPECT_EQ(result["serverInfo"]["version"], "1.0");
    ASSERT_TRUE(result["capabilities"].contains("tools"));
    EXPECT_TRUE(result["capabilities"]["tools"]["listChanged"]);

    EXPECT_TRUE(server.is_initialized());
}

TEST_F(ServerCoreTest, InitializeNegotiatesSupportedClientProtocolVersion) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json init_req = make_initialize_request("1");
    init_req["params"]["protocolVersion"] = "2025-06-18";
    raw_transport->enqueue_message(init_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["result"]["protocolVersion"], "2025-06-18");
}

TEST_F(ServerCoreTest, ShutdownSetsFlag) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    nlohmann::json init_req = make_initialize_request("1");
    nlohmann::json shutdown_req = make_shutdown_request("2");
    raw_transport->enqueue_message(init_req.dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(shutdown_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);

    EXPECT_EQ(responses[0]["id"], "1");
    ASSERT_TRUE(responses[0].contains("result"));

    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());

    EXPECT_TRUE(server.is_initialized());
    EXPECT_TRUE(server.is_shutdown_requested());
}

TEST_F(ServerCoreTest, UnknownMethodReturnsError) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    nlohmann::json unknown_req = {{"jsonrpc", "2.0"}, {"id", "42"}, {"method", "nonexistent/method"}};
    raw_transport->enqueue_message(make_initialize_request("1").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(unknown_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    ASSERT_TRUE(responses[1].contains("error"));
    EXPECT_EQ(responses[1]["id"], "42");
    EXPECT_EQ(responses[1]["error"]["code"], mcp::g_METHOD_NOT_FOUND);
    EXPECT_TRUE(responses[1]["error"]["message"].get<std::string>().find("nonexistent/method") !=
                std::string::npos);
}

TEST_F(ServerCoreTest, NotificationsAreSilentlyIgnored) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    bool got_response = false;
    raw_transport->set_on_write([&response, &got_response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        got_response = true;
        raw_transport->close();
    });

    nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    nlohmann::json init_req = make_initialize_request("1");
    raw_transport->enqueue_message(notification.dump());
    raw_transport->enqueue_message(init_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(got_response);
    EXPECT_EQ(response["id"], "1");
    ASSERT_TRUE(response.contains("result"));
}

TEST_F(ServerCoreTest, ContextLogInfoSendsNotification) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());

    nlohmann::json notification;
    raw_transport->set_on_write(
        [&notification](std::string_view msg) { notification = nlohmann::json::parse(msg); });

    mcp::Context ctx(*raw_transport);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await ctx.log_info("hello world");
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(notification["jsonrpc"], "2.0");
    EXPECT_EQ(notification["method"], "notifications/message");
    ASSERT_TRUE(notification.contains("params"));
    EXPECT_EQ(notification["params"]["level"], "info");
    EXPECT_EQ(notification["params"]["data"], "hello world");
    EXPECT_FALSE(notification.contains("id"));
}

TEST_F(ServerCoreTest, ContextLogInfoMultipleMessages) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());

    std::vector<nlohmann::json> notifications;
    raw_transport->set_on_write([&notifications](std::string_view msg) {
        notifications.push_back(nlohmann::json::parse(msg));
    });

    mcp::Context ctx(*raw_transport);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await ctx.log_info("first");
            co_await ctx.log_info("second");
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(notifications.size(), 2);
    EXPECT_EQ(notifications[0]["params"]["data"], "first");
    EXPECT_EQ(notifications[1]["params"]["data"], "second");
}

TEST_F(ServerCoreTest, DispatchDirectlyWithoutRun) {
    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    raw_transport->set_on_write(
        [&response](std::string_view msg) { response = nlohmann::json::parse(msg); });

    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json init_req = make_initialize_request("10");
    raw_transport->enqueue_message(init_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response["id"], "10");
    ASSERT_TRUE(response.contains("result"));
    EXPECT_FALSE(server.is_shutdown_requested());
}

TEST_F(ServerCoreTest, PingHandlerReturnsEmptyResult) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json ping_req = {{"jsonrpc", "2.0"}, {"id", "99"}, {"method", "ping"}};
    raw_transport->enqueue_message(ping_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"], "99");
    EXPECT_EQ(response["result"], nlohmann::json::object());
}

TEST_F(ServerCoreTest, RequestsRequireCompletedInitializationHandshake) {
    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";
    mcp::Server server(info, mcp::ServerCapabilities{});

    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();
    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view message) {
        responses.push_back(nlohmann::json::parse(message));
        if (responses.size() == 6) {
            raw_transport->close();
        }
    });

    auto list_request = nlohmann::json{{"jsonrpc", "2.0"}, {"id", "list"}, {"method", "tools/list"}};
    raw_transport->enqueue_message(list_request.dump());
    raw_transport->enqueue_message(make_initialize_request("init").dump());
    raw_transport->enqueue_message(list_request.dump());
    raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "1.0"}, {"method", "notifications/initialized"}}.dump());
    raw_transport->enqueue_message(nlohmann::json{{"jsonrpc", "2.0"}, {"method", 7}}.dump());
    raw_transport->enqueue_message(list_request.dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    raw_transport->enqueue_message(list_request.dump());
    raw_transport->enqueue_message(make_initialize_request("duplicate").dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 6);
    EXPECT_EQ(responses[0]["error"]["code"], mcp::g_INVALID_REQUEST);
    EXPECT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[2]["error"]["code"], mcp::g_INVALID_REQUEST);
    EXPECT_EQ(responses[3]["error"]["code"], mcp::g_INVALID_REQUEST);
    EXPECT_TRUE(responses[4]["result"]["tools"].empty());
    EXPECT_EQ(responses[5]["error"]["code"], mcp::g_INVALID_REQUEST);
    EXPECT_TRUE(server.is_initialized());
}

TEST_F(ServerCoreTest, InvalidJsonRpcEnvelopesAreRejected) {
    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";
    mcp::Server server(info, mcp::ServerCapabilities{});

    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();
    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view message) {
        responses.push_back(nlohmann::json::parse(message));
        if (responses.size() == 4) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "1.0"}, {"id", "response"}, {"result", nlohmann::json::object()}}
            .dump());
    raw_transport->enqueue_message(nlohmann::json{{"jsonrpc", "2.0"},
                                                  {"id", "response"},
                                                  {"result", nlohmann::json::object()},
                                                  {"error", {{"code", -1}, {"message", "bad"}}}}
                                       .dump());
    raw_transport->enqueue_message(
        nlohmann::json{{"id", "missing-version"}, {"method", "ping"}}.dump());
    raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "1.0"}, {"id", "wrong-version"}, {"method", "ping"}}.dump());
    raw_transport->enqueue_message(nlohmann::json{{"jsonrpc", "2.0"}, {"id", "missing-method"}}.dump());
    raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", "bad-method"}, {"method", 7}}.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 4);
    for (const auto& response : responses) {
        EXPECT_EQ(response["error"]["code"], mcp::g_INVALID_REQUEST);
    }
}

TEST_F(ServerCoreTest, MalformedAndNonObjectMessagesReturnErrorsThenAcceptValidRequest) {
    mcp::Server server({"validation-server", "1.0"}, mcp::ServerCapabilities{});
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view message) {
        responses.push_back(nlohmann::json::parse(message));
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(R"({"jsonrpc":"2.0","id":)");
    raw_transport->enqueue_message(nlohmann::json::array().dump());
    raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", "ping"}, {"method", "ping"}}.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);
    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);
    EXPECT_EQ(responses[0]["error"]["code"], mcp::g_PARSE_ERROR);
    EXPECT_TRUE(responses[0]["id"].is_null());
    EXPECT_EQ(responses[1]["error"]["code"], mcp::g_INVALID_REQUEST);
    EXPECT_TRUE(responses[1]["id"].is_null());
    EXPECT_EQ(responses[2]["id"], "ping");
    EXPECT_EQ(responses[2]["result"], nlohmann::json::object());
}

TEST_F(ServerCoreTest, MemoryTransportRecoversFromMalformedJson) {
    mcp::Server server({"validation-server", "1.0"}, mcp::ServerCapabilities{});
    auto [server_transport, client_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    std::vector<nlohmann::json> responses;
    std::exception_ptr server_error;
    std::exception_ptr client_error;
    boost::asio::co_spawn(
        io_ctx_, server.run(server_transport, io_ctx_.get_executor()),
        [&server_error](std::exception_ptr error) { server_error = std::move(error); });
    boost::asio::co_spawn(
        io_ctx_, verify_memory_transport_parse_recovery(client_transport, &responses),
        [&client_error](std::exception_ptr error) { client_error = std::move(error); });

    io_ctx_.run();

    EXPECT_EQ(server_error, nullptr);
    EXPECT_EQ(client_error, nullptr);
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0]["error"]["code"], mcp::g_PARSE_ERROR);
    EXPECT_TRUE(responses[0]["id"].is_null());
    EXPECT_EQ(responses[1]["id"], 7);
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());
}

TEST_F(ServerCoreTest, ParameterDecodingErrorsRemainDistinctFromHandlerFailures) {
    mcp::Server server({"validation-server", "1.0"}, mcp::ServerCapabilities{});
    mcp::Resource resource;
    resource.uri = "file:///failure.txt";
    resource.name = "failure";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        resource, [](mcp::ReadResourceRequestParams) -> mcp::ReadResourceResult {
            throw std::runtime_error("resource handler failed");
        });
    server.set_page_size(1);

    std::vector<nlohmann::json> requests = {
        {{"jsonrpc", "2.0"},
         {"id", "resource-params"},
         {"method", "resources/read"},
         {"params", nlohmann::json::object()}},
        {{"jsonrpc", "2.0"},
         {"id", "subscribe-params"},
         {"method", "resources/subscribe"},
         {"params", nlohmann::json::object()}},
        {{"jsonrpc", "2.0"},
         {"id", "prompt-params"},
         {"method", "prompts/get"},
         {"params", {{"name", 7}}}},
        {{"jsonrpc", "2.0"},
         {"id", "logging-params"},
         {"method", "logging/setLevel"},
         {"params", nlohmann::json::object()}},
        {{"jsonrpc", "2.0"},
         {"id", "completion-params"},
         {"method", "completion/complete"},
         {"params", nlohmann::json::object()}},
        {{"jsonrpc", "2.0"},
         {"id", "pagination-params"},
         {"method", "resources/list"},
         {"params", {{"cursor", 7}}}},
        {{"jsonrpc", "2.0"},
         {"id", "handler"},
         {"method", "resources/read"},
         {"params", {{"uri", "file:///failure.txt"}}}},
    };

    std::vector<nlohmann::json> responses;
    std::exception_ptr dispatch_error;
    boost::asio::co_spawn(
        io_ctx_,
        [&server, &requests, &responses]() -> mcp::Task<void> {
            for (auto& request : requests) {
                responses.push_back(
                    nlohmann::json::parse(co_await server.dispatch_request_direct(std::move(request))));
            }
        },
        [&dispatch_error](std::exception_ptr error) { dispatch_error = std::move(error); });

    io_ctx_.run();

    EXPECT_EQ(dispatch_error, nullptr);
    ASSERT_EQ(responses.size(), requests.size());
    for (std::size_t index = 0; index + 1 < responses.size(); ++index) {
        EXPECT_EQ(responses[index]["error"]["code"], mcp::g_INVALID_PARAMS);
    }
    EXPECT_EQ(responses.back()["error"]["code"], mcp::g_INTERNAL_ERROR);
    EXPECT_EQ(responses.back()["error"]["message"], "resource handler failed");
}

TEST_F(ServerCoreTest, DirectDispatchDoesNotRequireStatefulHandshake) {
    mcp::Implementation info;
    info.name = "stateless-server";
    info.version = "1.0";
    mcp::Server server(info, mcp::ServerCapabilities{});

    nlohmann::json response;
    std::exception_ptr error;
    boost::asio::co_spawn(io_ctx_,
                          server.dispatch_request_direct(nlohmann::json{
                              {"jsonrpc", "2.0"}, {"id", "list"}, {"method", "tools/list"}}),
                          [&response, &error](std::exception_ptr dispatch_error, std::string wire) {
                              error = std::move(dispatch_error);
                              if (error) {
                                  return;
                              }
                              response = nlohmann::json::parse(wire);
                          });

    io_ctx_.run();

    EXPECT_EQ(error, nullptr);
    EXPECT_TRUE(response["result"]["tools"].empty());
}

TEST_F(ServerCoreTest, InitializeRequiresCompleteTypedParameters) {
    mcp::Server server({"validation-server", "1.0"}, mcp::ServerCapabilities{});

    nlohmann::json response;
    std::exception_ptr error;
    boost::asio::co_spawn(
        io_ctx_,
        server.dispatch_request_direct(nlohmann::json{{"jsonrpc", "2.0"},
                                                      {"id", "init"},
                                                      {"method", "initialize"},
                                                      {"params", nlohmann::json::object()}}),
        [&response, &error](std::exception_ptr dispatch_error, std::string wire) {
            error = std::move(dispatch_error);
            if (!error) {
                response = nlohmann::json::parse(wire);
            }
        });

    io_ctx_.run();

    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(response["id"], "init");
    EXPECT_EQ(response["error"]["code"], mcp::g_INVALID_PARAMS);
}

TEST_F(ServerCoreTest, ToolArgumentsMustBeAnObject) {
    mcp::Server server({"validation-server", "1.0"}, mcp::ServerCapabilities{});

    nlohmann::json response;
    std::exception_ptr error;
    boost::asio::co_spawn(io_ctx_,
                          server.dispatch_request_direct(
                              nlohmann::json{{"jsonrpc", "2.0"},
                                             {"id", "tool"},
                                             {"method", "tools/call"},
                                             {"params", {{"name", "echo"}, {"arguments", nullptr}}}}),
                          [&response, &error](std::exception_ptr dispatch_error, std::string wire) {
                              error = std::move(dispatch_error);
                              if (!error) {
                                  response = nlohmann::json::parse(wire);
                              }
                          });

    io_ctx_.run();

    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(response["id"], "tool");
    EXPECT_EQ(response["error"]["code"], mcp::g_INVALID_PARAMS);
}

TEST_F(ServerCoreTest, RunWaitsForDispatchedHandlersBeforeReturning) {
    using namespace std::chrono_literals;

    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    mcp::Server server({"draining-server", "1.0"}, capabilities);
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());

    bool handler_started = false;
    bool handler_completed = false;
    server.add_tool<nlohmann::json, nlohmann::json>(
        "delayed", "Delayed result", nlohmann::json{{"type", "object"}},
        [this, transport, &handler_started,
         &handler_completed](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
            handler_started = true;
            transport->close();
            boost::asio::steady_timer delay(io_ctx_, 10ms);
            co_await delay.async_wait(boost::asio::use_awaitable);
            handler_completed = true;
            co_return nlohmann::json{{"done", true}};
        });

    transport->enqueue_message(make_initialize_request("init").dump());
    transport->enqueue_message(make_initialized_notification().dump());
    transport->enqueue_message(make_tool_call_request("tool", "delayed").dump());

    bool run_completed = false;
    boost::asio::co_spawn(io_ctx_, server.run(transport, io_ctx_.get_executor()),
                          [&run_completed](std::exception_ptr error) {
                              EXPECT_EQ(error, nullptr);
                              run_completed = true;
                          });

    io_ctx_.run();

    EXPECT_TRUE(handler_started);
    EXPECT_TRUE(handler_completed);
    EXPECT_TRUE(run_completed);
}

TEST_F(ServerCoreTest, SessionTeardownStaysOnConfiguredStrandAcrossRepeatedRuns) {
    constexpr std::size_t run_count = 64;
    auto configured_strand = boost::asio::make_strand(io_ctx_.get_executor());
    mcp::Server server({"threaded-server", "1.0"}, mcp::ServerCapabilities{});

    std::vector<std::shared_ptr<StrandCloseProbeTransport>> transports;
    transports.reserve(run_count);
    std::exception_ptr run_error;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            for (std::size_t index = 0; index < run_count; ++index) {
                auto transport = std::make_shared<StrandCloseProbeTransport>(configured_strand);
                transports.push_back(transport);
                co_await server.run(transport, configured_strand);
            }
        },
        [&run_error](std::exception_ptr error) { run_error = std::move(error); });

    run_server_io_on_thread_pool(io_ctx_);

    EXPECT_EQ(run_error, nullptr);
    ASSERT_EQ(transports.size(), run_count);
    for (const auto& transport : transports) {
        EXPECT_GE(transport->close_calls(), 1);
        EXPECT_EQ(transport->off_strand_closes(), 0);
    }
}

TEST_F(ServerCoreTest, ConcurrentReverseRequestsRemainCorrelatedOnMultiThreadedExecutor) {
    using namespace std::chrono_literals;

    constexpr std::size_t request_count = 256;
    auto [server_transport, client_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());
    mcp::Server server({"reverse-rpc-server", "1.0"}, mcp::ServerCapabilities{});

    std::atomic_bool session_ready{false};
    std::atomic_bool stop_poll{false};
    std::atomic_size_t completed{0};
    std::atomic_size_t correct{0};
    std::atomic_size_t errors{0};

    boost::asio::co_spawn(io_ctx_, server.run(server_transport, io_ctx_.get_executor()),
                          [&errors](std::exception_ptr error) {
                              if (error) {
                                  errors.fetch_add(1, std::memory_order_relaxed);
                              }
                          });

    boost::asio::co_spawn(
        io_ctx_,
        [client_transport, &session_ready]() -> mcp::Task<void> {
            co_await client_transport->write_message(make_initialize_request("init").dump());
            static_cast<void>(co_await client_transport->read_message());
            co_await client_transport->write_message(make_initialized_notification().dump());
            session_ready.store(true, std::memory_order_release);

            for (std::size_t response_count = 0; response_count < request_count;) {
                auto request = nlohmann::json::parse(co_await client_transport->read_message());
                if (!request.contains("id")) {
                    continue;
                }
                auto result = nlohmann::json{{"sequence", request.at("params").at("sequence")}};
                co_await client_transport->write_message(
                    make_result_response(request.at("id").get<std::string>(), std::move(result))
                        .dump());
                ++response_count;
            }
        },
        [&errors](std::exception_ptr error) {
            if (error) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });

    boost::asio::steady_timer launch_poll(io_ctx_);
    std::function<void()> launch_requests;
    launch_requests = [&]() {
        if (!session_ready.load(std::memory_order_acquire)) {
            launch_poll.expires_after(1ms);
            launch_poll.async_wait([&launch_requests](const boost::system::error_code& error) {
                if (!error) {
                    launch_requests();
                }
            });
            return;
        }

        for (std::size_t sequence = 0; sequence < request_count; ++sequence) {
            boost::asio::co_spawn(
                io_ctx_,
                server.send_request("sampling/createMessage", nlohmann::json{{"sequence", sequence}}),
                [sequence, &completed, &correct, &errors](std::exception_ptr error,
                                                          nlohmann::json result) {
                    if (error) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    } else if (result.at("sequence").get<std::size_t>() == sequence) {
                        correct.fetch_add(1, std::memory_order_relaxed);
                    }
                    completed.fetch_add(1, std::memory_order_release);
                });
        }
    };
    launch_requests();

    boost::asio::steady_timer watchdog(io_ctx_);
    watchdog.expires_after(5s);
    watchdog.async_wait(
        [&completed, &stop_poll, client_transport](const boost::system::error_code& error) {
            if (!error && completed.load(std::memory_order_acquire) != request_count) {
                stop_poll.store(true, std::memory_order_release);
                client_transport->close();
            }
        });

    boost::asio::steady_timer completion_poll(io_ctx_);
    std::function<void()> poll_completion;
    poll_completion = [&]() {
        if (stop_poll.load(std::memory_order_acquire)) {
            return;
        }
        if (completed.load(std::memory_order_acquire) == request_count) {
            stop_poll.store(true, std::memory_order_release);
            watchdog.cancel();
            client_transport->close();
            return;
        }
        completion_poll.expires_after(1ms);
        completion_poll.async_wait([&poll_completion](const boost::system::error_code& error) {
            if (!error) {
                poll_completion();
            }
        });
    };
    poll_completion();

    run_server_io_on_thread_pool(io_ctx_);

    EXPECT_EQ(completed.load(), request_count);
    EXPECT_EQ(correct.load(), request_count);
    EXPECT_EQ(errors.load(), 0);
}
