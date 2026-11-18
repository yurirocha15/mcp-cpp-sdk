#pragma once

#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/stdio_client.hpp>

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

    /// Talk to the server at @p url over Streamable HTTP.
    static Endpoint http(std::string url, HttpEndpointOptions options = {});

    /// Use the transport @p factory makes, such as a WebSocketClientTransport or a transport of
    /// the application's own.
    /// @throws std::invalid_argument If @p factory is empty.
    static Endpoint custom(TransportFactory factory);

    /// Make the transport for the event loop @p executor belongs to.
    [[nodiscard]] std::shared_ptr<ITransport> make_transport(const Executor& executor) const;

   private:
    explicit Endpoint(TransportFactory factory);

    TransportFactory factory_;
};

}  // namespace mcp
