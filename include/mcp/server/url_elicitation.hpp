#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/elicitation.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace mcp {

/**
 * @brief Thrown by a request handler that cannot go on until the user completes URL-mode
 *        elicitations, such as a sign-in on a third-party site (protocol revision 2025-11-25).
 *
 * @details In a session that negotiated 2025-11-25, the request is answered with the JSON-RPC
 * error mcp::constants::jsonrpc::g_URL_ELICITATION_REQUIRED, whose `data` lists the elicitations,
 * instead of a result; for a tools/call that also takes the place of an `isError` result. The
 * server records each elicitation for the session, as Context::elicit() does, so that
 * Server::notify_elicitation_complete() tells the client when one has completed. A session of an
 * earlier revision and a 2026-07-28 request get what any other exception gives: an `isError`
 * result for a tools/call and an internal error for other requests, with what() as the text. An
 * `elicitationId` that another session has in progress gets an internal error as well.
 */
class MCP_API UrlElicitationRequired : public std::runtime_error {
   public:
    /**
     * @brief Lists the elicitations the user must complete.
     *
     * @details An elicitation without an `elicitationId` gets a generated one, which
     * elicitations() returns, so that the handler can keep it to complete the elicitation later.
     *
     * @param elicitations The URL-mode elicitations.
     * @param message The error message sent with them.
     */
    explicit UrlElicitationRequired(
        std::vector<ElicitRequestURLParams> elicitations,
        const std::string& message = "This request requires more information.");

    /// The elicitations, each with its `elicitationId`.
    [[nodiscard]] const std::vector<ElicitRequestURLParams>& elicitations() const noexcept {
        return elicitations_;
    }

   private:
    std::vector<ElicitRequestURLParams> elicitations_;
};

}  // namespace mcp
