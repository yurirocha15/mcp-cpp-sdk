#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

/**
 * @brief A generic list of string pairs, often used for query params or form fields.
 */
using KeyValuePairList = std::vector<std::pair<std::string, std::string>>;

/**
 * @brief A list of SSE events as (id, data) pairs.
 */
using SseEventList = KeyValuePairList;

/**
 * @brief What a bearer token validator decided about a token.
 *
 * A validator may return a plain `bool`, which converts to a TokenValidation without a principal.
 */
struct TokenValidation {
    /// Whether the token is accepted. A refused token is answered `401` with the bearer challenge.
    bool accepted = false;
    /// Who the token stands for, such as the subject or client the token was issued to. The
    /// requests the token carries are attributed to this principal (Context::principal()), so a
    /// client that refreshes its token keeps its identity. Without one the SDK derives a
    /// principal from the token itself, which changes when the token does.
    std::optional<std::string> principal_id;
    /// The scopes the token grants. A transport with a ScopeRequirementProvider compares them with
    /// the scopes a request needs and answers `403` with an `insufficient_scope` challenge when one
    /// is missing; a validator that leaves this empty grants none. Scope hierarchies are the
    /// validator's to expand: list every scope the token implies.
    std::optional<std::vector<std::string>> scopes;

    /// Accept or refuse without naming a principal.
    // NOLINTNEXTLINE(google-explicit-constructor)
    TokenValidation(bool is_accepted) : accepted(is_accepted) {}
    /// Accept or refuse, naming the principal the token stands for.
    TokenValidation(bool is_accepted, std::string principal)
        : accepted(is_accepted), principal_id(std::move(principal)) {}
};

/**
 * @brief The JSON-RPC message a ScopeRequirementProvider is asked about.
 */
struct ScopeRequestContext {
    std::string_view method;       ///< JSON-RPC method, such as `tools/call`.
    const nlohmann::json& params;  ///< The message's `params`, or null when it has none.
};

/**
 * @brief Names the scopes a request needs before it is served.
 *
 * Called on every POSTed JSON-RPC message that has a method, after the bearer token is accepted and
 * before anything is dispatched or any response stream is opened. Return every scope the operation
 * needs, together: the answer to a token that lacks one is a single `403` whose challenge names
 * all of them, so the client authorizes once. An empty list means the request needs no scope.
 * A provider that throws is answered `500`, as a validator that throws is.
 */
using ScopeRequirementProvider = std::function<std::vector<std::string>(const ScopeRequestContext&)>;

/// @brief Synchronous validator invoked at the HTTP boundary for a bearer token.
using BearerTokenValidator = std::function<TokenValidation(std::string_view)>;

/**
 * @brief Asynchronous validator invoked at the HTTP boundary for a bearer token.
 *
 * Use this when deciding on a token requires I/O (token introspection, a JWKS fetch), so the
 * decision suspends instead of blocking the executor that is concurrently serving MCP traffic.
 *
 * The token is passed by value: a view would point into the Beast request buffer, which is not
 * guaranteed to outlive a suspension.
 */
using AsyncBearerTokenValidator = std::function<Task<TokenValidation>(std::string)>;

/**
 * @brief Extract a bearer token from an HTTP Authorization header.
 * @param authorization_header Complete Authorization header value.
 * @return The characters after the Bearer scheme and the spaces that follow it,
 *         or an empty view when the scheme is absent or nothing follows it.
 */
MCP_API std::string_view http_bearer_token(std::string_view authorization_header);

/**
 * @brief Extract the path component of an HTTP request target.
 * @param target Request target, such as `/mcp?stream=1`.
 * @return The characters before the first `?` or `#`, or the whole target when neither appears.
 */
MCP_API std::string_view http_request_path(std::string_view target);

/**
 * @brief Parameters advertised in the `WWW-Authenticate` challenge on a 401.
 *
 * Every field is optional. A default-constructed config renders the bare `Bearer` challenge,
 * which is what the server transports send when no challenge is configured.
 */
struct BearerChallengeConfig {
    std::string resource_metadata;  ///< RFC 9728 metadata URL, sent as `resource_metadata="..."`.
    std::string scope;              ///< Space-delimited scopes, sent as `scope="..."`.
    std::string realm;              ///< RFC 7235 realm, sent as `realm="..."`.
    std::string error;              ///< RFC 6750 section 3.1 code such as `invalid_token`.
};

/**
 * @brief Render a challenge config as a `WWW-Authenticate` header value.
 *
 * Set parameters are emitted in the order realm, error, scope, resource_metadata, each as an
 * RFC 7235 quoted-string with `\` and `"` backslash-escaped.
 *
 * @param config Challenge parameters to render.
 * @return `Bearer` when no field is set, otherwise `Bearer key="value", ...`.
 * @throws std::invalid_argument If a value holds a byte that cannot appear in a quoted-string,
 *         that is, anything outside horizontal tab and printable US-ASCII. The transports render
 *         the challenge once when it is configured, so an invalid value is reported to the caller
 *         that supplied it rather than while a request is being served.
 */
MCP_API std::string format_www_authenticate(const BearerChallengeConfig& config);

/**
 * @brief RFC 9728 protected-resource metadata served by a server transport.
 */
struct ProtectedResourceMetadataConfig {
    std::string resource;                            ///< Required. Canonical resource URL.
    std::vector<std::string> authorization_servers;  ///< Issuer URLs able to mint tokens.
    std::vector<std::string> scopes_supported;       ///< Scopes the resource recognizes.
    /// Path the document is served at. Empty means derive it from `resource` per RFC 9728 3.1;
    /// set it only to override that derivation.
    std::string path;
};

/**
 * @brief Path the metadata document is served at.
 *
 * Returns `path` when it is set. Otherwise derives it the way RFC 9728 3.1 requires, by inserting the
 * well-known segment between the authority and the resource's own path: a resource at
 * `https://host/mcp` is described at `/.well-known/oauth-protected-resource/mcp`, and one at
 * `https://host` at `/.well-known/oauth-protected-resource`.
 *
 * @param metadata Metadata whose `resource` supplies the path component.
 * @return The path, always beginning with `/`.
 * @throws std::invalid_argument If `resource` is not an absolute URL with an authority.
 */
MCP_API std::string protected_resource_metadata_path(const ProtectedResourceMetadataConfig& metadata);

/**
 * @brief Render protected-resource metadata as the JSON document clients fetch.
 *
 * Empty list members are omitted rather than written as empty arrays.
 *
 * @param metadata Metadata to render.
 * @return A JSON object with `resource` and any populated list members.
 */
MCP_API std::string format_protected_resource_metadata(const ProtectedResourceMetadataConfig& metadata);

/**
 * @brief Build the absolute URL the metadata document is reachable at.
 *
 * The URL is the origin of `resource` (its scheme and authority) followed by
 * protected_resource_metadata_path(). It is built entirely from `resource`, never from the
 * address a transport happens to be bound to: behind a TLS terminator, a reverse proxy or a
 * container port mapping the listener's own origin is not the one clients can reach, so inferring
 * it would publish a URL that cannot be fetched.
 *
 * @param metadata Metadata whose `resource` supplies the origin and path.
 * @return The absolute metadata URL.
 * @throws std::invalid_argument If `resource` is not an absolute URL with an authority.
 */
MCP_API std::string protected_resource_metadata_url(const ProtectedResourceMetadataConfig& metadata);

/**
 * @brief What an HTTP server transport lets its connections and requests hold at once.
 *
 * Every limit covers the whole listener, whatever session a request belongs to.
 */
struct HttpConnectionLimits {
    /// Connections at once; one past it is closed as soon as it is accepted, without an answer.
    /// Zero takes three quarters of the process's soft descriptor limit (RLIMIT_NOFILE) where
    /// there is one, else 4096. systemd gives a service a soft limit of 1024 unless its unit sets
    /// LimitNOFILE=, which makes 768; each SSE stream on GET holds a connection too.
    std::size_t max_connections{0};
    /// How long reading one request may take, from waiting for its first byte to the end of its
    /// body, and how long the peer may take to read the answer; a connection that takes longer is
    /// closed. Zero means no limit.
    std::chrono::milliseconds request_read_timeout{
        mcp::constants::http_server::g_DEFAULT_REQUEST_READ_TIMEOUT};
    /// Requests being handled at once. One past it is answered 503.
    std::size_t max_requests{mcp::constants::http_server::g_DEFAULT_MAX_REQUESTS};
    /// Memory the requests being read or handled may hold together: a body by what has arrived
    /// while it is read, a JSON-RPC message by what it takes parsed. A request past it is answered
    /// 503.
    std::size_t max_request_bytes{mcp::constants::http_server::g_DEFAULT_MAX_REQUEST_BYTES};
};

/**
 * @brief How an HTTP server transport streams the answer to a 2026-07-28 request.
 */
struct StreamOptions {
    /// How often an open stream carries an SSE comment, so idle intermediaries keep it open.
    std::chrono::milliseconds keepalive_interval{
        mcp::constants::http_server::g_DEFAULT_KEEPALIVE_INTERVAL};
    /// After a client half-closes its connection, how often the server checks that the client is
    /// still there: by writing an SSE comment to a stream, where a write that fails cancels the
    /// request, or, for a client that does not accept a stream, by looking for a reset on the
    /// socket. A client that accepts a stream gets one when it half-closes, since a clean close
    /// shows no reset and is found only by a failed write; its answer then has status 200.
    std::chrono::milliseconds disconnect_probe_interval{
        mcp::constants::http_server::g_DEFAULT_DISCONNECT_PROBE_INTERVAL};
    /// Notifications a request may have waiting to be written. A notification that would take the
    /// request past this limit, or past max_queued_bytes, is dropped and counted in
    /// StreamStats::dropped_notifications; the request carries on and its answer is unchanged.
    /// A subscriptions/listen stream instead ends when a notification is dropped, so that the
    /// client reconnects and lists again.
    std::size_t max_queued_notifications =
        mcp::constants::http_server::g_DEFAULT_MAX_QUEUED_NOTIFICATIONS;
    /// Bytes of notifications a request may have waiting to be written or being written, counted
    /// as the size of each serialized message. Notifications are dropped over this limit as over
    /// max_queued_notifications. The bytes are also charged to the connection memory budget
    /// (HttpConnectionLimits::max_request_bytes), so a notification the budget cannot hold is
    /// dropped too. A message larger than the limit is always dropped.
    std::size_t max_queued_bytes{mcp::constants::http_server::g_DEFAULT_MAX_QUEUED_BYTES};
};

/**
 * @brief Counts of what the 2026-07-28 streams of an HTTP server transport have done.
 */
struct StreamStats {
    std::uint64_t streams_opened{0};         ///< Answers sent as an SSE stream.
    std::uint64_t disconnect_cancels{0};     ///< Requests cancelled because the client went away.
    std::uint64_t half_close_probes{0};      ///< Probe comments written after a half-close.
    std::uint64_t dropped_notifications{0};  ///< Notifications over the queue limits.
};

}  // namespace mcp
