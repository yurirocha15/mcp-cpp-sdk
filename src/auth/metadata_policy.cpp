#include <mcp/auth/metadata_policy.hpp>
#include <mcp/core/async.hpp>

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "mcp/detail/constants.hpp"
#include "mcp/detail/text/unicode.hpp"

namespace ipv4 = mcp::detail::constants::ipv4;
namespace ipv6 = mcp::detail::constants::ipv6;

namespace mcp::auth {

namespace net = boost::asio;

namespace {

/// Decompose an absolute URL into its scheme and authority without normalizing either.
bool split_url(const std::string& url, std::string& scheme, std::string& authority) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return false;
    }
    scheme = url.substr(0, scheme_end);

    const auto authority_start = scheme_end + 3;
    auto authority_end = url.size();
    for (auto index = authority_start; index < url.size(); ++index) {
        const char character = url[index];
        if (character == '/' || character == '?' || character == '#') {
            authority_end = index;
            break;
        }
    }
    authority = url.substr(authority_start, authority_end - authority_start);
    return !authority.empty();
}

/// Extract the host from an authority. Userinfo is rejected by the caller, not stripped here.
bool authority_host(const std::string& authority, std::string& host) {
    if (authority.front() == '[') {
        // Only a port may follow the closing bracket.
        const auto closing = authority.find(']');
        if (closing == std::string::npos || closing == 1 ||
            (closing + 1 < authority.size() && authority[closing + 1] != ':')) {
            return false;
        }
        host = authority.substr(1, closing - 1);
        return true;
    }
    const auto colon = authority.find(':');
    if (colon == std::string::npos) {
        host = authority;
        return true;
    }
    if (authority.find(':', colon + 1) != std::string::npos) {
        return false;
    }
    host = authority.substr(0, colon);
    return !host.empty();
}

/// Lowercase only ASCII letters; every other byte (digits, punctuation, non-ASCII) is left as-is.
std::string lowercase_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
    return value;
}

/// Parse a URL port substring strictly: ASCII digits only, no sign, no whitespace, and in range
/// [0, 65535]. Leading zeros are tolerated (`"00443"` and `"0443"` both parse to 443). Anything
/// else (an empty string, a sign, embedded whitespace, a value that overflows 16 bits) returns
/// `nullopt` rather than truncating or wrapping.
std::optional<std::uint32_t> parse_strict_port(const std::string& port_text) {
    std::uint16_t value = 0;
    const char* const end = port_text.data() + port_text.size();
    const auto [stop, error] = std::from_chars(port_text.data(), end, value);
    if (port_text.empty() || error != std::errc{} || stop != end) {
        return std::nullopt;
    }
    return value;
}

/// Canonical form of an origin's components. Kept split (rather than only the formatted string) so
/// `validate_metadata_url` can feed the same canonical scheme and host into the checks that run after
/// the origin decision, instead of re-deriving them from raw, non-canonical text.
struct CanonicalOrigin {
    std::string scheme;
    std::string host;  ///< Never bracketed, even when the origin names an IPv6 literal.
    std::optional<std::uint32_t>
        port;        ///< Absent when no port was written, or it was the scheme's default.
    bool bracketed;  ///< Whether the original authority wrote the host as `[...]`.
};

/// Decompose and canonicalize an origin (`scheme://authority`) for comparison.
///
/// Lowercases the scheme and a non-IP-literal host, and strips every trailing dot from such a host. A
/// host that parses as an IP literal (IPv4, or IPv6 with or without brackets) is replaced by its
/// normalized textual form, so expanded and compressed IPv6 spellings compare equal; a literal is
/// never dot-stripped. An explicit port is parsed strictly (see `parse_strict_port`) and dropped when
/// it equals the scheme's default (`443` for `https`, `80` for `http`).
///
/// Returns `nullopt` when `origin` is not `scheme://authority` with nothing following (a path, query
/// or fragment), when the host is malformed or empty after dot-stripping, or when a port is not a
/// plain in-range decimal number.
std::optional<CanonicalOrigin> canonicalize_origin_parts(const std::string& origin) {
    std::string scheme;
    std::string authority;
    if (!split_url(origin, scheme, authority)) {
        return std::nullopt;
    }
    // A bare origin has nothing past the authority. An entry carrying a path, query or fragment
    // (e.g. an allow-list entry mistakenly written as "https://as.test/realms/foo") is not an origin
    // and must never be silently truncated into a match for the origin it happens to prefix.
    if (scheme.size() + 3 + authority.size() != origin.size()) {
        return std::nullopt;
    }
    std::string host;
    if (!authority_host(authority, host)) {
        return std::nullopt;
    }

    const bool bracketed = authority.front() == '[';
    std::string port_text;
    bool has_port = false;
    if (bracketed) {
        const auto closing = authority.find(']');
        if (closing != std::string::npos && closing + 1 < authority.size() &&
            authority[closing + 1] == ':') {
            port_text = authority.substr(closing + 2);
            has_port = true;
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string::npos) {
            port_text = authority.substr(colon + 1);
            has_port = true;
        }
    }

    std::optional<std::uint32_t> port;
    if (has_port) {
        port = parse_strict_port(port_text);
        if (!port) {
            return std::nullopt;
        }
    }

    auto canonical_scheme = lowercase_ascii(scheme);

    boost::system::error_code error;
    const auto address = net::ip::make_address(host, error);
    const bool is_ip_literal = !error;

    std::string canonical_host;
    if (is_ip_literal) {
        canonical_host = address.to_string();
    } else {
        canonical_host = lowercase_ascii(host);
        while (!canonical_host.empty() && canonical_host.back() == '.') {
            canonical_host.pop_back();
        }
        if (canonical_host.empty()) {
            return std::nullopt;
        }
    }

    namespace url = mcp::detail::constants::url;
    if (port && ((*port == url::g_HTTPS_DEFAULT_PORT && canonical_scheme == "https") ||
                 (*port == url::g_HTTP_DEFAULT_PORT && canonical_scheme == "http"))) {
        port.reset();
    }

    return CanonicalOrigin{
        .scheme = std::move(canonical_scheme),
        .host = std::move(canonical_host),
        .port = port,
        .bracketed = bracketed,
    };
}

/// Format a `CanonicalOrigin` back into `scheme://authority` text, for list comparison and for the
/// value handed to `origin_allowance`.
std::string format_canonical_origin(const CanonicalOrigin& parts) {
    std::string authority = parts.bracketed ? "[" + parts.host + "]" : parts.host;
    if (parts.port) {
        authority += ":" + std::to_string(*parts.port);
    }
    return parts.scheme + "://" + authority;
}

/// Canonicalize an origin purely for list-membership comparison; see `canonicalize_origin_parts`.
std::optional<std::string> canonicalize_origin(const std::string& origin) {
    const auto parts = canonicalize_origin_parts(origin);
    if (!parts) {
        return std::nullopt;
    }
    return format_canonical_origin(*parts);
}

/// Compare a canonicalized origin against the allow list, canonicalizing each entry at comparison
/// time so the caller never has to keep a normalized copy of the policy around. An entry that does
/// not canonicalize to a bare origin (a malformed port, a path/query/fragment, or any other
/// malformed form) matches nothing, rather than being widened or truncated into a match for the
/// origin it happens to prefix. Dropping an allow entry grants nothing, so this direction is
/// fail-closed; the deny list, where the same silence would be fail-open, is handled by
/// `denies_origin` instead.
bool contains_origin(const std::vector<std::string>& origins, const std::string& canonical_origin) {
    return std::ranges::any_of(origins, [&](const std::string& candidate) {
        const auto canonical_candidate = canonicalize_origin(candidate);
        return canonical_candidate && *canonical_candidate == canonical_origin;
    });
}

/// Compare a canonicalized origin against the deny list, requiring every entry to be a bare origin.
///
/// An entry that does not canonicalize is a configuration error and is reported, not discarded (which
/// would be fail-open) or reinterpreted as the origin it resembles. The whole list is examined before
/// a match is returned, so a malformed entry is reported even when an earlier entry matched; a policy
/// carrying one refuses every target until it is corrected.
///
/// @throws MetadataPolicyError With `denied_origin_entry_malformed`, naming the offending entry.
bool denies_origin(const std::vector<std::string>& origins, const std::string& canonical_origin) {
    bool denied = false;
    for (const auto& candidate : origins) {
        const auto canonical_candidate = canonicalize_origin(candidate);
        if (!canonical_candidate) {
            throw MetadataPolicyError(MetadataUrlDecision::eDeniedOriginEntryMalformed, candidate);
        }
        if (*canonical_candidate == canonical_origin) {
            denied = true;
        }
    }
    return denied;
}

/// Classify an IPv4 address against the ranges that must never be reached by a metadata fetch.
MetadataUrlDecision classify_v4(const MetadataFetchPolicy& policy, const net::ip::address_v4& address) {
    const auto value = address.to_uint();
    const auto in = [value](const ipv4::Block& block) { return block.contains(value); };
    const auto in_any = [&in](const auto& blocks) { return std::ranges::any_of(blocks, in); };

    if (in(ipv4::g_LOOPBACK)) {
        return policy.allow_plain_http_loopback ? MetadataUrlDecision::eAllowed
                                                : MetadataUrlDecision::eAddressLoopback;
    }
    if (in(ipv4::g_LINK_LOCAL)) {
        return MetadataUrlDecision::eAddressLinkLocal;
    }
    if (in_any(ipv4::g_PRIVATE_USE)) {
        return MetadataUrlDecision::eAddressPrivate;
    }
    if (value == ipv4::g_LIMITED_BROADCAST || in(ipv4::g_MULTICAST)) {
        return MetadataUrlDecision::eAddressMulticast;
    }
    if (in_any(ipv4::g_RESERVED)) {
        return MetadataUrlDecision::eAddressReserved;
    }
    return MetadataUrlDecision::eAllowed;
}

/// Recognize the ::ffff:a.b.c.d form so an IPv4 target cannot be smuggled through an IPv6 literal.
bool mapped_v4(const net::ip::address_v6& address, net::ip::address_v4& mapped) {
    if (!address.is_v4_mapped()) {
        return false;
    }
    mapped = net::ip::make_address_v4(net::ip::v4_mapped, address);
    return true;
}

/// Recognize the NAT64 well-known prefix (64:ff9b::/96) and 6to4 (2002::/16), which embed an IPv4
/// address that a translator or relay delivers to, so a private IPv4 target cannot hide in either.
bool embedded_v4(const net::ip::address_v6& address, net::ip::address_v4& embedded) {
    const auto bytes = address.to_bytes();
    const auto v4_at = [&bytes](std::size_t offset) {
        net::ip::address_v4::bytes_type v4_bytes{};
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), v4_bytes.size(),
                    v4_bytes.begin());
        return net::ip::address_v4(v4_bytes);
    };
    if (std::equal(ipv6::g_NAT64_PREFIX.begin(), ipv6::g_NAT64_PREFIX.end(), bytes.begin())) {
        embedded = v4_at(ipv6::g_NAT64_PREFIX.size());
        return true;
    }
    if (std::equal(ipv6::g_SIX_TO_FOUR_PREFIX.begin(), ipv6::g_SIX_TO_FOUR_PREFIX.end(),
                   bytes.begin())) {
        embedded = v4_at(ipv6::g_SIX_TO_FOUR_PREFIX.size());
        return true;
    }
    return false;
}

MetadataUrlDecision classify_v6(const MetadataFetchPolicy& policy, const net::ip::address_v6& address) {
    net::ip::address_v4 mapped;
    if (mapped_v4(address, mapped)) {
        return classify_v4(policy, mapped);
    }
    if (embedded_v4(address, mapped)) {
        // Only a real loopback address may use the loopback opt-out; an embedded one is refused.
        auto strict = policy;
        strict.allow_plain_http_loopback = false;
        return classify_v4(strict, mapped);
    }
    if (address.is_unspecified()) {
        return MetadataUrlDecision::eAddressReserved;
    }
    if (address.is_loopback()) {
        return policy.allow_plain_http_loopback ? MetadataUrlDecision::eAllowed
                                                : MetadataUrlDecision::eAddressLoopback;
    }
    if (address.is_link_local()) {
        return MetadataUrlDecision::eAddressLinkLocal;
    }
    if (address.is_multicast()) {
        return MetadataUrlDecision::eAddressMulticast;
    }
    const auto bytes = address.to_bytes();
    if ((bytes[0] & ipv6::g_UNIQUE_LOCAL_MASK) == ipv6::g_UNIQUE_LOCAL_PREFIX) {
        return MetadataUrlDecision::eAddressPrivate;
    }
    if (address.is_site_local()) {
        return MetadataUrlDecision::eAddressPrivate;
    }
    return MetadataUrlDecision::eAllowed;
}

bool is_loopback_host(const std::string& host) {
    if (host == "localhost") {
        return true;
    }
    boost::system::error_code error;
    const auto address = net::ip::make_address(host, error);
    return !error && address.is_loopback();
}

}  // namespace

std::string_view describe(MetadataUrlDecision decision) {
    switch (decision) {
        case MetadataUrlDecision::eAllowed:
            return "allowed";
        case MetadataUrlDecision::eMalformedUrl:
            return "URL is malformed or carries userinfo";
        case MetadataUrlDecision::eSchemeNotAllowed:
            return "scheme is not https and the loopback opt-out does not apply";
        case MetadataUrlDecision::eOriginDenied:
            return "origin is on the application deny list";
        case MetadataUrlDecision::eOriginNotAllowed:
            return "origin is not on the application allow list";
        case MetadataUrlDecision::eAddressLinkLocal:
            return "address is link-local";
        case MetadataUrlDecision::eAddressPrivate:
            return "address is in a private range";
        case MetadataUrlDecision::eAddressLoopback:
            return "address is loopback and the loopback opt-out is not enabled";
        case MetadataUrlDecision::eAddressMulticast:
            return "address is multicast or broadcast";
        case MetadataUrlDecision::eAddressReserved:
            return "address is reserved or non-routable";
        case MetadataUrlDecision::eRedirectLimitExceeded:
            return "redirect chain exceeded the configured bound";
        case MetadataUrlDecision::eResponseTooLarge:
            return "response exceeded the configured size cap";
        case MetadataUrlDecision::eDeniedOriginEntryMalformed:
            return "deny list entry is not a bare origin";
    }
    return "refused";
}

std::string metadata_url_origin(const std::string& url) {
    std::string scheme;
    std::string authority;
    if (!split_url(url, scheme, authority)) {
        return {};
    }
    return scheme + "://" + authority;
}

MetadataUrlDecision validate_metadata_url(const MetadataFetchPolicy& policy, const std::string& url) {
    std::string scheme;
    std::string authority;
    if (!split_url(url, scheme, authority)) {
        return MetadataUrlDecision::eMalformedUrl;
    }
    // Userinfo is the classic way to make an allowed origin read as the prefix of a hostile one.
    if (authority.find('@') != std::string::npos) {
        return MetadataUrlDecision::eMalformedUrl;
    }

    std::string host;
    if (!authority_host(authority, host)) {
        return MetadataUrlDecision::eMalformedUrl;
    }

    // Canonicalized once, before any list or callback sees it, so a deny entry cannot be bypassed by
    // a differently-cased scheme/host, an explicit or oddly-spelled default port, a run of trailing
    // FQDN dots, or an alternate textual spelling of the same IP literal. A URL that only canonicalizes
    // this far because of a malformed port or an all-dots host is refused outright.
    const auto canonical = canonicalize_origin_parts(scheme + "://" + authority);
    if (!canonical) {
        return MetadataUrlDecision::eMalformedUrl;
    }
    const auto canonical_origin = format_canonical_origin(*canonical);
    if (denies_origin(policy.denied_origins, canonical_origin)) {
        return MetadataUrlDecision::eOriginDenied;
    }
    if (!contains_origin(policy.allowed_origins, canonical_origin) &&
        !(policy.origin_allowance && policy.origin_allowance(canonical_origin))) {
        return MetadataUrlDecision::eOriginNotAllowed;
    }

    // Runs on the canonical scheme/host, matching what the origin decision and the origin_allowance
    // callback above just saw, rather than re-deriving the answer from raw, non-canonical text.
    if (canonical->scheme != "https") {
        const auto loopback_opt_out = policy.allow_plain_http_loopback && canonical->scheme == "http" &&
                                      is_loopback_host(canonical->host);
        if (!loopback_opt_out) {
            return MetadataUrlDecision::eSchemeNotAllowed;
        }
    }

    // A host written as an IP literal needs no lookup, so classify it here and refuse before
    // resolution is ever reached.
    boost::system::error_code error;
    const auto address = net::ip::make_address(host, error);
    if (!error) {
        return validate_metadata_address(policy, address.to_string());
    }
    return MetadataUrlDecision::eAllowed;
}

MetadataUrlDecision validate_metadata_address(const MetadataFetchPolicy& policy,
                                              const std::string& address_literal) {
    boost::system::error_code error;
    const auto address = net::ip::make_address(address_literal, error);
    if (error) {
        return MetadataUrlDecision::eMalformedUrl;
    }
    if (address.is_v6()) {
        return classify_v6(policy, address.to_v6());
    }
    return classify_v4(policy, address.to_v4());
}

namespace detail {

namespace {

/// Codepoints that can end a line somewhere downstream. C0 and DEL are the obvious ones; C1 and
/// U+2028/U+2029 are here because a JSON encoder emits them literally and JavaScript-based log
/// viewers treat the last two as line terminators, which is the same forgery by another route.
bool breaks_a_line(std::uint32_t codepoint) {
    namespace unicode = mcp::detail::constants::unicode;
    return codepoint < unicode::g_C0_CONTROLS_END || codepoint == unicode::g_DELETE ||
           (codepoint >= unicode::g_C1_CONTROLS_FIRST && codepoint <= unicode::g_C1_CONTROLS_LAST) ||
           codepoint == unicode::g_LINE_SEPARATOR || codepoint == unicode::g_PARAGRAPH_SEPARATOR;
}

/// Codepoints that re-order the text after them without ending the line: the explicit embeddings and
/// overrides (U+202A-U+202E), the isolates (U+2066-U+2069) and the implicit marks LRM (U+200E), RLM
/// (U+200F) and ALM (U+061C). They let a peer reverse the rendering of the rest of a diagnostic, so a
/// refusal can be made to read as its opposite. ALM sits in the Arabic block but is the same class as
/// RLM.
bool reorders_the_line(std::uint32_t codepoint) {
    namespace unicode = mcp::detail::constants::unicode;
    return (codepoint >= unicode::g_BIDI_EMBEDDINGS_FIRST &&
            codepoint <= unicode::g_BIDI_EMBEDDINGS_LAST) ||
           (codepoint >= unicode::g_BIDI_ISOLATES_FIRST &&
            codepoint <= unicode::g_BIDI_ISOLATES_LAST) ||
           codepoint == unicode::g_LEFT_TO_RIGHT_MARK || codepoint == unicode::g_RIGHT_TO_LEFT_MARK ||
           codepoint == unicode::g_ARABIC_LETTER_MARK;
}

}  // namespace

std::string sanitize_for_diagnostics(std::string_view value) {
    constexpr std::size_t max_length = 256;
    std::string cleaned;
    cleaned.reserve(std::min(value.size(), max_length));

    std::size_t index = 0;
    bool truncated = false;
    while (index < value.size()) {
        const auto decoded = mcp::detail::unicode::decode_utf8(value, index);

        // Ill-formed bytes are replaced one for one rather than copied, so the result is always
        // well-formed UTF-8 even when the input was not. Text that reaches here through a header
        // rather than through a JSON document has had nothing validate it.
        const std::size_t width = decoded.length == 0 ? 1 : decoded.length;
        if (cleaned.size() + width > max_length) {
            truncated = true;
            break;
        }
        if (decoded.length == 0) {
            cleaned.push_back('?');
        } else if (breaks_a_line(decoded.code_point) || reorders_the_line(decoded.code_point)) {
            cleaned.push_back(' ');
        } else {
            cleaned.append(value.substr(index, decoded.length));
        }
        index += width;
    }

    if (truncated) {
        cleaned += "...";
    }
    return cleaned;
}

}  // namespace detail

/// The target is sanitized for the message and kept raw in `target_`: every throw site passes a URL
/// or address from a peer, so sanitizing here covers all of them. `target()` returns the value
/// verbatim for callers that inspect it.
MetadataPolicyError::MetadataPolicyError(MetadataUrlDecision decision, std::string target)
    : std::runtime_error("OAuth metadata target refused (" + std::string(describe(decision)) +
                         "): " + detail::sanitize_for_diagnostics(target)),
      decision_(decision),
      target_(std::move(target)) {}

}  // namespace mcp::auth
