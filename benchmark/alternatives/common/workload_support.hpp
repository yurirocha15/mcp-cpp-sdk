#pragma once

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace mcp_benchmark {

/// The environment variable @p name, or @p fallback when it is unset or empty.
inline std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

/// @p value as JSON writes a number, to 15 significant digits.
inline std::string number(double value) {
    std::ostringstream out;
    out << std::setprecision(15) << value;
    return out.str();
}

/// The number after the last dash of @p user_id, or 42 when it has none.
inline int user_number(const std::string& user_id) {
    const auto dash = user_id.rfind('-');
    if (dash == std::string::npos) {
        return 42;
    }
    try {
        return std::stoi(user_id.substr(dash + 1));
    } catch (...) {
        return 42;
    }
}

}  // namespace mcp_benchmark
