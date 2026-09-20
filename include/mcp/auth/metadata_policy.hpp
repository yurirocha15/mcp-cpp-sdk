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
 * @details Diagnostics built from peer-controlled text are a log-forging vector: a value carrying
 * newlines can close the SDK's message and open a line of the attacker's own, and an unbounded one
 * floods whatever the message lands in. Parsed JSON is the sharp edge rather than raw bytes on the
 * wire, because a metadata document is decoded before its fields are interpolated, so `\n` and
 * `\r` written as escape sequences arrive as real control bytes. No secret is involved, so this
 * addresses forgery and flooding rather than disclosure.
 *
 * The output is always well-formed UTF-8, which is a second requirement rather than a detail of the
 * first. Truncation stops on a codepoint boundary, so a multi-byte character straddling the budget
 * is dropped whole rather than cut in half, and bytes that were already ill-formed on the way in
 * are replaced. Emitting invalid UTF-8 would hand a peer the same flooding it is denied here by
 * another route, since a JSON log encoder given invalid UTF-8 throws or drops the record. Text
 * arriving through a header has had nothing validate it, unlike text from a parsed document.
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
 * @details A challenge-supplied `resource_metadata` URL is attacker-influenced input, so every
 * OAuth metadata, protected-resource and token request derived from it is an outbound-request
 * primitive. This policy is consulted before any such request is issued.
 *
 * A default-constructed policy denies every origin: `allowed_origins` is empty, and an origin that
 * is not listed is refused. Applications must opt in to the origins they intend to talk to.
 */
struct MetadataFetchPolicy {
    /// Origins the application permits, each written as `scheme://host[:port]` with nothing else
    /// after the authority. An empty list denies every origin. Compared after canonicalizing scheme
    /// and host case, a trailing run of host dots, a strictly-numeric explicit port (so `:00443` and
    /// `:0443` both canonicalize the same as `:443`) that equals the scheme's default (`443` for
    /// `https`, `80` for `http`), and an IP literal's textual form (so an expanded and a compressed
    /// IPv6 spelling of the same address compare equal). A non-default port and a genuinely different
    /// host otherwise still distinguish origins exactly. An entry that carries a path, query or
    /// fragment after the authority, or whose port is not a plain in-range decimal number, is not a
    /// bare origin and matches nothing rather than being widened or truncated into one.
    std::vector<std::string> allowed_origins;

    /// Origins the application refuses. Consulted before `allowed_origins`, so a denied origin is
    /// refused even when it also appears in the allow list. Compared with the same canonicalization
    /// as `allowed_origins`.
    ///
    /// Every entry must be a bare origin. Unlike `allowed_origins`, an entry that is not one — it
    /// carries a path (a lone trailing `/` included), a query or a fragment, or its port is not a
    /// plain in-range decimal number — is rejected rather than ignored: `validate_metadata_url`
    /// throws `MetadataPolicyError` with `MetadataUrlDecision::denied_origin_entry_malformed`,
    /// naming the offending entry, and refuses every target until the entry is corrected.
    ///
    /// The two lists differ here because the consequence of dropping an entry differs. An allow
    /// entry that cannot be interpreted grants nothing, so ignoring it fails closed. A deny entry
    /// that cannot be interpreted blocks nothing, so ignoring it would admit the very origin the
    /// entry was written to refuse. A deny rule the SDK cannot apply is therefore a configuration
    /// error, not a rule that quietly matches nothing, and it is never reinterpreted into some
    /// nearby rule the author did not write.
    std::vector<std::string> denied_origins;

    /// Consulted only for an origin `allowed_origins` does not list; returning true admits it. The
    /// origin passed to the callback is the canonicalized form (see `allowed_origins`), not the raw
    /// text of the URL.
    ///
    /// An application generally cannot enumerate its authorization servers in advance, because a
    /// protected resource names them in metadata at run time. Rather than force such an application
    /// to abandon the allow list altogether, it may state the rule it would have written. The hook
    /// can only widen: `denied_origins` is consulted first and still refuses, and the decision is
    /// still made before host resolution, so an origin this hook rejects is never contacted. An
    /// unset hook leaves the allow list as the only way in.
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
 *         examined before anything else, so such a policy refuses every target, whether or not the
 *         offending entry describes the one at hand, and the target is never resolved. The thrown
 *         error names the offending entry rather than the URL.
 *
 * @details This runs before host resolution. When it refuses, the host is never resolved and no
 * socket is opened. A host written as an IP literal is additionally classified here, so a URL
 * naming `169.254.169.254` or an RFC 1918 address is refused without any lookup at all. The origin
 * derived from `url` is canonicalized (see `MetadataFetchPolicy::allowed_origins`) once, before the
 * deny list, allow list or `origin_allowance` sees it; a URL whose port or host does not canonicalize
 * at all (a malformed port, or a host that is nothing but dots) is refused as `malformed_url`. The
 * https-required check and the loopback opt-out that follow the origin decision also run on that same
 * canonical scheme and host, not on the raw URL text, so they see exactly what the origin decision and
 * `origin_allowance` saw. This canonicalization is purely internal to `validate_metadata_url` and does
 * not affect `metadata_url_origin`, which never normalizes its result.
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
 *
 * The decision may also report the policy itself rather than the target: for
 * `MetadataUrlDecision::denied_origin_entry_malformed`, `target()` is the offending `denied_origins`
 * entry, and the request that triggered the check was refused without being issued.
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
