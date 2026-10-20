#include "native_schema.hpp"

#include "../detail/diagnostic_text.hpp"

#include <mcp/detail/regex.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::detail {

struct NativeSchema::Node {
    bool reject_all = false;         ///< The boolean schema `false`.
    std::vector<std::string> types;  ///< `type`, in the schema's order; empty accepts any.
    std::optional<nlohmann::json> enum_values;
    std::optional<nlohmann::json> const_value;
    std::optional<nlohmann::json> minimum;
    std::optional<nlohmann::json> maximum;
    std::optional<nlohmann::json> exclusive_minimum;
    std::optional<nlohmann::json> exclusive_maximum;
    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;
    std::optional<Regex> pattern;
    std::unique_ptr<Node> items;
    std::optional<std::size_t> min_items;
    std::optional<std::size_t> max_items;
    std::vector<std::string> property_names;  ///< Sorted; property_schemas is in the same order.
    std::vector<Node> property_schemas;
    std::vector<std::string> required;
    std::unique_ptr<Node> additional_properties;
    std::optional<std::size_t> min_properties;
    std::optional<std::size_t> max_properties;
};

namespace {

using Node = NativeSchema::Node;

constexpr std::string_view g_dialect = "https://json-schema.org/draft/2020-12/schema";

/// Keywords jsoncons implements for 2020-12 that the native evaluation does not. A schema using
/// one is left to jsoncons; any keyword in neither list is one jsoncons ignores, and so is ignored
/// here.
bool is_unsupported_keyword(std::string_view keyword) {
    static constexpr std::array<std::string_view, 26> unsupported = {
        "$anchor",
        "$defs",
        "$dynamicAnchor",
        "$dynamicRef",
        "$ref",
        "additionalItems",
        "allOf",
        "anyOf",
        "contains",
        "contentEncoding",
        "contentMediaType",
        "dependencies",
        "dependentRequired",
        "dependentSchemas",
        "else",
        "if",
        "multipleOf",
        "not",
        "oneOf",
        "patternProperties",
        "prefixItems",
        "propertyNames",
        "then",
        "uniqueItems",
        "unevaluatedItems",
        "unevaluatedProperties",
    };
    return std::find(unsupported.begin(), unsupported.end(), keyword) != unsupported.end();
}

/// A size keyword's value. The metaschema has checked that it is a non-negative integer; one
/// beyond std::size_t saturates.
std::size_t size_value(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number > std::numeric_limits<std::size_t>::max()
                   ? std::numeric_limits<std::size_t>::max()
                   : static_cast<std::size_t>(number);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number < 0 ? 0 : static_cast<std::size_t>(number);
    }
    const double number = value.get<double>();
    const auto largest = static_cast<double>(std::numeric_limits<std::size_t>::max());
    if (!(number > 0)) {
        return 0;
    }
    return number >= largest ? std::numeric_limits<std::size_t>::max()
                             : static_cast<std::size_t>(number);
}

std::unique_ptr<Node> compile_node(const nlohmann::json& schema, bool root);

/// Compiles @p schema into @p node; false when the schema is not supported.
bool compile_into(const nlohmann::json& schema, bool root, Node& node) {
    if (schema.is_boolean()) {
        node.reject_all = !schema.get<bool>();
        return true;
    }
    if (!schema.is_object()) {
        return false;
    }
    for (const auto& [keyword, value] : schema.items()) {
        if (keyword == "$schema") {
            if (!root || !value.is_string()) {
                return false;
            }
            std::string_view uri = value.get_ref<const std::string&>();
            if (!uri.empty() && uri.back() == '#') {
                uri.remove_suffix(1);
            }
            if (uri != g_dialect) {
                return false;
            }
        } else if (is_unsupported_keyword(keyword)) {
            return false;
        } else if (keyword == "type") {
            if (value.is_string()) {
                node.types.push_back(value.get<std::string>());
            } else {
                for (const auto& type : value) {
                    node.types.push_back(type.get<std::string>());
                }
            }
        } else if (keyword == "enum") {
            node.enum_values = value;
        } else if (keyword == "const") {
            node.const_value = value;
        } else if (keyword == "minimum") {
            node.minimum = value;
        } else if (keyword == "maximum") {
            node.maximum = value;
        } else if (keyword == "exclusiveMinimum") {
            node.exclusive_minimum = value;
        } else if (keyword == "exclusiveMaximum") {
            node.exclusive_maximum = value;
        } else if (keyword == "minLength") {
            node.min_length = size_value(value);
        } else if (keyword == "maxLength") {
            node.max_length = size_value(value);
        } else if (keyword == "pattern") {
            try {
                node.pattern.emplace(value.get_ref<const std::string&>());
            } catch (const RegexError&) {
                return false;
            }
        } else if (keyword == "items") {
            node.items = compile_node(value, false);
            if (!node.items) {
                return false;
            }
        } else if (keyword == "minItems") {
            node.min_items = size_value(value);
        } else if (keyword == "maxItems") {
            node.max_items = size_value(value);
        } else if (keyword == "properties") {
            // nlohmann objects iterate in key order, so the names come out sorted.
            for (const auto& [name, property_schema] : value.items()) {
                Node property;
                if (!compile_into(property_schema, false, property)) {
                    return false;
                }
                node.property_names.push_back(name);
                node.property_schemas.push_back(std::move(property));
            }
        } else if (keyword == "required") {
            node.required = value.get<std::vector<std::string>>();
        } else if (keyword == "additionalProperties") {
            node.additional_properties = compile_node(value, false);
            if (!node.additional_properties) {
                return false;
            }
        } else if (keyword == "minProperties") {
            node.min_properties = size_value(value);
        } else if (keyword == "maxProperties") {
            node.max_properties = size_value(value);
        }
        // Anything else is an annotation or a keyword jsoncons ignores.
    }
    return true;
}

std::unique_ptr<Node> compile_node(const nlohmann::json& schema, bool root) {
    auto node = std::make_unique<Node>();
    if (!compile_into(schema, root, *node)) {
        return nullptr;
    }
    return node;
}

/// The JSON Schema type name of @p value, as jsoncons reports it.
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

/// Compares two numbers exactly when both are integers, and as doubles otherwise: negative,
/// zero or positive as @p left is less than, equal to or greater than @p right.
int compare_numbers(const nlohmann::json& left, const nlohmann::json& right) {
    if (left.is_number_integer() && right.is_number_integer()) {
        const bool left_unsigned = left.is_number_unsigned();
        const bool right_unsigned = right.is_number_unsigned();
        if (left_unsigned && right_unsigned) {
            const auto a = left.get<std::uint64_t>();
            const auto b = right.get<std::uint64_t>();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        if (!left_unsigned && !right_unsigned) {
            const auto a = left.get<std::int64_t>();
            const auto b = right.get<std::int64_t>();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        if (left_unsigned) {
            const auto b = right.get<std::int64_t>();
            if (b < 0) {
                return 1;
            }
            const auto a = left.get<std::uint64_t>();
            return a < static_cast<std::uint64_t>(b) ? -1 : (a > static_cast<std::uint64_t>(b) ? 1 : 0);
        }
        return -compare_numbers(right, left);
    }
    const double a = left.get<double>();
    const double b = right.get<double>();
    return a < b ? -1 : (a > b ? 1 : 0);
}

/// JSON Schema equality: numbers are equal when their values are, whatever their representation.
/// nlohmann's operator== converts an unsigned integer to a signed one when comparing the two, so
/// it finds -1 equal to 18446744073709551615.
bool json_equal(const nlohmann::json& left, const nlohmann::json& right) {
    if (left.is_number() && right.is_number()) {
        return compare_numbers(left, right) == 0;
    }
    if (left.type() != right.type()) {
        return false;
    }
    if (left.is_array()) {
        return std::equal(left.begin(), left.end(), right.begin(), right.end(), json_equal);
    }
    if (left.is_object()) {
        if (left.size() != right.size()) {
            return false;
        }
        for (const auto& [name, member] : left.items()) {
            const auto other = right.find(name);
            if (other == right.end() || !json_equal(member, *other)) {
                return false;
            }
        }
        return true;
    }
    return left == right;
}

/// Number of code points in UTF-8 @p text.
std::size_t code_points(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char byte) {
        return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U;
    }));
}

/// One evaluation of a value against a NativeSchema.
class Evaluation {
   public:
    Evaluation(std::size_t max_violations, ValidationResult& result)
        : max_violations_(max_violations), result_(result) {}

    /// Checks @p value against @p node; false once enough violations are recorded.
    bool check(const Node& node, const nlohmann::json& value) {
        if (value.is_number_float() && !std::isfinite(value.get<double>())) {
            // JSON cannot carry NaN or Infinity; nlohmann writes them as null, which is what the
            // peer would receive, so that is what is checked.
            static const nlohmann::json null_value;
            return check(node, null_value);
        }
        if (node.reject_all) {
            return report("No value is allowed here");
        }
        if (!node.types.empty() &&
            std::none_of(node.types.begin(), node.types.end(),
                         [&](const std::string& type) { return has_type(value, type); })) {
            if (!report(type_message(node.types, value))) {
                return false;
            }
        }
        if (node.enum_values &&
            std::none_of(node.enum_values->begin(), node.enum_values->end(),
                         [&](const nlohmann::json& allowed) { return json_equal(allowed, value); })) {
            if (!report("Value is not one of the values the enum lists")) {
                return false;
            }
        }
        if (node.const_value && !json_equal(value, *node.const_value)) {
            if (!report("Value is not the const value")) {
                return false;
            }
        }
        if (value.is_number()) {
            return check_number(node, value);
        }
        if (value.is_string()) {
            return check_string(node, value.get_ref<const std::string&>());
        }
        if (value.is_array()) {
            return check_array(node, value);
        }
        if (value.is_object()) {
            return check_object(node, value);
        }
        return true;
    }

   private:
    static std::string type_message(const std::vector<std::string>& types,
                                    const nlohmann::json& value) {
        std::string message = "Expected ";
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (i > 0) {
                message += ", ";
                if (i + 1 == types.size()) {
                    message += "or ";
                }
            }
            message += types[i];
        }
        message += ", found ";
        message += type_name(value);
        return message;
    }

    bool check_number(const Node& node, const nlohmann::json& value) {
        if (node.minimum && compare_numbers(value, *node.minimum) < 0 &&
            !report("Value must be at least " + node.minimum->dump())) {
            return false;
        }
        if (node.maximum && compare_numbers(value, *node.maximum) > 0 &&
            !report("Value must be at most " + node.maximum->dump())) {
            return false;
        }
        if (node.exclusive_minimum && compare_numbers(value, *node.exclusive_minimum) <= 0 &&
            !report("Value must be greater than " + node.exclusive_minimum->dump())) {
            return false;
        }
        if (node.exclusive_maximum && compare_numbers(value, *node.exclusive_maximum) >= 0 &&
            !report("Value must be less than " + node.exclusive_maximum->dump())) {
            return false;
        }
        return true;
    }

    bool check_string(const Node& node, const std::string& text) {
        if (node.min_length || node.max_length) {
            const auto length = code_points(text);
            if (node.min_length && length < *node.min_length &&
                !report("Number of characters must be at least " + std::to_string(*node.min_length))) {
                return false;
            }
            if (node.max_length && length > *node.max_length &&
                !report("Number of characters must be at most " + std::to_string(*node.max_length))) {
                return false;
            }
        }
        if (node.pattern && !node.pattern->search(text) &&
            !report("String does not match the pattern " + node.pattern->pattern())) {
            return false;
        }
        return true;
    }

    bool check_array(const Node& node, const nlohmann::json& array) {
        if (node.items) {
            for (std::size_t index = 0; index < array.size(); ++index) {
                const auto length = path_.size();
                path_ += '/';
                path_ += std::to_string(index);
                const bool more = check(*node.items, array[index]);
                path_.resize(length);
                if (!more) {
                    return false;
                }
            }
        }
        if (node.min_items && array.size() < *node.min_items &&
            !report("Array must have at least " + std::to_string(*node.min_items) + " items, found " +
                    std::to_string(array.size()))) {
            return false;
        }
        if (node.max_items && array.size() > *node.max_items &&
            !report("Array must have at most " + std::to_string(*node.max_items) + " items, found " +
                    std::to_string(array.size()))) {
            return false;
        }
        return true;
    }

    bool check_object(const Node& node, const nlohmann::json& object) {
        for (const auto& name : node.required) {
            if (!object.contains(name) && !report("Required property '" + name + "' is missing")) {
                return false;
            }
        }
        if (!node.property_names.empty() || node.additional_properties) {
            for (const auto& [name, member] : object.items()) {
                const auto found =
                    std::lower_bound(node.property_names.begin(), node.property_names.end(), name);
                const Node* member_schema = nullptr;
                if (found != node.property_names.end() && *found == name) {
                    member_schema = &node.property_schemas[static_cast<std::size_t>(
                        found - node.property_names.begin())];
                } else if (node.additional_properties) {
                    member_schema = node.additional_properties.get();
                }
                if (member_schema == nullptr) {
                    continue;
                }
                const auto length = path_.size();
                append_token(name);
                const bool more =
                    member_schema->reject_all && member_schema == node.additional_properties.get()
                        ? report("Additional property '" + name + "' is not allowed")
                        : check(*member_schema, member);
                path_.resize(length);
                if (!more) {
                    return false;
                }
            }
        }
        if (node.min_properties && object.size() < *node.min_properties &&
            !report("Object must have at least " + std::to_string(*node.min_properties) +
                    " properties, found " + std::to_string(object.size()))) {
            return false;
        }
        if (node.max_properties && object.size() > *node.max_properties &&
            !report("Object must have at most " + std::to_string(*node.max_properties) +
                    " properties, found " + std::to_string(object.size()))) {
            return false;
        }
        return true;
    }

    /// Appends @p name to the JSON Pointer being built, escaped as RFC 6901 requires.
    void append_token(std::string_view name) {
        path_ += '/';
        for (const char character : name) {
            if (character == '~') {
                path_ += "~0";
            } else if (character == '/') {
                path_ += "~1";
            } else {
                path_ += character;
            }
        }
    }

    bool report(const std::string& message) {
        result_.valid = false;
        if (result_.violations.size() < max_violations_) {
            result_.violations.push_back(
                {sanitize_for_diagnostics(path_), sanitize_for_diagnostics(message)});
        }
        return result_.violations.size() < max_violations_;
    }

    std::size_t max_violations_;
    ValidationResult& result_;
    std::string path_;
};

}  // namespace

NativeSchema::NativeSchema(std::shared_ptr<const Node> root) : root_(std::move(root)) {}

std::optional<NativeSchema> NativeSchema::compile(const nlohmann::json& schema) {
    auto root = compile_node(schema, true);
    if (!root) {
        return std::nullopt;
    }
    return NativeSchema(std::shared_ptr<const Node>(std::move(root)));
}

void NativeSchema::validate(const nlohmann::json& instance, std::size_t max_violations,
                            ValidationResult& result) const {
    Evaluation(max_violations, result).check(*root_, instance);
}

}  // namespace mcp::detail
