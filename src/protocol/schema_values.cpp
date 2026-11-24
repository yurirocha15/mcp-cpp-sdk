#include "schema_values.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mcp::detail {

std::string_view type_name(const nlohmann::json& value) {
    switch (value.type()) {
        case nlohmann::json::value_t::null:
            return "null";
        case nlohmann::json::value_t::boolean:
            return "boolean";
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            return "integer";
        case nlohmann::json::value_t::number_float:
            return "number";
        case nlohmann::json::value_t::string:
            return "string";
        case nlohmann::json::value_t::array:
            return "array";
        case nlohmann::json::value_t::object:
            return "object";
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            break;
    }
    return "unknown";
}

bool has_type(const nlohmann::json& value, std::string_view type) {
    if (type == "number") {
        return value.is_number();
    }
    if (type == "integer") {
        if (value.is_number_integer()) {
            return true;
        }
        if (!value.is_number_float()) {
            return false;
        }
        const double number = value.get<double>();
        return std::isfinite(number) && std::trunc(number) == number;
    }
    return type_name(value) == type;
}

namespace {

/// -1, 0 or 1 as @p a is less than, equal to or greater than @p b. Unordered doubles give 0.
template <typename T>
int order(const T& a, const T& b) {
    if (a < b) {
        return -1;
    }
    return a > b ? 1 : 0;
}

/// Compares the integer @p integer with the finite double @p number exactly: negative, zero or
/// positive as @p integer is less than, equal to or greater than @p number. Converting the integer
/// to a double instead would round integers beyond 2^53.
int compare_integer_with_double(const nlohmann::json& integer, double number) {
    constexpr double two_pow_63 = 9223372036854775808.0;
    constexpr double two_pow_64 = 18446744073709551616.0;
    if (integer.is_number_unsigned()) {
        if (number < 0) {
            return 1;
        }
        if (number >= two_pow_64) {
            return -1;
        }
        const double whole = std::floor(number);
        const auto truncated = static_cast<std::uint64_t>(whole);
        const auto value = integer.get<std::uint64_t>();
        if (value != truncated) {
            return value < truncated ? -1 : 1;
        }
        return number > whole ? -1 : 0;
    }
    if (number >= two_pow_63) {
        return -1;
    }
    if (number < -two_pow_63) {
        return 1;
    }
    const double whole = std::floor(number);
    const auto truncated = static_cast<std::int64_t>(whole);
    const auto value = integer.get<std::int64_t>();
    if (value != truncated) {
        return value < truncated ? -1 : 1;
    }
    return number > whole ? -1 : 0;
}

}  // namespace

int compare_numbers(const nlohmann::json& left, const nlohmann::json& right) {
    if (left.is_number_integer() && right.is_number_integer()) {
        const bool left_unsigned = left.is_number_unsigned();
        const bool right_unsigned = right.is_number_unsigned();
        if (left_unsigned && right_unsigned) {
            const auto a = left.get<std::uint64_t>();
            const auto b = right.get<std::uint64_t>();
            return order(a, b);
        }
        if (!left_unsigned && !right_unsigned) {
            const auto a = left.get<std::int64_t>();
            const auto b = right.get<std::int64_t>();
            return order(a, b);
        }
        if (left_unsigned) {
            const auto b = right.get<std::int64_t>();
            if (b < 0) {
                return 1;
            }
            const auto a = left.get<std::uint64_t>();
            return order(a, static_cast<std::uint64_t>(b));
        }
        return -compare_numbers(right, left);
    }
    if (left.is_number_integer()) {
        return compare_integer_with_double(left, right.get<double>());
    }
    if (right.is_number_integer()) {
        return -compare_integer_with_double(right, left.get<double>());
    }
    const double a = left.get<double>();
    const double b = right.get<double>();
    return order(a, b);
}

bool is_non_finite(const nlohmann::json& value) {
    return value.is_number_float() && !std::isfinite(value.get<double>());
}

void charge(std::size_t& work, std::size_t cost) {
    if (work < cost) {
        // NOLINTNEXTLINE(bugprone-std-exception-baseclass): see the declaration.
        throw WorkBudgetExceeded{};
    }
    work -= cost;
}

bool json_equal(const nlohmann::json& left, const nlohmann::json& right, std::size_t& work) {
    constexpr std::size_t g_comparison_units = 8;  // What the vendored validator charges for one.
    constexpr std::size_t g_string_bytes_per_unit = 64;
    charge(work, g_comparison_units + (left.is_string() ? left.get_ref<const std::string&>().size() /
                                                              g_string_bytes_per_unit
                                                        : 0));
    // JSON cannot carry NaN or Infinity; nlohmann writes them as null, which is what the peer
    // receives, so one nested in the value compares as null, as check() treats one at the top.
    if (is_non_finite(left) || is_non_finite(right)) {
        return (is_non_finite(left) || left.is_null()) && (is_non_finite(right) || right.is_null());
    }
    if (left.is_number() && right.is_number()) {
        return compare_numbers(left, right) == 0;
    }
    if (left.type() != right.type()) {
        return false;
    }
    if (left.is_array()) {
        return std::ranges::equal(left.get_ref<const nlohmann::json::array_t&>(),
                                  right.get_ref<const nlohmann::json::array_t&>(),
                                  [&work](const nlohmann::json& one, const nlohmann::json& other) {
                                      return json_equal(one, other, work);
                                  });
    }
    if (left.is_object()) {
        if (left.size() != right.size()) {
            return false;
        }
        for (const auto& [name, member] : left.items()) {
            const auto other = right.find(name);
            if (other == right.end() || !json_equal(member, *other, work)) {
                return false;
            }
        }
        return true;
    }
    return left == right;
}

namespace {

/// A hash under which values json_equal() finds equal hash alike: numbers hash by their value as a
/// double, NaN and Infinity as null, and object members without regard to order.
std::size_t equality_hash(const nlohmann::json& value) {
    const auto mix = [](std::size_t seed, std::size_t part) {
        constexpr auto g_golden = static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
        constexpr unsigned g_left = 6;
        constexpr unsigned g_right = 2;
        return seed ^ (part + g_golden + (seed << g_left) + (seed >> g_right));
    };
    enum Kind : std::uint8_t {
        eNumber = 1,
        eString,
        eArray,
        eObject,
        eOther,
    };
    if (value.is_number() && !is_non_finite(value)) {
        const double number = value.get<double>();
        return mix(eNumber, number == 0 ? 0 : std::hash<double>()(number));
    }
    if (value.is_string()) {
        return mix(eString, std::hash<std::string>()(value.get_ref<const std::string&>()));
    }
    if (value.is_array()) {
        std::size_t seed = eArray;
        for (const auto& element : value) {
            seed = mix(seed, equality_hash(element));
        }
        return seed;
    }
    if (value.is_object()) {
        std::size_t sum = 0;
        for (const auto& [name, member] : value.items()) {
            sum += mix(std::hash<std::string>()(name), equality_hash(member));
        }
        return mix(eObject, sum);
    }
    if (value.is_boolean()) {
        return mix(eOther, value.get<bool>() ? 1 : 2);
    }
    return mix(eOther, 0);  // null, NaN and Infinity
}

}  // namespace

bool all_unique(const nlohmann::json& array, std::size_t& work) {
    constexpr std::size_t g_pairwise_size = 8;
    if (array.size() <= g_pairwise_size) {
        for (std::size_t i = 0; i < array.size(); ++i) {
            for (std::size_t j = i + 1; j < array.size(); ++j) {
                if (json_equal(array[i], array[j], work)) {
                    return false;
                }
            }
        }
        return true;
    }
    std::vector<std::pair<std::size_t, std::size_t>> hashed;  // hash, index
    hashed.reserve(array.size());
    for (std::size_t index = 0; index < array.size(); ++index) {
        charge(work, 1);
        hashed.emplace_back(equality_hash(array[index]), index);
    }
    std::ranges::sort(hashed);
    for (std::size_t i = 0; i < hashed.size(); ++i) {
        // Hashes a peer chose can all collide; each comparison is charged.
        for (std::size_t j = i + 1; j < hashed.size() && hashed[j].first == hashed[i].first; ++j) {
            if (json_equal(array[hashed[i].second], array[hashed[j].second], work)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace mcp::detail
