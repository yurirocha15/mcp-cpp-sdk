#include "../detail/proxy.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>

#include <openssl/evp.h>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

namespace net = boost::asio;

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char symbol) { return static_cast<char>(std::tolower(symbol)); });
    return text;
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}

int hex_value(char symbol) {
    if (symbol >= '0' && symbol <= '9') {
        return symbol - '0';
    }
    const char folded = static_cast<char>(std::tolower(static_cast<unsigned char>(symbol)));
    if (folded >= 'a' && folded <= 'f') {
        return folded - 'a' + 10;
    }
    return -1;
}

std::string percent_decode(std::string_view text, const std::string& origin) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '%') {
            result.push_back(text[index]);
            continue;
        }
        const int high = index + 2 < text.size() ? hex_value(text[index + 1]) : -1;
        const int low = index + 2 < text.size() ? hex_value(text[index + 2]) : -1;
        if (high < 0 || low < 0) {
            throw std::invalid_argument(origin + " holds a malformed percent-encoded credential");
        }
        result.push_back(static_cast<char>(high * 16 + low));
        index += 2;
    }
    return result;
}

std::string base64(const std::string& bytes) {
    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                       reinterpret_cast<const unsigned char*>(bytes.data()),
                                       static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(length));
    return encoded;
}

/// Splits `host[:port]` or `[v6][:port]`. The port is empty when the authority names none.
bool split_host_port(const std::string& authority, std::string& host, std::string& port) {
    if (!authority.empty() && authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string::npos ||
            (closing + 1 < authority.size() && authority[closing + 1] != ':')) {
            return false;
        }
        host = authority.substr(1, closing - 1);
        port = closing + 1 < authority.size() ? authority.substr(closing + 2) : "";
        return true;
    }
    const auto colon = authority.find(':');
    if (colon != std::string::npos && authority.find(':', colon + 1) != std::string::npos) {
        // A bare IPv6 address, which names no port.
        host = authority;
        port.clear();
        return true;
    }
    host = authority.substr(0, colon);
    port = colon == std::string::npos ? "" : authority.substr(colon + 1);
    return true;
}

bool is_port(const std::string& text) {
    if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    const auto value = std::stoul(text);
    return value > 0 && value <= 65535;
}

/// Whether one NO_PROXY entry covers @p host (lower case, no brackets) on @p port.
bool entry_matches(const std::string& raw_entry, const std::string& host, const std::string& port) {
    const auto entry = lower(trim(raw_entry));
    if (entry.empty()) {
        return false;
    }
    if (entry == "*") {
        return true;
    }
    boost::system::error_code error;
    const auto address = net::ip::make_address(host, error);
    const bool host_is_address = !error;
    if (entry.find('/') != std::string::npos) {
        if (!host_is_address) {
            return false;
        }
        if (address.is_v4()) {
            const auto network = net::ip::make_network_v4(entry, error);
            return !error &&
                   net::ip::network_v4(address.to_v4(), network.prefix_length()).canonical() ==
                       network.canonical();
        }
        const auto network = net::ip::make_network_v6(entry, error);
        return !error && net::ip::network_v6(address.to_v6(), network.prefix_length()).canonical() ==
                             network.canonical();
    }
    std::string entry_host;
    std::string entry_port;
    if (!split_host_port(entry, entry_host, entry_port)) {
        return false;
    }
    if (!entry_port.empty() && entry_port != port) {
        return false;
    }
    const auto entry_address = net::ip::make_address(entry_host, error);
    if (!error) {
        return host_is_address && entry_address == address;
    }
    if (entry_host.rfind("*.", 0) == 0) {
        entry_host.erase(0, 2);
    } else if (!entry_host.empty() && entry_host.front() == '.') {
        entry_host.erase(0, 1);
    }
    if (entry_host.empty()) {
        return false;
    }
    return host == entry_host || (host.size() > entry_host.size() && host.ends_with(entry_host) &&
                                  host[host.size() - entry_host.size() - 1] == '.');
}

}  // namespace

namespace detail {

ProxyRoute parse_proxy_url(const std::string& url, const std::string& origin) {
    std::string rest = trim(url);
    const auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        const auto scheme = lower(rest.substr(0, scheme_end));
        if (scheme != "http") {
            throw std::invalid_argument(origin + " names a " + scheme +
                                        ":// proxy; only http:// proxies are supported");
        }
        rest.erase(0, scheme_end + 3);
    }
    auto authority = rest.substr(0, rest.find_first_of("/?#"));
    ProxyRoute route;
    const auto at = authority.rfind('@');
    if (at != std::string::npos) {
        const auto userinfo = authority.substr(0, at);
        authority.erase(0, at + 1);
        const auto colon = userinfo.find(':');
        const auto user = percent_decode(userinfo.substr(0, colon), origin);
        const auto password = colon == std::string::npos
                                  ? std::string()
                                  : percent_decode(userinfo.substr(colon + 1), origin);
        route.authorization = "Basic " + base64(user + ":" + password);
    }
    if (!split_host_port(authority, route.host, route.port) || route.host.empty()) {
        throw std::invalid_argument(origin + " names no proxy host");
    }
    if (route.port.empty()) {
        route.port = "80";
    } else if (!is_port(route.port)) {
        throw std::invalid_argument(origin + " names an invalid proxy port");
    }
    return route;
}

void validate_proxy_options(const ProxyOptions& options) {
    if (!options.http_proxy.empty()) {
        (void)parse_proxy_url(options.http_proxy, "The http proxy");
    }
    if (!options.https_proxy.empty()) {
        (void)parse_proxy_url(options.https_proxy, "The https proxy");
    }
}

bool is_loopback_host(const std::string& host) {
    auto name = lower(host);
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    if (name == "localhost") {
        return true;
    }
    boost::system::error_code error;
    const auto address = net::ip::make_address(name, error);
    if (error) {
        return false;
    }
    if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        return net::ip::make_address_v4(net::ip::v4_mapped, address.to_v6()).is_loopback();
    }
    return address.is_loopback();
}

std::optional<ProxyRoute> select_proxy(const ProxyOptions& options, bool https, const std::string& host,
                                       const std::string& port) {
    const auto& url = https ? options.https_proxy : options.http_proxy;
    if (url.empty() || is_loopback_host(host)) {
        return std::nullopt;
    }
    auto name = lower(host);
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    for (const auto& entry : options.no_proxy) {
        if (entry_matches(entry, name, port)) {
            return std::nullopt;
        }
    }
    return parse_proxy_url(url, https ? "The https proxy" : "The http proxy");
}

std::string authority_of(const std::string& host, const std::string& port, bool https) {
    const bool v6 = host.find(':') != std::string::npos;
    std::string authority = v6 ? "[" + host + "]" : host;
    if (port != (https ? "443" : "80")) {
        authority += ":" + port;
    }
    return authority;
}

}  // namespace detail

}  // namespace mcp
