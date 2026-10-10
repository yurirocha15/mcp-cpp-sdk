#pragma once

#include <mcp/core/export.hpp>

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::auth {

namespace constants {

constexpr std::size_t g_default_max_metadata_bytes = 256 * 1024;
constexpr std::size_t g_default_max_metadata_redirects = 3;

}  // namespace constants

namespace detail {

/**
 * @brief Bound and clean peer-controlled text before embedding it in a diagnostic message.
 *
 * @param value Text that came from a peer: a URL, a host, an issuer, a metadata field, a response
 *        body. Anything the SDK did not author itself.
 * @return Well-formed UTF-8, bounded by a fixed budget and ending in an ellipsis when it was cut,
 *         with every character that could end a line replaced by a space and every ill-formed byte
 *         replaced by `?`.
 *
 * @details Keeps a peer-chosen value from forging a log line with embedded newlines or flooding the
 * message. Truncation stops on a codepoint boundary, so a multi-byte character straddling the budget
 * is dropped whole.
 */
[[nodiscard]] MCP_API std::string sanitize_for_diagnostics(std::string_view value);

}  // namespace detail

/**
 * @brief Outcome of validating a metadata target before any network access occurs.
 *
 * @details Every value other than `allowed` means the SDK refused the target. Refusal happens
 * before host resolution, so a refused target is never contacted.
 */
enum class MetadataUrlDecision {
    allowed,        ///< Target passed every configured control.
    malformed_url,  ///< URL could not be decomposed into scheme, host and port, including a
                    ///< port that is not a plain decimal number or a host that is nothing but dots.
    scheme_not_allowed,  ///< Scheme is not `https`, and the plain-HTTP loopback opt-out did not apply.
    origin_denied,       ///< Origin matched the application's deny list.
    origin_not_allowed,  ///< Origin is not present in the application's allow list.
    address_link_local,  ///< Address is IPv4 link-local (169.254.0.0/16) or IPv6 link-local.
    address_private,     ///< Address is in an RFC 1918 or IPv6 unique-local range.
    address_loopback,    ///< Address is loopback and the loopback opt-out is not enabled.
    address_multicast,   ///< Address is multicast or broadcast.
    address_reserved,    ///< Address is otherwise reserved, unspecified or non-routable.
    redirect_limit_exceeded,        ///< Redirect chain exceeded the configured bound.
    response_too_large,             ///< Response body exceeded the configured size cap.
    denied_origin_entry_malformed,  ///< A `denied_origins` entry is not a bare origin, so the rule
                                    ///< it states cannot be applied. Reported against the offending
                                    ///< entry, not against the target.
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
 * @details Consulted before any OAuth metadata, protected-resource or token request is issued; a
 * challenge-supplied `resource_metadata` URL is attacker-influenced input.
 *
 * A default-constructed policy denies every origin: `allowed_origins` is empty, and an origin that is
 * not listed is refused.
 */
struct MetadataFetchPolicy {
    /// Origins the application permits, each written as `scheme://host[:port]` with nothing after the
    /// authority. An empty list denies every origin. Compared after canonicalizing scheme and host
    /// case, trailing host dots, an explicit port equal to the scheme's default (`:443` for `https`,
    /// `:80` for `http`, also when written as `:0443`), and an IP literal's textual form (expanded
    /// and compressed IPv6 spellings compare equal). An entry that carries a path, query or fragment,
    /// or whose port is not a plain in-range decimal number, matches nothing.
    std::vector<std::string> allowed_origins;

    /// Origins the application refuses. Consulted before `allowed_origins`, so a denied origin is
    /// refused even when it also appears in the allow list. Compared with the same canonicalization
    /// as `allowed_origins`.
    ///
    /// Every entry must be a bare origin. Unlike `allowed_origins`, an entry that is not one -- it
    /// carries a path (a lone trailing `/` included), a query or a fragment, or its port is not a
    /// plain in-range decimal number -- is rejected rather than ignored: `validate_metadata_url`
    /// throws `MetadataPolicyError` with `MetadataUrlDecision::denied_origin_entry_malformed`, naming
    /// the offending entry, and refuses every target until the entry is corrected.
    std::vector<std::string> denied_origins;

    /// Consulted only for an origin `allowed_origins` does not list; returning true admits it. The
    /// origin passed to the callback is the canonicalized form (see `allowed_origins`), not the raw
    /// text of the URL.
    ///
    /// The hook can only widen: `denied_origins` is consulted first and still refuses, and the
    /// decision is made before host resolution, so an origin this hook rejects is never contacted.
    std::function<bool(const std::string& origin)> origin_allowance;

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
 * @throws MetadataPolicyError With `MetadataUrlDecision::denied_origin_entry_malformed` when any
 *         `MetadataFetchPolicy::denied_origins` entry is not a bare origin. The deny list is
 *         examined first, so such a policy refuses every target. The error names the offending
 *         entry rather than the URL.
 *
 * @details Runs before host resolution: when it refuses, the host is never resolved and no socket is
 * opened. A host written as an IP literal is classified here, so a URL naming `169.254.169.254` or an
 * RFC 1918 address is refused without any lookup. The origin derived from `url` is canonicalized (see
 * `MetadataFetchPolicy::allowed_origins`) before the deny list, allow list or `origin_allowance` sees
 * it, and the https-required check and loopback opt-out run on the same canonical scheme and host; a
 * URL whose port or host does not canonicalize is refused as `malformed_url`. `metadata_url_origin`
 * is unaffected and never normalizes its result.
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
 * @details Applied to every address produced by resolution. The SDK connects only to addresses that
 * pass, and pins the addresses from that single resolution rather than resolving again.
 */
[[nodiscard]] MCP_API MetadataUrlDecision validate_metadata_address(const MetadataFetchPolicy& policy,
                                                                    const std::string& address_literal);

/**
 * @brief Error raised when a metadata target is refused by the fetch policy.
 *
 * @details Thrown instead of performing the request: for a URL or origin decision the host is never
 * resolved, and for an address decision the address is never connected to. For
 * `MetadataUrlDecision::denied_origin_entry_malformed`, `target()` is the offending `denied_origins`
 * entry rather than the request target.
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
