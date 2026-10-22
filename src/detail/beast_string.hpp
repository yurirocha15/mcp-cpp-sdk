#pragma once

#include <boost/beast/core/string.hpp>

#include <string_view>

namespace mcp::detail {

/// View Beast's string_view as a std::string_view. In older Boost releases, 1.74 among them,
/// Beast's string_view is boost::string_view, which has no conversion to std::string_view.
inline std::string_view to_std_string_view(boost::beast::string_view value) noexcept {
    return {value.data(), value.size()};
}

}  // namespace mcp::detail
