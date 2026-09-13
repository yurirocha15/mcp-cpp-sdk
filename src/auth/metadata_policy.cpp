#include <mcp/auth/metadata_policy.hpp>

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
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

/// Canonicalize an origin (`scheme://authority`) for comparison. Lowercases the scheme and host,
/// strips a single trailing dot from a non-literal host, and drops an explicit default port for the
/// scheme (`:443` for `https`, `:80` for `http`). The port itself, IP literals and IPv6 bracket forms
/// are otherwise preserved untouched, so a non-default port still distinguishes origins. An origin
/// that does not decompose into `scheme://authority` is returned unchanged, so it still compares
/// (and simply fails to match anything well-formed) instead of being dropped or throwing.
std::string canonicalize_origin(const std::string& origin) {
    std::string scheme;
    std::string authority;
    if (!split_url(origin, scheme, authority)) {
        return origin;
    }
    std::string host;
    if (!authority_host(authority, host)) {
        return origin;
    }

    const bool bracketed = authority.front() == '[';
    std::string port;
    if (bracketed) {
        const auto closing = authority.find(']');
        if (closing != std::string::npos && closing + 1 < authority.size() &&
            authority[closing + 1] == ':') {
            port = authority.substr(closing + 2);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string::npos) {
            port = authority.substr(colon + 1);
        }
    }

    const auto canonical_scheme = lowercase_ascii(scheme);

    boost::system::error_code error;
    net::ip::make_address(host, error);
    const bool is_ip_literal = !error;

    auto canonical_host = lowercase_ascii(host);
    if (!is_ip_literal && !canonical_host.empty() && canonical_host.back() == '.') {
        canonical_host.pop_back();
    }

    const bool default_port =
        (port == "443" && canonical_scheme == "https") || (port == "80" && canonical_scheme == "http");
    if (default_port) {
        port.clear();
    }

    std::string canonical_authority = bracketed ? "[" + canonical_host + "]" : canonical_host;
    if (!port.empty()) {
        canonical_authority += ":" + port;
    }
    return canonical_scheme + "://" + canonical_authority;
}

/// Compare a canonicalized origin against a policy list, canonicalizing each list entry at
/// comparison time so the caller never has to keep a normalized copy of the policy around.
bool contains_origin(const std::vector<std::string>& origins, const std::string& canonical_origin) {
    return std::any_of(origins.begin(), origins.end(), [&](const std::string& candidate) {
        return canonicalize_origin(candidate) == canonical_origin;
    });
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
    // a differently-cased scheme/host, an explicit default port, or a trailing FQDN dot.
    const auto canonical_origin = canonicalize_origin(scheme + "://" + authority);
    if (contains_origin(policy.denied_origins, canonical_origin)) {
        return MetadataUrlDecision::origin_denied;
    }
    if (!contains_origin(policy.allowed_origins, canonical_origin) &&
        !(policy.origin_allowance && policy.origin_allowance(canonical_origin))) {
        return MetadataUrlDecision::origin_not_allowed;
    }

    if (scheme != "https") {
        const auto loopback_opt_out =
            policy.allow_plain_http_loopback && scheme == "http" && is_loopback_host(host);
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

MetadataPolicyError::MetadataPolicyError(MetadataUrlDecision decision, std::string target)
    : std::runtime_error("OAuth metadata target refused (" + std::string(describe(decision)) +
                         "): " + target),
      decision_(decision),
      target_(std::move(target)) {}

}  // namespace mcp::auth
