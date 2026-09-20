#include <mcp/auth/metadata_policy.hpp>

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp::auth {

namespace net = boost::asio;

namespace {

constexpr std::uint32_t g_octet_mask = 0xFFU;
constexpr int g_octet1_shift = 24;
constexpr int g_octet2_shift = 16;
constexpr int g_octet3_shift = 8;

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
        const auto closing = authority.find(']');
        if (closing == std::string::npos || closing == 1) {
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
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
    return value;
}

/// Parse a URL port substring strictly: ASCII digits only, no sign, no whitespace, and in range
/// [0, 65535]. Leading zeros are tolerated and simply absorbed into the numeric value (`"00443"` and
/// `"0443"` both parse to 443), but anything that is not a plain unsigned decimal number — including
/// an empty string, a sign, embedded whitespace, or a value that overflows a 16-bit port — returns
/// `nullopt` rather than silently truncating or wrapping.
std::optional<std::uint32_t> parse_strict_port(const std::string& port_text) {
    if (port_text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : port_text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<std::uint64_t>(character - '0');
        if (value > 65535U) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
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
/// Lowercases the scheme and, for a non-IP-literal host, the host; strips every trailing dot from
/// such a host (an FQDN dot run), and returns `nullopt` if that leaves it empty. For a host that
/// parses as an IP literal (IPv4, or IPv6 with or without brackets), replaces it with its normalized
/// textual form instead of lowercasing it, so an expanded and a compressed IPv6 spelling of the same
/// address compare equal; a literal is never dot-stripped. Parses an explicit port strictly and
/// numerically (see `parse_strict_port`) and drops it when it equals the scheme's default (`443` for
/// `https`, `80` for `http`); a malformed port makes the whole origin fail to canonicalize.
///
/// Returns `nullopt` — meaning "does not canonicalize to a bare origin" — when `origin` does not
/// decompose into `scheme://authority` with nothing following (a path, query or fragment means it was
/// never a bare origin to begin with), when the host is malformed, or when a port is present but is
/// not a plain in-range decimal number.
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

    if (port && ((*port == 443U && canonical_scheme == "https") ||
                 (*port == 80U && canonical_scheme == "http"))) {
        port.reset();
    }

    return CanonicalOrigin{std::move(canonical_scheme), std::move(canonical_host), port, bracketed};
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
    return std::any_of(origins.begin(), origins.end(), [&](const std::string& candidate) {
        const auto canonical_candidate = canonicalize_origin(candidate);
        return canonical_candidate && *canonical_candidate == canonical_origin;
    });
}

/// Compare a canonicalized origin against the deny list, requiring every entry to be a bare origin.
///
/// Silently discarding a deny entry the way `contains_origin` discards an allow entry would be
/// fail-open: an author who writes `https://evil.example/` would block nothing while believing the
/// origin was refused. Nor is such an entry quietly reinterpreted as the origin it resembles;
/// guessing at a security rule the author did not write trades one silent misapplication for
/// another. An entry that does not canonicalize is a configuration error, so it is reported as one.
///
/// The whole list is examined before a match can be returned, so a malformed entry is reported even
/// when an earlier entry already matched and even when no entry describes the target at hand. A
/// policy carrying one therefore refuses every target until it is corrected.
///
/// @throws MetadataPolicyError With `denied_origin_entry_malformed`, naming the offending entry.
bool denies_origin(const std::vector<std::string>& origins, const std::string& canonical_origin) {
    bool denied = false;
    for (const auto& candidate : origins) {
        const auto canonical_candidate = canonicalize_origin(candidate);
        if (!canonical_candidate) {
            throw MetadataPolicyError(MetadataUrlDecision::denied_origin_entry_malformed, candidate);
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
    const auto octet1 = static_cast<std::uint32_t>(value >> g_octet1_shift) & g_octet_mask;
    const auto octet2 = static_cast<std::uint32_t>(value >> g_octet2_shift) & g_octet_mask;
    const auto octet3 = static_cast<std::uint32_t>(value >> g_octet3_shift) & g_octet_mask;

    if (octet1 == 127) {
        return policy.allow_plain_http_loopback ? MetadataUrlDecision::allowed
                                                : MetadataUrlDecision::address_loopback;
    }
    if (octet1 == 169 && octet2 == 254) {
        return MetadataUrlDecision::address_link_local;
    }
    if (octet1 == 10 || (octet1 == 172 && octet2 >= 16 && octet2 <= 31) ||
        (octet1 == 192 && octet2 == 168)) {
        return MetadataUrlDecision::address_private;
    }
    if (value == 0xFFFFFFFFU) {
        return MetadataUrlDecision::address_multicast;
    }
    if (octet1 >= 224 && octet1 <= 239) {
        return MetadataUrlDecision::address_multicast;
    }
    if (octet1 == 0 || octet1 >= 240) {
        return MetadataUrlDecision::address_reserved;
    }
    if (octet1 == 100 && octet2 >= 64 && octet2 <= 127) {
        return MetadataUrlDecision::address_reserved;
    }
    if (octet1 == 192 && octet2 == 0 && (octet3 == 0 || octet3 == 2)) {
        return MetadataUrlDecision::address_reserved;
    }
    if (octet1 == 198 && (octet2 == 18 || octet2 == 19)) {
        return MetadataUrlDecision::address_reserved;
    }
    if (octet1 == 198 && octet2 == 51 && octet3 == 100) {
        return MetadataUrlDecision::address_reserved;
    }
    if (octet1 == 203 && octet2 == 0 && octet3 == 113) {
        return MetadataUrlDecision::address_reserved;
    }
    return MetadataUrlDecision::allowed;
}

/// Recognize the ::ffff:a.b.c.d form so an IPv4 target cannot be smuggled through an IPv6 literal.
bool mapped_v4(const net::ip::address_v6& address, net::ip::address_v4& mapped) {
    const auto bytes = address.to_bytes();
    for (std::size_t index = 0; index < 10; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    if (bytes[10] != 0xFF || bytes[11] != 0xFF) {
        return false;
    }
    const net::ip::address_v4::bytes_type v4_bytes{bytes[12], bytes[13], bytes[14], bytes[15]};
    mapped = net::ip::address_v4(v4_bytes);
    return true;
}

MetadataUrlDecision classify_v6(const MetadataFetchPolicy& policy, const net::ip::address_v6& address) {
    net::ip::address_v4 mapped;
    if (mapped_v4(address, mapped)) {
        return classify_v4(policy, mapped);
    }
    if (address.is_unspecified()) {
        return MetadataUrlDecision::address_reserved;
    }
    if (address.is_loopback()) {
        return policy.allow_plain_http_loopback ? MetadataUrlDecision::allowed
                                                : MetadataUrlDecision::address_loopback;
    }
    if (address.is_link_local()) {
        return MetadataUrlDecision::address_link_local;
    }
    if (address.is_multicast()) {
        return MetadataUrlDecision::address_multicast;
    }
    const auto bytes = address.to_bytes();
    if ((bytes[0] & 0xFEU) == 0xFCU) {
        return MetadataUrlDecision::address_private;
    }
    if (address.is_site_local()) {
        return MetadataUrlDecision::address_private;
    }
    return MetadataUrlDecision::allowed;
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
        case MetadataUrlDecision::allowed:
            return "allowed";
        case MetadataUrlDecision::malformed_url:
            return "URL is malformed or carries userinfo";
        case MetadataUrlDecision::scheme_not_allowed:
            return "scheme is not https and the loopback opt-out does not apply";
        case MetadataUrlDecision::origin_denied:
            return "origin is on the application deny list";
        case MetadataUrlDecision::origin_not_allowed:
            return "origin is not on the application allow list";
        case MetadataUrlDecision::address_link_local:
            return "address is link-local";
        case MetadataUrlDecision::address_private:
            return "address is in a private range";
        case MetadataUrlDecision::address_loopback:
            return "address is loopback and the loopback opt-out is not enabled";
        case MetadataUrlDecision::address_multicast:
            return "address is multicast or broadcast";
        case MetadataUrlDecision::address_reserved:
            return "address is reserved or non-routable";
        case MetadataUrlDecision::redirect_limit_exceeded:
            return "redirect chain exceeded the configured bound";
        case MetadataUrlDecision::response_too_large:
            return "response exceeded the configured size cap";
        case MetadataUrlDecision::denied_origin_entry_malformed:
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
        return MetadataUrlDecision::malformed_url;
    }
    // Userinfo is the classic way to make an allowed origin read as the prefix of a hostile one.
    if (authority.find('@') != std::string::npos) {
        return MetadataUrlDecision::malformed_url;
    }

    std::string host;
    if (!authority_host(authority, host)) {
        return MetadataUrlDecision::malformed_url;
    }

    // Canonicalized once, before any list or callback sees it, so a deny entry cannot be bypassed by
    // a differently-cased scheme/host, an explicit or oddly-spelled default port, a run of trailing
    // FQDN dots, or an alternate textual spelling of the same IP literal. A URL that only canonicalizes
    // this far because of a malformed port or an all-dots host is refused outright.
    const auto canonical = canonicalize_origin_parts(scheme + "://" + authority);
    if (!canonical) {
        return MetadataUrlDecision::malformed_url;
    }
    const auto canonical_origin = format_canonical_origin(*canonical);
    if (denies_origin(policy.denied_origins, canonical_origin)) {
        return MetadataUrlDecision::origin_denied;
    }
    if (!contains_origin(policy.allowed_origins, canonical_origin) &&
        !(policy.origin_allowance && policy.origin_allowance(canonical_origin))) {
        return MetadataUrlDecision::origin_not_allowed;
    }

    // Runs on the canonical scheme/host, matching what the origin decision and the origin_allowance
    // callback above just saw, rather than re-deriving the answer from raw, non-canonical text.
    if (canonical->scheme != "https") {
        const auto loopback_opt_out = policy.allow_plain_http_loopback && canonical->scheme == "http" &&
                                      is_loopback_host(canonical->host);
        if (!loopback_opt_out) {
            return MetadataUrlDecision::scheme_not_allowed;
        }
    }

    // A host written as an IP literal needs no lookup, so classify it here and refuse before
    // resolution is ever reached.
    boost::system::error_code error;
    const auto address = net::ip::make_address(host, error);
    if (!error) {
        return validate_metadata_address(policy, address.to_string());
    }
    return MetadataUrlDecision::allowed;
}

MetadataUrlDecision validate_metadata_address(const MetadataFetchPolicy& policy,
                                              const std::string& address_literal) {
    boost::system::error_code error;
    const auto address = net::ip::make_address(address_literal, error);
    if (error) {
        return MetadataUrlDecision::malformed_url;
    }
    if (address.is_v6()) {
        return classify_v6(policy, address.to_v6());
    }
    return classify_v4(policy, address.to_v4());
}

namespace detail {

namespace {

/// One decoded UTF-8 sequence. `length` is 0 when the bytes at the offset are not a well-formed
/// sequence, in which case `codepoint` is meaningless.
struct Utf8Sequence {
    std::size_t length{0};
    std::uint32_t codepoint{0};
};

/// Decode the UTF-8 sequence starting at `index`, rejecting every ill-formed encoding rather than
/// accepting the bytes and hoping: a truncated tail, a bad continuation byte, an overlong form, a
/// surrogate, or a value past U+10FFFF. Ill-formed input is what a peer sends when it wants the
/// consumer of the log, not the log line itself, to misbehave.
Utf8Sequence decode_utf8(std::string_view value, std::size_t index) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (lead < 0x80) {
        return {1, lead};
    }

    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        codepoint = lead & 0x1FU;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        codepoint = lead & 0x0FU;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        codepoint = lead & 0x07U;
    } else {
        return {};  // A continuation byte with no lead, or an invalid lead.
    }

    if (index + length > value.size()) {
        return {};
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(value[index + offset]);
        if ((continuation & 0xC0) != 0x80) {
            return {};
        }
        codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }

    const bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                          (length == 4 && codepoint < 0x10000);
    const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
    if (overlong || surrogate || codepoint > 0x10FFFF) {
        return {};
    }
    return {length, codepoint};
}

/// Codepoints that can end a line somewhere downstream. C0 and DEL are the obvious ones; C1 and
/// U+2028/U+2029 are here because a JSON encoder emits them literally and JavaScript-based log
/// viewers treat the last two as line terminators, which is the same forgery by another route.
bool breaks_a_line(std::uint32_t codepoint) {
    return codepoint < 0x20 || codepoint == 0x7f || (codepoint >= 0x80 && codepoint <= 0x9f) ||
           codepoint == 0x2028 || codepoint == 0x2029;
}

}  // namespace

std::string sanitize_for_diagnostics(std::string_view value) {
    constexpr std::size_t max_length = 256;
    std::string cleaned;
    cleaned.reserve(std::min(value.size(), max_length));

    std::size_t index = 0;
    bool truncated = false;
    while (index < value.size()) {
        const auto decoded = decode_utf8(value, index);

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
        } else if (breaks_a_line(decoded.codepoint)) {
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

// The target is sanitized for the MESSAGE and kept raw in `target_`. Every throw site passes a URL
// or address that came from a peer, and sanitizing here rather than at each of them means a throw
// site cannot forget. `target()` still returns the value verbatim, because a caller inspecting it
// programmatically wants the real URL, not one with its control characters flattened.
MetadataPolicyError::MetadataPolicyError(MetadataUrlDecision decision, std::string target)
    : std::runtime_error("OAuth metadata target refused (" + std::string(describe(decision)) +
                         "): " + detail::sanitize_for_diagnostics(target)),
      decision_(decision),
      target_(std::move(target)) {}

}  // namespace mcp::auth
