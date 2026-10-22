#pragma once

#include "header_value.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mcp::detail {

/// A tool argument that an `x-mcp-header` annotation mirrors into an `Mcp-Param-{name}` header.
struct ParamHeader {
    std::string name;               ///< The annotation's value: the `{name}` of the header.
    std::vector<std::string> path;  ///< The chain of `properties` keys leading to the argument.
};

/// `Mcp-Param-{Name}` headers of one request: {Name, value as received} pairs.
using ParamHeaderValues = std::vector<std::pair<std::string, std::string>>;

/// The largest integer an `x-mcp-header` argument may hold: 2^53 - 1.
inline constexpr std::int64_t g_MAX_SAFE_HEADER_INTEGER = (std::int64_t{1} << 53) - 1;

/// Compares ASCII letters without regard to case, as HTTP compares field names.
inline bool equals_ignoring_case(std::string_view lhs, std::string_view rhs) {
    const auto lower = [](char symbol) {
        return symbol >= 'A' && symbol <= 'Z' ? static_cast<char>(symbol - 'A' + 'a') : symbol;
    };
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [&](char left, char right) { return lower(left) == lower(right); });
}

/// RFC 9110 `token`: one or more `tchar`.
inline bool is_http_token(std::string_view text) {
    constexpr std::string_view symbols = "!#$%&'*+-.^_`|~";
    return !text.empty() && std::all_of(text.begin(), text.end(), [&](char raw) {
        const bool alphanumeric =
            (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z') || (raw >= '0' && raw <= '9');
        return alphanumeric || symbols.find(raw) != std::string_view::npos;
    });
}

namespace param_headers {

/// Keywords whose value is one subschema.
inline constexpr std::array<std::string_view, 12> g_SCHEMA_KEYWORDS = {"additionalProperties",
                                                                       "unevaluatedProperties",
                                                                       "propertyNames",
                                                                       "items",
                                                                       "additionalItems",
                                                                       "unevaluatedItems",
                                                                       "contains",
                                                                       "not",
                                                                       "if",
                                                                       "then",
                                                                       "else",
                                                                       "contentSchema"};
/// Keywords whose value is an array of subschemas.
inline constexpr std::array<std::string_view, 5> g_SCHEMA_ARRAY_KEYWORDS = {"allOf", "anyOf", "oneOf",
                                                                            "prefixItems", "items"};
/// Keywords whose value maps names to subschemas, other than `properties`.
inline constexpr std::array<std::string_view, 5> g_SCHEMA_MAP_KEYWORDS = {
    "patternProperties", "dependentSchemas", "$defs", "definitions", "dependencies"};

inline std::string escape_pointer_token(std::string_view token) {
    std::string escaped;
    for (const char symbol : token) {
        if (symbol == '~') {
            escaped += "~0";
        } else if (symbol == '/') {
            escaped += "~1";
        } else {
            escaped += symbol;
        }
    }
    return escaped;
}

/// Whether @p type names one primitive an annotation may mirror, alone or alongside "null".
inline bool is_mirrorable_type(const nlohmann::json& type) {
    const auto primitive = [](const nlohmann::json& name) {
        return name == "string" || name == "integer" || name == "boolean";
    };
    if (type.is_string()) {
        return primitive(type);
    }
    if (!type.is_array() || type.empty() || type.size() > 2) {
        return false;
    }
    const auto primitives = std::count_if(type.begin(), type.end(), primitive);
    const auto nulls = std::count(type.begin(), type.end(), nlohmann::json("null"));
    return primitives == 1 && static_cast<std::size_t>(primitives + nulls) == type.size();
}

class Collector {
   public:
    std::vector<ParamHeader> headers;

    /// @p reachable: whether the schema at @p location is reached from the root through
    /// `properties` alone, with @p path the keys taken.
    void walk(const nlohmann::json& schema, const std::string& location, std::vector<std::string>& path,
              bool reachable) {
        if (!schema.is_object()) {
            return;
        }
        if (const auto annotation = schema.find("x-mcp-header"); annotation != schema.end()) {
            record(schema, *annotation, location, path, reachable);
        }
        for (const auto& [keyword, value] : schema.items()) {
            const auto child = location + "/" + escape_pointer_token(keyword);
            if (keyword == "properties") {
                walk_map(value, child, path, reachable);
            } else if (contains(g_SCHEMA_MAP_KEYWORDS, keyword)) {
                walk_map(value, child, path, false);
            } else if (value.is_array() && contains(g_SCHEMA_ARRAY_KEYWORDS, keyword)) {
                for (std::size_t index = 0; index < value.size(); ++index) {
                    walk(value[index], child + "/" + std::to_string(index), path, false);
                }
            } else if (contains(g_SCHEMA_KEYWORDS, keyword)) {
                walk(value, child, path, false);
            }
        }
    }

   private:
    template <std::size_t N>
    static bool contains(const std::array<std::string_view, N>& keywords, std::string_view keyword) {
        return std::find(keywords.begin(), keywords.end(), keyword) != keywords.end();
    }

    void walk_map(const nlohmann::json& map, const std::string& location,
                  std::vector<std::string>& path, bool reachable) {
        if (!map.is_object()) {
            return;
        }
        for (const auto& [name, subschema] : map.items()) {
            path.push_back(name);
            walk(subschema, location + "/" + escape_pointer_token(name), path, reachable);
            path.pop_back();
        }
    }

    void record(const nlohmann::json& schema, const nlohmann::json& annotation,
                const std::string& location, const std::vector<std::string>& path, bool reachable) {
        const auto refuse = [&location](std::string_view problem) {
            return std::invalid_argument("x-mcp-header at " + (location.empty() ? "/" : location) +
                                         " " + std::string(problem));
        };
        if (!reachable || path.empty()) {
            throw refuse("is not on a property reached from the root through properties alone");
        }
        if (!annotation.is_string()) {
            throw refuse("must be a string");
        }
        const auto& name = annotation.get_ref<const std::string&>();
        if (!is_http_token(name)) {
            throw refuse("must be a non-empty HTTP token");
        }
        const auto type = schema.find("type");
        if (type == schema.end() || !is_mirrorable_type(*type)) {
            throw refuse(R"(must be on a property of type "string", "integer" or "boolean")");
        }
        for (const auto& existing : headers) {
            if (equals_ignoring_case(existing.name, name)) {
                throw refuse("repeats the header name " + name);
            }
        }
        headers.push_back({name, path});
    }
};

/// Parses a decimal integer header value, allowing a fraction of zeros ("42.0"), into @p value.
inline bool parse_header_integer(std::string_view text, std::int64_t& value) {
    const auto point = text.find('.');
    const auto whole = text.substr(0, point);
    if (point != std::string_view::npos) {
        const auto fraction = text.substr(point + 1);
        if (fraction.empty() || fraction.find_first_not_of('0') != std::string_view::npos) {
            return false;
        }
    }
    if (whole.empty() || whole == "-") {
        return false;
    }
    const auto* end = whole.data() + whole.size();
    const auto [stop, error] = std::from_chars(whole.data(), end, value);
    return error == std::errc{} && stop == end;
}

/// The argument at @p path in @p arguments, or null when it is absent or JSON null.
inline const nlohmann::json* argument_at(const nlohmann::json& arguments,
                                         const std::vector<std::string>& path) {
    const nlohmann::json* current = &arguments;
    for (const auto& key : path) {
        if (!current->is_object()) {
            return nullptr;
        }
        const auto next = current->find(key);
        if (next == current->end()) {
            return nullptr;
        }
        current = &*next;
    }
    return current->is_null() ? nullptr : current;
}

}  // namespace param_headers

/// The arguments @p input_schema mirrors into headers. Throws std::invalid_argument, naming the
/// annotation's location, when an `x-mcp-header` annotation breaks a constraint of the
/// specification: it must be a non-empty HTTP token, unique ignoring case, on a property of a
/// primitive type other than `number` that the root reaches through `properties` alone. Values
/// inside non-schema keywords (`enum`, `const`, `default`, `examples`) and property names are
/// never annotations.
inline std::vector<ParamHeader> collect_param_headers(const nlohmann::json& input_schema) {
    param_headers::Collector collector;
    std::vector<std::string> path;
    collector.walk(input_schema, "", path, true);
    return std::move(collector.headers);
}

/// Checks the `Mcp-Param-*` headers of a tools/call against its @p arguments and returns why they
/// do not match, or nullopt when they do. Each argument in @p params that is present and not null
/// needs exactly one header whose value, once decoded, is that argument: the same text, the same
/// integer (compared numerically) or `true`/`false`. Headers no annotation names are ignored. A
/// header for an argument that is absent or null is not compared, but it must still be a single
/// header with a valid value: a server rejects a recognized header with invalid characters.
inline std::optional<std::string> param_header_mismatch(const std::vector<ParamHeader>& params,
                                                        const nlohmann::json& arguments,
                                                        const ParamHeaderValues& headers) {
    for (const auto& param : params) {
        const auto header_name = "Mcp-Param-" + param.name;
        const std::string* received = nullptr;
        for (const auto& [name, header_value] : headers) {
            if (equals_ignoring_case(name, param.name)) {
                if (received != nullptr) {
                    return "Duplicate " + header_name + " header";
                }
                received = &header_value;
            }
        }
        std::optional<std::string> decoded;
        if (received != nullptr) {
            decoded = decode_header_value(*received);
            if (!decoded) {
                return "Invalid " + header_name + " header value";
            }
        }
        const auto* value = param_headers::argument_at(arguments, param.path);
        if (value == nullptr) {
            continue;
        }
        if (received == nullptr) {
            return "Missing " + header_name + " header";
        }
        bool matches = false;
        if (value->is_string()) {
            matches = *decoded == value->get_ref<const std::string&>();
        } else if (value->is_boolean()) {
            matches = *decoded == (value->get<bool>() ? "true" : "false");
        } else if (value->is_number_integer() || value->is_number_float()) {
            std::int64_t body = 0;
            if (value->is_number_unsigned()) {
                const auto unsigned_body = value->get<std::uint64_t>();
                if (unsigned_body > static_cast<std::uint64_t>(g_MAX_SAFE_HEADER_INTEGER)) {
                    return header_name + " argument is outside the safe integer range";
                }
                body = static_cast<std::int64_t>(unsigned_body);
            } else if (value->is_number_integer()) {
                body = value->get<std::int64_t>();
            } else {
                // Range first: converting a double outside int64 to an integer is undefined.
                const auto number = value->get<double>();
                constexpr auto limit = static_cast<double>(g_MAX_SAFE_HEADER_INTEGER);
                if (!(number >= -limit && number <= limit) ||
                    number != static_cast<double>(static_cast<std::int64_t>(number))) {
                    return header_name + " cannot carry the argument's value";
                }
                body = static_cast<std::int64_t>(number);
            }
            if (body > g_MAX_SAFE_HEADER_INTEGER || body < -g_MAX_SAFE_HEADER_INTEGER) {
                return header_name + " argument is outside the safe integer range";
            }
            std::int64_t header_number = 0;
            matches =
                param_headers::parse_header_integer(*decoded, header_number) && header_number == body;
        } else {
            return header_name + " cannot carry the argument's value";
        }
        if (!matches) {
            return header_name + " header does not match the request body";
        }
    }
    return std::nullopt;
}

}  // namespace mcp::detail
