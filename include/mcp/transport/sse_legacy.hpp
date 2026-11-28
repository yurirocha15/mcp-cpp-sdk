#pragma once

#include <mcp/core/deprecation.hpp>
#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/proxy.hpp>
#include <mcp/transport/tls.hpp>
#include <mcp/transport/transport.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mcp {

/**
 * @brief Where StreamableHttpSessionManager serves the HTTP+SSE transport of protocol revision
 *        2024-11-05; see StreamableHttpSessionManager::set_legacy_sse().
 */
struct LegacySseOptions {
    /// The path a client opens its event stream on with GET.
    std::string sse_path{"/sse"};
    /// The path a client POSTs its messages to; the `endpoint` event names it with the session's
    /// `sessionId` query parameter.
    std::string message_path{"/messages"};
};

/**
 * @brief Client side of the HTTP+SSE transport of protocol revision 2024-11-05, which the
 *        specification deprecates in favour of Streamable HTTP.
 *
 * @details The transport opens an event stream with GET on the URL and an `Accept` of
 * `text/event-stream`. The first event must be `endpoint`; its data is the URL to POST messages
 * to, resolved against the stream's URL. An endpoint with another scheme, host or port than the
 * stream's URL is refused, so a server cannot send the client's messages elsewhere. Each outgoing
 * message is POSTed to the endpoint, which answers 202, and the server's messages arrive as
 * `message` events on the stream. Nothing resumes the stream: when it ends, the transport is
 * closed.
 *
 * An `https://` URL is reached over TLS, with the server's certificate checked against the
 * system's certificate authorities (see set_tls()). The Client that connects over this transport
 * reports the deprecation once (DeprecatedFeature::eHttpSse).
 *
 * Only revisions before 2026-07-28 use this transport; a 2026-07-28 request cannot be carried.
 */
class MCP_API LegacySseClientTransport final : public ITransport {
   public:
    /**
     * @brief Construct a transport for the event stream at @p url.
     *
     * @param executor Executor used for async operations.
     * @param url The event stream's URL, `http://` or `https://`. An IPv6 host is written in
     *        brackets.
     *
     * @throws std::invalid_argument If the URL is malformed, carries credentials, or uses another
     *         scheme.
     *
     * @deprecated The specification deprecates the HTTP+SSE transport (SEP-2596) since protocol
     * revision 2025-03-26; it may remove it three months after SEP-2596 reaches Final. See the
     * Deprecations guide.
     */
    MCP_DEPRECATED(
        "SEP-2596: The HTTP+SSE transport is deprecated since protocol revision 2025-03-26; "
        "earliest removal: three months after SEP-2596 reaches Final; use Streamable HTTP")
    LegacySseClientTransport(const Executor& executor, const std::string& url);

    ~LegacySseClientTransport() override;

    LegacySseClientTransport(const LegacySseClientTransport&) = delete;
    LegacySseClientTransport& operator=(const LegacySseClientTransport&) = delete;
    LegacySseClientTransport(LegacySseClientTransport&&) = delete;
    LegacySseClientTransport& operator=(LegacySseClientTransport&&) = delete;

    /**
     * @brief Configure a provider for HTTP Authorization: Bearer headers.
     *
     * Returning an empty string omits the header. The provider is asked for each request, the
     * event stream's GET and every POST.
     */
    void set_bearer_token_provider(std::function<std::string()> provider);

    /**
     * @brief Use these TLS settings for the connections opened from now on.
     *
     * @throws std::invalid_argument If the URL is not `https://`.
     * @throws TlsConfigError If the options cannot be used.
     */
    void set_tls(const TlsClientOptions& options);

    /** @brief Use a shared TLS context for the connections opened from now on. */
    void set_tls(std::shared_ptr<const TlsClientContext> context);

    /**
     * @brief Check the server's certificate against @p name instead of the URL's host, and send it
     * as the TLS server name.
     *
     * @throws std::invalid_argument If the URL is not `https://` or @p name is empty.
     */
    void set_server_name(std::string name);

    /**
     * @brief Send the connections opened from now on through an HTTP proxy.
     *
     * @throws std::invalid_argument If a proxy URL is malformed or not supported.
     */
    void set_proxy(ProxyOptions options);

    /**
     * @brief Dequeue the next message the server sent on the event stream.
     *
     * @details The first call, or the first write_message(), opens the stream.
     *
     * @throws TransportError If the transport is closed, or the stream ended, once the messages
     *         that arrived before are read. When the stream could not be opened, the error that
     *         prevented it.
     */
    Task<std::string> read_message() override;

    /**
     * @brief POST a message to the endpoint the server named, once the stream has delivered it.
     *
     * @throws TransportError When the transport is closed, the stream ended, the endpoint was
     *         refused or the POST cannot reach the server.
     * @throws HttpStatusError When the server answers the stream's GET or the POST with an error
     *         status.
     */
    Task<void> write_message(std::string_view message) override;

    /** @brief Close the event stream and the connection POSTs use. */
    void close() override;

    /**
     * @brief Whether the transport is over: closed, or its event stream failed or finished.
     *
     * A transport that is over fails every read and write, and nothing reopens the stream. That
     * includes a stream whose GET the server refused with 401.
     */
    [[nodiscard]] bool ended() const noexcept;

    /** @return DeprecatedFeature::eHttpSse. */
    [[nodiscard]] std::optional<DeprecatedFeature> deprecated_feature() const noexcept override {
        return DeprecatedFeature::eHttpSse;
    }

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp
