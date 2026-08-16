#pragma once

#include <mcp/core/export.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::auth {

namespace constants {

constexpr std::size_t g_default_max_metadata_bytes = 256 * 1024;
constexpr std::size_t g_default_max_metadata_redirects = 3;

}  // namespace constants

/**
 * @brief Outcome of validating a metadata target before any network access occurs.
 *
 * @details Every value other than `allowed` means the SDK refused the target. Refusal happens
 * before host resolution, so a refused target is never contacted.
 */
enum class MetadataUrlDecision {
    allowed,             ///< Target passed every configured control.
    malformed_url,       ///< URL could not be decomposed into scheme, host and port.
    scheme_not_allowed,  ///< Scheme is not `https`, and the plain-HTTP loopback opt-out did not apply.
    origin_denied,       ///< Origin matched the application's deny list.
    origin_not_allowed,  ///< Origin is not present in the application's allow list.
    address_link_local,  ///< Address is IPv4 link-local (169.254.0.0/16) or IPv6 link-local.
    address_private,     ///< Address is in an RFC 1918 or IPv6 unique-local range.
    address_loopback,    ///< Address is loopback and the loopback opt-out is not enabled.
    address_multicast,   ///< Address is multicast or broadcast.
    address_reserved,    ///< Address is otherwise reserved, unspecified or non-routable.
    redirect_limit_exceeded,  ///< Redirect chain exceeded the configured bound.
    response_too_large,       ///< Response body exceeded the configured size cap.
};

/**
 * @brief Human-readable description of a metadata URL decision.
 *
 * @param decision Decision to describe.
 * @return A short, stable description suitable for diagnostics.
 */
[[nodiscard]] MCP_API std::string_view describe(MetadataUrlDecision decision);

/**
 * @brief Application-controlled policy governing outbound OAuth metadata requests.
 *
 * @details A challenge-supplied `resource_metadata` URL is attacker-influenced input, so every
 * OAuth metadata, protected-resource and token request derived from it is an outbound-request
 * primitive. This policy is consulted before any such request is issued.
 *
 * A default-constructed policy denies every origin: `allowed_origins` is empty, and an origin that
 * is not listed is refused. Applications must opt in to the origins they intend to talk to.
 */
struct MetadataFetchPolicy {
    /// Origins the application permits, each written as `scheme://host[:port]`. An empty list
    /// denies every origin. Comparison is exact; no normalization is applied.
    std::vector<std::string> allowed_origins;

    /// Origins the application refuses. Consulted before `allowed_origins`, so a denied origin is
    /// refused even when it also appears in the allow list.
    std::vector<std::string> denied_origins;

    /// Permit plain `http://` and loopback addresses. This is a narrow opt-out for loopback
    /// development and test fixtures only; it is never enabled implicitly and it does not relax
    /// any control other than the HTTPS requirement and the loopback address block.
    bool allow_plain_http_loopback{false};

    /// Maximum number of response body bytes accepted from a metadata or token endpoint. A
    /// response that exceeds the cap is rejected without being fully buffered.
    std::size_t max_response_bytes{constants::g_default_max_metadata_bytes};

    /// Maximum number of HTTP redirects followed. Each redirect target is validated afresh.
    std::size_t max_redirects{constants::g_default_max_metadata_redirects};
};

/**
 * @brief Extract the origin of a URL as `scheme://host[:port]`.
 *
 * @param url Absolute URL to decompose.
 * @return The origin exactly as written in the URL, or an empty string when the URL is malformed.
 *
 * @note No normalization is performed: case, an explicit default port and a trailing dot are all
 * preserved, so allow-list entries must be written the way the URL will appear.
 */
[[nodiscard]] MCP_API std::string metadata_url_origin(const std::string& url);

/**
 * @brief Validate a metadata target URL against the scheme and origin controls.
 *
 * @param policy Application policy to apply.
 * @param url Absolute URL that the SDK is about to fetch.
 * @return `MetadataUrlDecision::allowed` when the URL may be resolved, otherwise the refusal
 *         reason.
 *
 * @details This runs before host resolution. When it refuses, the host is never resolved and no
 * socket is opened. A host written as an IP literal is additionally classified here, so a URL
 * naming `169.254.169.254` or an RFC 1918 address is refused without any lookup at all.
 */
[[nodiscard]] MCP_API MetadataUrlDecision validate_metadata_url(const MetadataFetchPolicy& policy,
                                                                const std::string& url);

/**
 * @brief Classify a resolved address against the SSRF controls.
 *
 * @param policy Application policy to apply.
 * @param address_literal Resolved address in textual form.
 * @return `MetadataUrlDecision::allowed` when the address may be connected to, otherwise the
 *         refusal reason.
 *
 * @details Applied to every address produced by resolution. The SDK connects only to addresses
 * that pass this check, and it pins the addresses obtained from that single resolution rather than
 * resolving again, so a name that resolves differently on a later lookup cannot redirect an
 * established fetch.
 */
[[nodiscard]] MCP_API MetadataUrlDecision validate_metadata_address(const MetadataFetchPolicy& policy,
                                                                    const std::string& address_literal);

/**
 * @brief Error raised when a metadata target is refused by the fetch policy.
 *
 * @details Thrown instead of performing the request. When the refusal is a URL or origin decision
 * the target host is never resolved, so no socket is opened; when it is an address decision the
 * address is never connected to.
 */
class MCP_API MetadataPolicyError : public std::runtime_error {
   public:
    /**
     * @brief Construct a policy refusal.
     *
     * @param decision Reason the target was refused.
     * @param target URL or address that was refused.
     */
    MetadataPolicyError(MetadataUrlDecision decision, std::string target);

    /** @return Reason the target was refused. */
    [[nodiscard]] MetadataUrlDecision decision() const noexcept { return decision_; }

    /** @return URL or address that was refused. */
    [[nodiscard]] const std::string& target() const noexcept { return target_; }

   private:
    MetadataUrlDecision decision_;
    std::string target_;
};

}  // namespace mcp::auth
