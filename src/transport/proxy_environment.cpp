// ProxyOptions::from_environment(), apart from the proxy rules in proxy.cpp so the tests can build
// those into their own program.

#include "../detail/proxy.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

namespace {

std::string environment(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): the caller reads the environment once.
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

/// The lower-case variable when it is set, else the upper-case one.
std::string environment_either(const char* lower_name, const char* upper_name) {
    auto value = environment(lower_name);
    return value.empty() ? environment(upper_name) : value;
}

}  // namespace

ProxyOptions ProxyOptions::from_environment() {
    ProxyOptions options;
    options.https_proxy = environment_either("https_proxy", "HTTPS_PROXY");
    if (environment("REQUEST_METHOD").empty()) {
        options.http_proxy = environment_either("http_proxy", "HTTP_PROXY");
    } else {
        // A CGI program: a request's Proxy header arrives as HTTP_PROXY. Windows matches variable
        // names without regard to case, so there http_proxy is that same variable.
#if !defined(_WIN32)
        options.http_proxy = environment("http_proxy");
#endif
    }
    const auto bypass = environment_either("no_proxy", "NO_PROXY");
    std::size_t begin = 0;
    while (begin <= bypass.size()) {
        const auto end = std::min(bypass.find(',', begin), bypass.size());
        auto entry = detail::trim_blanks(std::string_view(bypass).substr(begin, end - begin));
        if (!entry.empty()) {
            options.no_proxy.push_back(std::move(entry));
        }
        begin = end + 1;
    }
    detail::validate_proxy_options(options);
    return options;
}

}  // namespace mcp
