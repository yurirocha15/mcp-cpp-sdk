#pragma once

#include <mcp/auth/authenticator.hpp>
#include <mcp/core/async_fwd.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/transport.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace mcp::auth {

/**
 * @brief Resource limits for legacy JSON-RPC authentication replay correlation.
 *
 * @details Pending request wires are retained only until a matching response arrives, the entry is
 * evicted to satisfy these limits, the TTL elapses, or the transport closes. A zero limit or a
 * non-positive TTL disables legacy response-driven replay correlation. HTTP status-driven refresh
 * and retry does not depend on this cache.
 */
struct OAuthClientTransportOptions {
    std::size_t max_pending_requests{
        mcp::constants::oauth::g_DEFAULT_MAX_PENDING_REQUESTS};  ///< Maximum retained request count.
    std::size_t max_pending_request_bytes{
        mcp::constants::oauth::g_DEFAULT_MAX_PENDING_REQUEST_BYTES};  ///< Approximate retained wire/key
                                                                      ///< bytes.
    std::chrono::milliseconds pending_request_ttl{
        mcp::constants::oauth::g_DEFAULT_PENDING_REQUEST_TTL};  ///< Replay eligibility.
    /// Step-up authorizations per request: each answers a 403 whose challenge says
    /// `insufficient_scope` by asking the authenticator, and so usually the user, to authorize the
    /// wider scope. Past it the request fails with that 403. Zero leaves every such 403 to the
    /// caller, as an HttpStatusError carrying the challenge.
    std::size_t max_step_ups_per_request{mcp::constants::oauth::g_DEFAULT_MAX_STEP_UPS_PER_REQUEST};
};

/**
 * @brief Transport wrapper that supplies and refreshes OAuth bearer tokens.
 *
 * @details For HttpClientTransport, tokens are sent in the HTTP Authorization header. The legacy
 * request-metadata mechanism is retained only for non-HTTP transports. The wrapper also handles token
 * refresh on legacy JSON-RPC authorization failures. When the server returns -32000, the
 * transport calls `Authenticator::try_refresh_token()` and, if successful, re-sends the request whose
 * ID matches the error response and reads the new response. Only one retry attempt is
 * made per `read_message()` call. Callers should ensure messages are idempotent since they may be
 * re-sent after a token refresh. Outstanding request wires are tracked by JSON-RPC request ID in a
 * bounded, expiring cache so an authentication error cannot replay a different concurrent request.
 */
class MCP_API OAuthClientTransport final : public ITransport {
   public:
    /**
     * @brief Construct an authenticated transport wrapper.
     *
     * @param inner Underlying transport used for MCP message exchange.
     * @param authenticator Authenticator used to retrieve and refresh tokens. Its tokens go to the
     *        server @p inner talks to, which is not compared with any URL the authenticator holds,
     *        so it must be the authenticator for that server alone.
     */
    OAuthClientTransport(std::shared_ptr<ITransport> inner,
                         std::shared_ptr<Authenticator> authenticator);

    /**
     * @brief Construct an authenticated transport wrapper with replay-correlation limits.
     *
     * @param inner Underlying transport used for MCP message exchange.
     * @param authenticator Authenticator used to retrieve and refresh tokens. Its tokens go to the
     *        server @p inner talks to, which is not compared with any URL the authenticator holds,
     *        so it must be the authenticator for that server alone.
     * @param options Bounds and TTL for retaining outstanding request wires.
     */
    OAuthClientTransport(std::shared_ptr<ITransport> inner,
                         std::shared_ptr<Authenticator> authenticator,
                         OAuthClientTransportOptions options);

    /**
     * @brief Read a message from the inner transport, retrying once on authentication errors.
     * @details If the received message contains the legacy JSON-RPC authentication error -32000,
     * attempts one token refresh via `Authenticator::try_refresh_token()`. On successful
     * refresh, replays the matching outstanding request while it remains eligible and returns the new
     * response. If refresh fails, correlation has expired or been evicted, or a second auth error is
     * received, returns the error response as-is. If the response cannot be parsed as JSON, the raw
     * string is returned unchanged.
     * @return The (possibly retried) raw message string.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Inject the current bearer token into an outgoing MCP message.
     *
     * @param message Serialized JSON-RPC request or notification.
     * @return A task that completes once the wrapped transport accepts the message.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Cancel the authenticator's in-flight work and close the wrapped transport.
     *
     * @details Calls Authenticator::cancel_pending(), not Authenticator::close(): the authenticator
     * stays usable, so another transport built on it can still refresh and authorize. A write on
     * this transport that is still retrying after a 401 or 403 fails. Once close() has run, the
     * write asks the authenticator for no new refresh or authorization; one it started while
     * close() was running is cancelled through Authenticator::cancel_pending() and waited for.
     */
    void close() override;

    /** @brief What the wrapped transport does. */
    [[nodiscard]] bool overlapping_writes() const noexcept override;

    /**
     * @brief Report to @p observer while a write waits for Authenticator::try_handle_challenge().
     *
     * @details The challenge can run an interactive authorization, whose time is the user's and not
     * the request's. The observer is told when a write begins and ends that wait, keyed by the
     * request's correlation key; Client uses it to stop the request's deadline while it lasts. A
     * refresh runs no such prompt and is not reported.
     */
    void set_interaction_observer(InteractionObserver observer) override;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp::auth
