#include <gtest/gtest.h>

#include "mcp/client.hpp"
#include "mcp/server.hpp"
#include "mcp/transport/memory.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <string>

class E2EFeaturesTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

// ========== MemoryTransportPair ==========
TEST_F(E2EFeaturesTest, MemoryTransportPair) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.add_tool("test_tool", "A test tool", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "ok"}}})}};
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server.run(server_transport, io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            mcp::Client client(client_transport, io_ctx_.get_executor());
            auto init_res = co_await client.connect("test-client", "1.0");
            EXPECT_EQ(init_res.serverInfo.name, "test-server");

            auto result = co_await client.call_tool("test_tool", nlohmann::json::object());
            EXPECT_GT(result.content.size(), 0);

            co_await client.send_request("shutdown", std::nullopt);
            client.close();
            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

// ========== ConnectAndDisconnect ==========
TEST_F(E2EFeaturesTest, ConnectAndDisconnect) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.add_tool("dummy", "Dummy tool", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "ok"}}})}};
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server.run(server_transport, io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            mcp::Client client(client_transport, io_ctx_.get_executor());
            auto init_res = co_await client.connect("test-client", "1.0");
            EXPECT_EQ(init_res.serverInfo.name, "test-server");

            co_await client.send_request("shutdown", std::nullopt);
            client.close();
            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

// ========== ListTools ==========
TEST_F(E2EFeaturesTest, ListTools) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.add_tool("tool1", "First tool", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "ok"}}})}};
    });
    server.add_tool("tool2", "Second tool", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "ok"}}})}};
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server.run(server_transport, io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            mcp::Client client(client_transport, io_ctx_.get_executor());
            co_await client.connect("test-client", "1.0");

            auto result = co_await client.list_tools();
            EXPECT_GE(result.tools.size(), 2);

            co_await client.send_request("shutdown", std::nullopt);
            client.close();
            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

// ========== Pagination ==========
TEST_F(E2EFeaturesTest, Pagination) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.set_page_size(3);

    for (int i = 0; i < 10; ++i) {
        server.add_tool("tool_" + std::to_string(i), "Tool " + std::to_string(i), {},
                        [](const nlohmann::json&) {
                            return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                                                  {"type", "text"}, {"text", "ok"}}})}};
                        });
    }

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server.run(server_transport, io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            mcp::Client client(client_transport, io_ctx_.get_executor());
            co_await client.connect("test-client", "1.0");

            auto result = co_await client.list_tools();
            EXPECT_LE(result.tools.size(), 3);

            co_await client.send_request("shutdown", std::nullopt);
            client.close();
            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

// ========== Ping ==========
TEST_F(E2EFeaturesTest, Ping) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.add_tool("test_tool", "A test tool", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "ok"}}})}};
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server.run(server_transport, io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            mcp::Client client(client_transport, io_ctx_.get_executor());
            co_await client.connect("test-client", "1.0");

            co_await client.ping();

            co_await client.send_request("shutdown", std::nullopt);
            client.close();
            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

// ========== MultipleTools ==========
TEST_F(E2EFeaturesTest, MultipleTools) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.add_tool("add", "Add two numbers", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "3"}}})}};
    });
    server.add_tool("multiply", "Multiply two numbers", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "6"}}})}};
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server.run(server_transport, io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            mcp::Client client(client_transport, io_ctx_.get_executor());
            co_await client.connect("test-client", "1.0");

            auto add_result = co_await client.call_tool("add", nlohmann::json::object());
            EXPECT_GT(add_result.content.size(), 0);

            auto mul_result = co_await client.call_tool("multiply", nlohmann::json::object());
            EXPECT_GT(mul_result.content.size(), 0);

            co_await client.send_request("shutdown", std::nullopt);
            client.close();
            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

// ========== SequentialCalls ==========
TEST_F(E2EFeaturesTest, SequentialCalls) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.add_tool("echo", "Echo tool", {}, [](const nlohmann::json&) {
        return nlohmann::json{
            {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "echo"}}})}};
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server.run(server_transport, io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            mcp::Client client(client_transport, io_ctx_.get_executor());
            co_await client.connect("test-client", "1.0");

            auto r1 = co_await client.call_tool("echo", nlohmann::json::object());
            EXPECT_GT(r1.content.size(), 0);

            auto r2 = co_await client.call_tool("echo", nlohmann::json::object());
            EXPECT_GT(r2.content.size(), 0);

            auto r3 = co_await client.call_tool("echo", nlohmann::json::object());
            EXPECT_GT(r3.content.size(), 0);

            co_await client.send_request("shutdown", std::nullopt);
            client.close();
            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}
