#pragma once

// MCP servers over HTTP on a loopback port for transport tests: the session manager and the HTTP
// server transport, each serving a greet tool on an io_context of its own, on a thread of its own.

#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include "support/hang_guard.hpp"
#include "support/spawn.hpp"
#include "support/tls_test_support.hpp"

#include <boost/asio/post.hpp>

#include <functional>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace mcp_test {

/// A server with one tool, greet, that answers {"greeting": "hello"}.
inline std::unique_ptr<mcp::Server> make_greet_server() {
    mcp::Implementation info;
    info.name = "greet-server";
    info.version = "1.0";
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(std::move(info), std::move(capabilities));
    server->add_tool<nlohmann::json, nlohmann::json>(
        "greet", "Greets", nlohmann::json{{"type", "object"}},
        [](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
            co_return nlohmann::json{{"greeting", "hello"}};
        });
    return server;
}

/// A StreamableHttpSessionManager serving greet servers, configured before it listens.
class ManagerHost {
   public:
    using Configure = std::function<void(mcp::StreamableHttpSessionManager&)>;

    explicit ManagerHost(const Configure& configure, const std::string& address = "127.0.0.1") {
        manager_ = std::make_unique<mcp::StreamableHttpSessionManager>(
            thread_.io().get_executor(), address, 0,
            [](const mcp::Executor&) { return make_greet_server(); });
        if (configure) {
            configure(*manager_);
        }
        listening_ = spawn_future(thread_.io(), manager_->listen());
        thread_.start();
    }

    ManagerHost(const ManagerHost&) = delete;
    ManagerHost& operator=(const ManagerHost&) = delete;

    /// Closes the manager and waits for it to stop listening before the thread stops.
    ~ManagerHost() {
        boost::asio::post(thread_.io(), [this] { manager_->close(); });
        (void)listening_.wait_for(g_hang_guard);
        thread_.stop();
    }

    [[nodiscard]] unsigned short port() const { return manager_->port(); }
    [[nodiscard]] mcp::StreamableHttpSessionManager& manager() const { return *manager_; }

   private:
    tls::ServerThread thread_;
    std::unique_ptr<mcp::StreamableHttpSessionManager> manager_;
    std::future<void> listening_;
};

/// An HttpServerTransport serving a greet server, configured before it listens.
class TransportHost {
   public:
    using Configure = std::function<void(mcp::HttpServerTransport&)>;

    explicit TransportHost(const Configure& configure, const std::string& address = "127.0.0.1") {
        transport_ =
            std::make_shared<mcp::HttpServerTransport>(thread_.io().get_executor(), address, 0);
        if (configure) {
            configure(*transport_);
        }
        listening_ = spawn_future(thread_.io(), transport_->listen());
        running_ = spawn_future(thread_.io(), [this]() -> mcp::Task<void> {
            co_await server_->run(transport_, thread_.io().get_executor());
        });
        thread_.start();
    }

    TransportHost(const TransportHost&) = delete;
    TransportHost& operator=(const TransportHost&) = delete;

    /// Closes the transport and waits for the server and the listener to finish before the thread
    /// stops.
    ~TransportHost() {
        boost::asio::post(thread_.io(), [this] { transport_->close(); });
        (void)listening_.wait_for(g_hang_guard);
        (void)running_.wait_for(g_hang_guard);
        thread_.stop();
    }

    [[nodiscard]] unsigned short port() const { return transport_->port(); }
    [[nodiscard]] mcp::HttpServerTransport& transport() const { return *transport_; }

   private:
    tls::ServerThread thread_;
    std::unique_ptr<mcp::Server> server_ = make_greet_server();
    std::shared_ptr<mcp::HttpServerTransport> transport_;
    std::future<void> listening_;
    std::future<void> running_;
};

}  // namespace mcp_test
