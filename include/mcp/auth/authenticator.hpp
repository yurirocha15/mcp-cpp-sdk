#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/export.hpp>

#include <string>

namespace mcp::auth {

/**
 * @brief Abstract interface for providing access tokens and handling refresh.
 *
 * @details Implementations must be safe to call from within an asio strand context.
 * `get_access_token()` must be lightweight, with no network I/O. Returns the current bearer token or
 * empty string. `try_refresh_token()` is a coroutine that performs network I/O to refresh the token
 * and persists the new token via the configured token store on success.
 */
class MCP_API Authenticator {
   public:
    virtual ~Authenticator() = default;

    /**
     * @brief Returns the current bearer token without network I/O.
     *
     * @return The stored access token, or an empty string when none is available.
     */
    [[nodiscard]] virtual std::string get_access_token() const = 0;

    /**
     * @brief Performs a token refresh via network I/O, persisting the new token on success.
     *
     * @return true if refresh succeeded and a new token was persisted; false otherwise.
     */
    virtual Task<bool> try_refresh_token() = 0;

    /**
     * @brief Performs challenge-driven authorization for a `WWW-Authenticate` response.
     *
     * @param www_authenticate Raw `WWW-Authenticate` header value from the challenge response.
     * @return true if authorization completed and a new token was persisted; false otherwise.
     *
     * @details OAuthClientTransport calls it for a challenge that a renewal cannot answer: on a 401,
     * only after try_refresh_token() was unavailable, failed, or produced a token that was refused
     * too, and at most once per request; on a 403 whose challenge carries
     * `error="insufficient_scope"`, directly. An implementation can discover metadata and run a
     * full authorization exchange rather than only renewing an existing grant. The default
     * implementation reports that it handled nothing.
     */
    virtual Task<bool> try_handle_challenge(const std::string& www_authenticate);

    /**
     * @brief Cancel the authorization work this authenticator has in flight and release parked
     *        callers with an error, leaving it usable for later requests.
     *
     * @details Called by OAuthClientTransport::close(). The authenticator may be shared and outlive
     * the transport, so later work runs as usual. The default is a no-op; an authenticator that
     * forwards to another should forward this call too. Safe to call more than once and from any
     * thread.
     */
    virtual void cancel_pending() {}

    /**
     * @brief Cancel any authorization work this authenticator has in flight, release parked
     *        callers with an error, and refuse further work.
     *
     * @details The owner's permanent shutdown; no transport calls it. The default is a no-op. Safe
     * to call more than once.
     */
    virtual void close() {}
};

}  // namespace mcp::auth
