#pragma once

#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace mcp {

class HttpClientTransport;
class ITransport;

namespace auth {
class Authenticator;
}  // namespace auth

/**
 * @brief How an Endpoint::http() client reaches the server, beyond its URL.
 */
struct HttpEndpointOptions {
    /// Called with the HTTP transport before the client uses it, for settings such as
    /// set_bearer_token_provider(), set_response_timeout() or set_warning_handler().
    std::function<void(HttpClientTransport&)> configure;
    /// Makes the authenticator whose tokens the client sends, given the executor the client runs
    /// on; the client then talks to the server through an auth::OAuthClientTransport. Unset sends
    /// no authorization.
    std::function<std::shared_ptr<auth::Authenticator>(const Executor&)> authenticator;
};

/**
 * @brief The server a Client connects to: a process it launches, an HTTP URL, or a transport
 *        the application makes.
 *
 * @details A Client constructed from an Endpoint runs its own event loop on a thread of its own,
 * so it can be used without an executor, through Client::sync() or Client::run().
 */
class MCP_API Endpoint {
   public:
    /// Makes the transport for the event loop whose executor it is given.
    using TransportFactory = std::function<std::shared_ptr<ITransport>(const Executor&)>;

    /**
     * @brief Launch @p command, the program followed by its arguments, and talk to it over its
     *        standard input and output.
     *
     * @details A program without a directory is searched for in `PATH`; one with a directory is
     * taken relative to the working directory.
     * @throws std::invalid_argument If @p command is empty.
     */
    static Endpoint stdio(std::vector<std::string> command);

    /// As stdio() above, for a command written in place, such as
    /// `Endpoint::stdio({"./server", "--verbose"})` or `Endpoint::stdio({"./server"})`.
    static Endpoint stdio(std::initializer_list<std::string> command);

    /// Launch the process @p process describes, with its environment, working directory and
    /// standard error handling.
    /// @throws std::invalid_argument If `process.command` is empty.
    static Endpoint stdio(StdioProcessOptions process);

    /**
     * @brief Launch a server as stdio() above, and start it again as @p restart allows when it
     *        ends without the client having asked it to, such as
     *        `Endpoint::stdio({"./server"}, {.max_restarts = 3})`.
     *
     * @details The same Client goes on with the new process, keeping its handlers, callbacks and
     * sync() view. The requests waiting for an answer when the process ended fail with McpError
     * (mcp::constants::jsonrpc::g_CONNECTION_CLOSED); requests made while it starts again wait for
     * it, within their own timeouts. A legacy session sends its initialize request to the new
     * process again. Each open `subscriptions/listen` request is sent to the new process again, so
     * its Subscription goes on. What the old process kept for a legacy session, such as a log
     * level, resource subscriptions or tasks, is gone. A start that fails uses one of the restarts
     * the policy allows; when it allows no more, the client closes as it does without a restart.
     * StdioProcessOptions::on_exit runs for each process that ends.
     */
    static Endpoint stdio(std::vector<std::string> command, StdioRestartPolicy restart);
    /// As the stdio() overload above that takes a restart policy.
    static Endpoint stdio(std::initializer_list<std::string> command, StdioRestartPolicy restart);
    /// As the stdio() overload above that takes a restart policy.
    static Endpoint stdio(StdioProcessOptions process, StdioRestartPolicy restart);

    /// Talk to the server at @p url over Streamable HTTP.
    static Endpoint http(std::string url, HttpEndpointOptions options = {});

    /// Use the transport @p factory makes, such as a WebSocketClientTransport or a transport of
    /// the application's own.
    /// @throws std::invalid_argument If @p factory is empty.
    static Endpoint custom(TransportFactory factory);

    /// Make the transport for the event loop @p executor belongs to.
    [[nodiscard]] std::shared_ptr<ITransport> make_transport(const Executor& executor) const;

   private:
    friend class Client;

    explicit Endpoint(TransportFactory factory);

    TransportFactory factory_;
    /// How long a launched server may take to end once its standard input closes: the waits of
    /// its StdioProcessOptions. Zero for the other endpoints.
    std::chrono::milliseconds close_wait_{0};
};

}  // namespace mcp
