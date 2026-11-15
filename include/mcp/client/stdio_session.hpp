#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <mcp/client/client.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace mcp {

/**
 * @brief When StdioClientSession starts the server again after it ends on its own.
 */
struct StdioRestartPolicy {
    /// How many times the server is restarted over the life of the session; zero never restarts it.
    std::size_t max_restarts = 0;
    /// How long to wait before each start.
    std::chrono::milliseconds delay{std::chrono::milliseconds(500)};
    /// Also restart a server that ends on its own with status 0. By default only an abnormal end (a
    /// non-zero status or a signal) restarts it: a server that exits cleanly of its own accord is
    /// shutting down by choice.
    bool restart_after_clean_exit = false;
};

/**
 * @brief What a StdioClientSession launches and how it connects.
 */
struct StdioSessionOptions {
    /// The server process. Its `on_exit` still runs, before the session decides on a restart.
    StdioProcessOptions process;
    /// Options of each Client the session creates.
    ClientOptions client;
    /// What each Client connects as.
    Implementation client_info;
    /// What each Client declares, in addition to what its registered handlers imply.
    ClientCapabilities capabilities;
    /// When the server is started again.
    StdioRestartPolicy restart;
    /// Runs on each new Client before it connects: register elicitation, sampling and roots
    /// handlers and notification callbacks here, so that a restarted server finds them again.
    std::function<void(Client&)> configure;
    /// Runs after each Client has connected, the first one and those after a restart: open
    /// `subscriptions/listen` streams here and they are re-established with the new process.
    std::function<Task<void>(Client&, const ConnectResult&)> on_connected;
    /// Runs when a restart does not succeed: the last start failed, or the policy allows no more.
    /// The text says why. The session then has no running server.
    std::function<void(const std::string&)> on_restart_failed;
};

/**
 * @brief A Client together with the server process it launched, restarted when it ends.
 *
 * @details The specification asks a stdio client to restart a server that exits unexpectedly: the
 * requests that were in flight are lost, and active `subscriptions/listen` streams are
 * established again. A session starts the server with a StdioClientTransport, connects a Client to
 * it, and when the process ends without the session or the transport having asked it to, starts
 * another one (as StdioSessionOptions::restart allows) and connects a new Client.
 *
 * Calls on the previous Client fail with `mcp::constants::jsonrpc::g_CONNECTION_CLOSED` and its
 * subscriptions end as SubscriptionEnd::eDropped; the application then calls the new client() again. A
 * modern-era server keeps no state between requests, so the new Client carries on from there; a legacy
 * session starts over with the initialize handshake that connect() performs.
 *
 * A server that has not finished connecting never triggers a restart: start() fails and shuts the
 * process down. At most one restart sequence runs at a time.
 */
class MCP_API StdioClientSession {
   public:
    /**
     * @brief Create a session; nothing starts until start().
     * @throws std::invalid_argument If the process command is empty.
     */
    StdioClientSession(const boost::asio::any_io_executor& executor, StdioSessionOptions options);

    /// Stops restarting and shuts the current server down.
    ~StdioClientSession();

    StdioClientSession(const StdioClientSession&) = delete;
    StdioClientSession& operator=(const StdioClientSession&) = delete;
    StdioClientSession(StdioClientSession&&) = delete;
    StdioClientSession& operator=(StdioClientSession&&) = delete;

    /**
     * @brief Launch the server and connect to it.
     * @return What the server declared.
     * @throws std::runtime_error If the process cannot start.
     * @throws McpError If the connection fails; the process is then shut down, nothing is
     *         restarted, and client() is null.
     */
    Task<ConnectResult> start();

    /// The Client of the running server, or null before start() and after stop(). After a restart
    /// it is a different object.
    [[nodiscard]] std::shared_ptr<Client> client() const;

    /// How many times the server has been started again.
    [[nodiscard]] std::size_t restart_count() const;

    /**
     * @brief Stop restarting, close the Client and shut the server down.
     * @return How the last process ended, or nothing if none ran.
     */
    Task<std::optional<ProcessExit>> stop();

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp
