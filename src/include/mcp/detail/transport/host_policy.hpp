#pragma once

#include "mcp/detail/net/proxy.hpp"

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcp::detail {

/// The host a Host header value names, as the policy compares it: without the port or the brackets
/// of an IPv6 literal, in lower case, without one trailing dot. Empty for a value that names none.
inline std::string normalized_host(std::string_view value) {
    std::string_view host = value;
    if (!host.empty() && host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string_view::npos || (close + 1 < host.size() && host[close + 1] != ':')) {
            return {};
        }
        host = host.substr(1, close - 1);
    } else if (std::count(host.begin(), host.end(), ':') == 1) {
        host = host.substr(0, host.find(':'));
    }
    std::string result(host);
    std::transform(result.begin(), result.end(), result.begin(), [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                    : character;
    });
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result;
}

/// Whether @p endpoint is a loopback address, counting an IPv4 loopback address that a dual-stack
/// socket shows as IPv4-mapped IPv6.
inline bool is_loopback_endpoint(const boost::asio::ip::tcp::endpoint& endpoint) {
    const auto address = endpoint.address();
    if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6())
            .is_loopback();
    }
    return address.is_loopback();
}

/// The Host header values a server serves, against DNS rebinding: a page whose name an attacker
/// points at a loopback address reaches a local server with the attacker's name in Host.
///
/// Without a list, a connection to a loopback address must name a loopback host (`localhost` or a
/// loopback address); other connections are not checked. With a list, every connection must name a
/// host on it.
class HostPolicy {
   public:
    HostPolicy() = default;

    /// A policy that accepts only @p hosts, or the loopback rule for an empty list.
    /// @throws std::invalid_argument For an entry that names no host.
    explicit HostPolicy(const std::vector<std::string>& hosts) {
        for (const auto& host : hosts) {
            auto normalized = normalized_host(host);
            if (normalized.empty()) {
                throw std::invalid_argument("An allowed host names no host: '" + host + "'");
            }
            allowed_.insert(std::move(normalized));
        }
    }

    /// Whether a request whose Host header is @p host, or that has none (or several), may be served
    /// on a connection whose local address is a loopback one or not.
    [[nodiscard]] bool allows(const std::optional<std::string_view>& host,
                              bool loopback_connection) const {
        if (allowed_.empty() && !loopback_connection) {
            return true;
        }
        if (!host) {
            return false;
        }
        const auto normalized = normalized_host(*host);
        if (normalized.empty()) {
            return false;
        }
        return allowed_.empty() ? is_loopback_host(normalized) : allowed_.contains(normalized);
    }

   private:
    std::unordered_set<std::string> allowed_;
};

}  // namespace mcp::detail
