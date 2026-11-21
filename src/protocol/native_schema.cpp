#include "native_schema.hpp"

#include "../detail/constants.hpp"
#include "../detail/diagnostic_text.hpp"

#include <mcp/detail/regex.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
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
    std::optional<double> multiple_of;
    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;
    std::optional<Regex> pattern;
    std::vector<Node> prefix_items;
    std::unique_ptr<Node> items;
    std::unique_ptr<Node> contains;
    std::optional<std::size_t> min_contains;
    std::optional<std::size_t> max_contains;
    bool unique_items = false;
    std::optional<std::size_t> min_items;
    std::optional<std::size_t> max_items;
    std::vector<std::string> property_names;  ///< Sorted; property_schemas is in the same order.
    std::vector<Node> property_schemas;
    std::vector<Regex> pattern_regexes;  ///< `patternProperties`; pattern_schemas in the same order.
    std::vector<Node> pattern_schemas;
    std::unique_ptr<Node> additional_properties;
    std::unique_ptr<Node> property_names_schema;
    std::vector<std::string> required;
    std::vector<std::string> dependent_required_names;  ///< Same order as dependent_required_lists.
    std::vector<std::vector<std::string>> dependent_required_lists;
    std::vector<std::string> dependent_schema_names;  ///< Same order as dependent_schemas.
    std::vector<Node> dependent_schemas;
    std::optional<std::size_t> min_properties;
    std::optional<std::size_t> max_properties;
    std::vector<Node> all_of;
    std::vector<Node> any_of;
    std::vector<Node> one_of;
    std::unique_ptr<Node> not_schema;
    std::unique_ptr<Node> if_schema;
    std::unique_ptr<Node> then_schema;
    std::unique_ptr<Node> else_schema;
    bool has_applicators = false;  ///< Any of all_of to else_schema is set.
};

namespace {

using Node = NativeSchema::Node;

/// Keywords jsoncons implements for 2020-12 that the native evaluation does not. A schema using
/// one is left to jsoncons; any keyword in neither list is one jsoncons ignores, and so is ignored
/// here.
bool is_unsupported_keyword(std::string_view keyword) {
    static constexpr std::array<std::string_view, 9> s_unsupported = {
        "$anchor",         "$defs",        "$dynamicAnchor",   "$dynamicRef",           "$ref",
        "additionalItems", "dependencies", "unevaluatedItems", "unevaluatedProperties",
    };
    return std::find(s_unsupported.begin(), s_unsupported.end(), keyword) != s_unsupported.end();
}

/// A size keyword's value, or nothing when it is not a non-negative number. One beyond std::size_t
/// saturates.
std::optional<std::size_t> size_value(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number > std::numeric_limits<std::size_t>::max()
                   ? std::numeric_limits<std::size_t>::max()
                   : static_cast<std::size_t>(number);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < 0) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(number);
    }
    if (!value.is_number_float()) {
        return std::nullopt;
    }
    const double number = value.get<double>();
    const auto largest = static_cast<double>(std::numeric_limits<std::size_t>::max());
    if (!(number >= 0)) {
        return std::nullopt;
    }
    return number >= largest ? std::numeric_limits<std::size_t>::max()
                             : static_cast<std::size_t>(number);
}

/// The strings of @p value, which must be an array of strings; nothing otherwise.
std::optional<std::vector<std::string>> string_list(const nlohmann::json& value) {
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> strings;
    strings.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            return std::nullopt;
        }
        strings.push_back(item.get<std::string>());
    }
    return strings;
}

bool compile_into(const nlohmann::json& schema, bool root, Node& node);

std::unique_ptr<Node> compile_node(const nlohmann::json& schema) {
    auto node = std::make_unique<Node>();
    if (!compile_into(schema, false, *node)) {
        return nullptr;
    }
    return node;
}

/// Compiles the non-empty array of schemas @p value into @p nodes; false when it is not one.
bool compile_list(const nlohmann::json& value, std::vector<Node>& nodes) {
    if (!value.is_array() || value.empty()) {
        return false;
    }
    nodes.reserve(value.size());
    for (const auto& item : value) {
        Node node;
        if (!compile_into(item, false, node)) {
            return false;
        }
        nodes.push_back(std::move(node));
    }
    return true;
}

/// Compiles the object of schemas @p value into @p names (in key order) and @p nodes.
bool compile_map(const nlohmann::json& value, std::vector<std::string>& names,
                 std::vector<Node>& nodes) {
    if (!value.is_object()) {
        return false;
    }
    // nlohmann objects iterate in key order, so the names come out sorted.
    for (const auto& [name, item] : value.items()) {
        Node node;
        if (!compile_into(item, false, node)) {
            return false;
        }
        names.push_back(name);
        nodes.push_back(std::move(node));
    }
    return true;
}

bool compile_number(const nlohmann::json& value, std::optional<nlohmann::json>& target) {
    if (!value.is_number()) {
        return false;
    }
    target = value;
    return true;
}

bool compile_size(const nlohmann::json& value, std::optional<std::size_t>& target) {
    target = size_value(value);
    return target.has_value();
}

bool compile_schema(const nlohmann::json& value, std::unique_ptr<Node>& target) {
    target = compile_node(value);
    return target != nullptr;
}

/// Compiles @p schema into @p node; false when the schema is not supported. Never throws for a
/// malformed schema: a value of the wrong shape leaves the schema to jsoncons, which reports it.
bool compile_into(const nlohmann::json& schema, bool root, Node& node) {
    if (schema.is_boolean()) {
        node.reject_all = !schema.get<bool>();
        return true;
    }
    if (!schema.is_object()) {
        return false;
    }
    bool supported = true;
    for (const auto& [keyword, value] : schema.items()) {
        if (!supported) {
            break;
        }
        if (keyword == "$schema") {
            if (!root || !value.is_string()) {
                return false;
            }
            std::string_view uri = value.get_ref<const std::string&>();
            if (!uri.empty() && uri.back() == '#') {
                uri.remove_suffix(1);
            }
            supported = uri == mcp::detail::constants::native_schema::g_DIALECT;
        } else if (is_unsupported_keyword(keyword)) {
            supported = false;
        } else if (keyword == "type") {
            if (value.is_string()) {
                node.types.push_back(value.get<std::string>());
            } else if (auto types = string_list(value)) {
                node.types = std::move(*types);
            } else {
                supported = false;
            }
        } else if (keyword == "enum") {
            supported = value.is_array();
            node.enum_values = value;
        } else if (keyword == "const") {
            node.const_value = value;
        } else if (keyword == "minimum") {
            supported = compile_number(value, node.minimum);
        } else if (keyword == "maximum") {
            supported = compile_number(value, node.maximum);
        } else if (keyword == "exclusiveMinimum") {
            supported = compile_number(value, node.exclusive_minimum);
        } else if (keyword == "exclusiveMaximum") {
            supported = compile_number(value, node.exclusive_maximum);
        } else if (keyword == "multipleOf") {
            supported = value.is_number() && value.get<double>() > 0;
            if (supported) {
                node.multiple_of = value.get<double>();
            }
        } else if (keyword == "minLength") {
            supported = compile_size(value, node.min_length);
        } else if (keyword == "maxLength") {
            supported = compile_size(value, node.max_length);
        } else if (keyword == "pattern") {
            if (!value.is_string()) {
                return false;
            }
            try {
                node.pattern.emplace(value.get_ref<const std::string&>());
            } catch (const RegexError&) {
                return false;
            }
        } else if (keyword == "prefixItems") {
            supported = compile_list(value, node.prefix_items);
        } else if (keyword == "items") {
            supported = compile_schema(value, node.items);
        } else if (keyword == "contains") {
            supported = compile_schema(value, node.contains);
        } else if (keyword == "minContains") {
            supported = compile_size(value, node.min_contains);
        } else if (keyword == "maxContains") {
            supported = compile_size(value, node.max_contains);
        } else if (keyword == "uniqueItems") {
            supported = value.is_boolean();
            node.unique_items = supported && value.get<bool>();
        } else if (keyword == "minItems") {
            supported = compile_size(value, node.min_items);
        } else if (keyword == "maxItems") {
            supported = compile_size(value, node.max_items);
        } else if (keyword == "properties") {
            supported = compile_map(value, node.property_names, node.property_schemas);
        } else if (keyword == "patternProperties") {
            std::vector<std::string> patterns;
            supported = compile_map(value, patterns, node.pattern_schemas);
            for (std::size_t i = 0; supported && i < patterns.size(); ++i) {
                try {
                    node.pattern_regexes.emplace_back(patterns[i]);
                } catch (const RegexError&) {
                    return false;
                }
            }
        } else if (keyword == "additionalProperties") {
            supported = compile_schema(value, node.additional_properties);
        } else if (keyword == "propertyNames") {
            supported = compile_schema(value, node.property_names_schema);
        } else if (keyword == "required") {
            if (auto names = string_list(value)) {
                node.required = std::move(*names);
            } else {
                supported = false;
            }
        } else if (keyword == "dependentRequired") {
            supported = value.is_object();
            if (supported) {
                for (const auto& [name, list] : value.items()) {
                    auto names = string_list(list);
                    if (!names) {
                        supported = false;
                        break;
                    }
                    node.dependent_required_names.push_back(name);
                    node.dependent_required_lists.push_back(std::move(*names));
                }
            }
        } else if (keyword == "dependentSchemas") {
            supported = compile_map(value, node.dependent_schema_names, node.dependent_schemas);
        } else if (keyword == "minProperties") {
            supported = compile_size(value, node.min_properties);
        } else if (keyword == "maxProperties") {
            supported = compile_size(value, node.max_properties);
        } else if (keyword == "allOf") {
            supported = compile_list(value, node.all_of);
        } else if (keyword == "anyOf") {
            supported = compile_list(value, node.any_of);
        } else if (keyword == "oneOf") {
            supported = compile_list(value, node.one_of);
        } else if (keyword == "not") {
            supported = compile_schema(value, node.not_schema);
        } else if (keyword == "if") {
            supported = compile_schema(value, node.if_schema);
        } else if (keyword == "then") {
            supported = compile_schema(value, node.then_schema);
        } else if (keyword == "else") {
            supported = compile_schema(value, node.else_schema);
        }
        // Anything else is an annotation or a keyword jsoncons ignores, such as `format` and, in
        // 2020-12, `contentEncoding` and `contentMediaType`.
    }
    node.has_applicators = !node.all_of.empty() || !node.any_of.empty() || !node.one_of.empty() ||
                           node.not_schema || node.if_schema;
    return supported;
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

/// Compares two numbers exactly: negative, zero or positive as @p left is less than, equal to or
/// greater than @p right.
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
    if (left.is_number_integer()) {
        return compare_integer_with_double(left, right.get<double>());
    }
    if (right.is_number_integer()) {
        return -compare_integer_with_double(right, left.get<double>());
    }
    const double a = left.get<double>();
    const double b = right.get<double>();
    return a < b ? -1 : (a > b ? 1 : 0);
}

/// Whether @p value is a multiple of @p divisor, decided as jsoncons decides it: in doubles, with
/// zero a multiple of everything and a quotient that overflows a multiple of nothing.
bool is_multiple_of(double value, double divisor) {
    if (value == 0) {
        return true;
    }
    if (!std::isfinite(value / divisor)) {
        return false;
    }
    const double remainder = std::remainder(value, divisor);
    const double epsilon = std::nextafter(value, 0) - value;
    return std::fabs(remainder) < std::fabs(epsilon);
}

/// Thrown when an evaluation needs more work than the validation's budget holds.
struct WorkBudgetExceeded {};

/// Thrown when evaluations nest deeper than g_MAX_EVAL_DEPTH.
struct EvaluationTooDeep {};

/// Thrown when pattern matching needs more steps than the validation's budget holds.
struct PatternBudgetExceeded {};

bool is_non_finite(const nlohmann::json& value) {
    return value.is_number_float() && !std::isfinite(value.get<double>());
}

/// Work charged for reading a whole string: one unit per this many bytes, as jsoncons charges its
/// whole-string keywords.
constexpr std::size_t g_STRING_BYTES_PER_UNIT = 16;

/// Work charged for a property name looked up or added to a location: one unit, and one more per
/// this many bytes of the name, as jsoncons charges its location copies.
constexpr std::size_t g_NAME_BYTES_PER_UNIT = 64;

/// Takes @p cost units from @p work, or throws when it holds fewer.
void charge(std::size_t& work, std::size_t cost) {
    if (work < cost) {
        throw WorkBudgetExceeded{};
    }
    work -= cost;
}

/// JSON Schema equality: numbers are equal when their values are, whatever their representation.
/// nlohmann's operator== converts an unsigned integer to a signed one when comparing the two, so
/// it finds -1 equal to 18446744073709551615.
///
/// A comparison costs @p work, which pays for values as large as the schema and the instance
/// together: an enum of many long values compared with a long value is quadratic otherwise.
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
        return std::equal(left.begin(), left.end(), right.begin(), right.end(),
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

/// A hash under which values json_equal() finds equal hash alike: numbers hash by their value as a
/// double, NaN and Infinity as null, and object members without regard to order.
std::size_t equality_hash(const nlohmann::json& value) {
    const auto mix = [](std::size_t seed, std::size_t part) {
        constexpr std::size_t g_golden = static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
        constexpr unsigned g_left = 6;
        constexpr unsigned g_right = 2;
        return seed ^ (part + g_golden + (seed << g_left) + (seed >> g_right));
    };
    enum Kind : std::size_t {
        eNumber = 1,
        eString,
        eArray,
        eObject,
        eOther
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

/// Where an evaluation records its violations: the validation's result, a bounded buffer for an
/// `anyOf` or `oneOf` branch, or nowhere when only validity counts.
struct Sink {
    std::vector<SchemaViolation>* violations;  ///< Null when only validity counts.
    std::size_t limit;
    bool valid = true;
};

/// One evaluation of a value against a NativeSchema.
class Evaluation {
   public:
    Evaluation(std::size_t max_violations, std::size_t& pattern_steps, std::size_t& work,
               ValidationResult& result)
        : root_{&result.violations, max_violations},
          sink_(&root_),
          pattern_steps_(pattern_steps),
          work_(work) {}

    /// Validity of the evaluation so far.
    [[nodiscard]] bool valid() const { return root_.valid; }

    /// Checks @p value against @p node; false once the current sink takes no more violations.
    bool check(const Node& node, const nlohmann::json& value) {
        if (is_non_finite(value)) {
            // JSON cannot carry NaN or Infinity; nlohmann writes them as null, which is what the
            // peer would receive, so that is what is checked.
            static const nlohmann::json s_null_value;
            return check(node, s_null_value);
        }
        const Frame frame(*this);
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
        if (node.enum_values && std::none_of(node.enum_values->begin(), node.enum_values->end(),
                                             [&](const nlohmann::json& allowed) {
                                                 return json_equal(allowed, value, work_);
                                             })) {
            if (!report("Value is not one of the values the enum lists")) {
                return false;
            }
        }
        if (node.const_value && !json_equal(value, *node.const_value, work_)) {
            if (!report("Value is not the const value")) {
                return false;
            }
        }
        bool more = true;
        if (value.is_number()) {
            more = check_number(node, value);
        } else if (value.is_string()) {
            more = check_string(node, value.get_ref<const std::string&>());
        } else if (value.is_array()) {
            more = check_array(node, value);
        } else if (value.is_object()) {
            more = check_object(node, value);
        }
        return more && (!node.has_applicators || check_applicators(node, value));
    }

   private:
    /// One step of the path to the value being checked: a member's name, or an element's index
    /// when name is null. The name is the instance's own key, which outlives the step.
    struct Location {
        const std::string* name;
        std::size_t index;
    };

    /// The steps to the value being checked. The first few are kept inline, so that a validation
    /// of a shallow value allocates nothing for them.
    class Locations {
       public:
        void push(Location location) {
            if (size_ < inline_.size()) {
                inline_[size_] = location;
            } else {
                overflow_.push_back(location);
            }
            ++size_;
        }
        void pop() {
            --size_;
            if (size_ >= inline_.size()) {
                overflow_.pop_back();
            }
        }
        [[nodiscard]] std::size_t size() const { return size_; }
        [[nodiscard]] const Location& operator[](std::size_t index) const {
            return index < inline_.size() ? inline_[index] : overflow_[index - inline_.size()];
        }

       private:
        static constexpr std::size_t g_inline_steps = 8;
        std::array<Location, g_inline_steps> inline_{};
        std::vector<Location> overflow_;
        std::size_t size_ = 0;
    };

    /// Adds a step to the current location until destroyed.
    class LocationScope {
       public:
        LocationScope(Evaluation& evaluation, Location location) : evaluation_(evaluation) {
            evaluation_.locations_.push(location);
        }
        ~LocationScope() { evaluation_.locations_.pop(); }
        LocationScope(const LocationScope&) = delete;
        LocationScope& operator=(const LocationScope&) = delete;
        LocationScope(LocationScope&&) = delete;
        LocationScope& operator=(LocationScope&&) = delete;

       private:
        Evaluation& evaluation_;
    };

    /// One level of evaluation nesting, charged one unit of work.
    class Frame {
       public:
        explicit Frame(Evaluation& evaluation) : evaluation_(evaluation) {
            charge(evaluation_.work_, 1);
            if (++evaluation_.depth_ > mcp::detail::constants::schema::g_MAX_EVAL_DEPTH) {
                throw EvaluationTooDeep{};
            }
        }
        ~Frame() { --evaluation_.depth_; }
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&&) = delete;
        Frame& operator=(Frame&&) = delete;

       private:
        Evaluation& evaluation_;
    };

    /// Makes @p sink the current sink until destroyed.
    class SinkScope {
       public:
        SinkScope(Evaluation& evaluation, Sink& sink)
            : evaluation_(evaluation), outer_(evaluation.sink_) {
            evaluation_.sink_ = &sink;
        }
        ~SinkScope() { evaluation_.sink_ = outer_; }
        SinkScope(const SinkScope&) = delete;
        SinkScope& operator=(const SinkScope&) = delete;
        SinkScope(SinkScope&&) = delete;
        SinkScope& operator=(SinkScope&&) = delete;

       private:
        Evaluation& evaluation_;
        Sink* outer_;
    };

    /// Whether @p value passes @p node, its violations going to @p buffer (up to @p limit) or, when
    /// @p buffer is null, nowhere. The evaluation stops at the first violation it cannot keep.
    bool passes(const Node& node, const nlohmann::json& value,
                std::vector<SchemaViolation>* buffer = nullptr, std::size_t limit = 0) {
        Sink sink{buffer, limit};
        const SinkScope scope(*this, sink);
        (void)check(node, value);
        return sink.valid;
    }

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
        if (node.multiple_of && !is_multiple_of(value.get<double>(), *node.multiple_of) &&
            !report("Value is not a multiple of " + nlohmann::json(*node.multiple_of).dump())) {
            return false;
        }
        return true;
    }

    bool matches(const Regex& regex, const std::string& text) {
        const auto matched = regex.search(text, pattern_steps_);
        if (!matched) {
            throw PatternBudgetExceeded();
        }
        return *matched;
    }

    bool check_string(const Node& node, const std::string& text) {
        if (node.min_length || node.max_length) {
            charge(work_, 1 + text.size() / g_STRING_BYTES_PER_UNIT);
            const auto length = code_point_count(text);
            if (node.min_length && length < *node.min_length &&
                !report("Number of characters must be at least " + std::to_string(*node.min_length))) {
                return false;
            }
            if (node.max_length && length > *node.max_length &&
                !report("Number of characters must be at most " + std::to_string(*node.max_length))) {
                return false;
            }
        }
        if (node.pattern && !matches(*node.pattern, text) &&
            !report("String does not match the pattern " + node.pattern->pattern())) {
            return false;
        }
        return true;
    }

    /// Checks @p item, element @p index of the array being checked, against @p node.
    bool check_item(const Node& node, const nlohmann::json& item, std::size_t index) {
        const LocationScope scope(*this, Location{{}, index});
        return check(node, item);
    }

    bool unique(const nlohmann::json& array) {
        constexpr std::size_t g_pairwise_size = 8;
        if (array.size() <= g_pairwise_size) {
            for (std::size_t i = 0; i < array.size(); ++i) {
                for (std::size_t j = i + 1; j < array.size(); ++j) {
                    if (json_equal(array[i], array[j], work_)) {
                        return false;
                    }
                }
            }
            return true;
        }
        std::vector<std::pair<std::size_t, std::size_t>> hashed;  // hash, index
        hashed.reserve(array.size());
        for (std::size_t index = 0; index < array.size(); ++index) {
            charge(work_, 1);
            hashed.emplace_back(equality_hash(array[index]), index);
        }
        std::sort(hashed.begin(), hashed.end());
        for (std::size_t i = 0; i < hashed.size(); ++i) {
            // Hashes a peer chose can all collide; each comparison is charged.
            for (std::size_t j = i + 1; j < hashed.size() && hashed[j].first == hashed[i].first; ++j) {
                if (json_equal(array[hashed[i].second], array[hashed[j].second], work_)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool check_array(const Node& node, const nlohmann::json& array) {
        const auto prefix = std::min(node.prefix_items.size(), array.size());
        for (std::size_t index = 0; index < prefix; ++index) {
            if (!check_item(node.prefix_items[index], array[index], index)) {
                return false;
            }
        }
        if (node.items) {
            for (std::size_t index = node.prefix_items.size(); index < array.size(); ++index) {
                if (!check_item(*node.items, array[index], index)) {
                    return false;
                }
            }
        }
        if (node.contains) {
            std::size_t matching = 0;
            for (const auto& item : array) {
                if (passes(*node.contains, item)) {
                    ++matching;
                }
            }
            const auto least = node.min_contains.value_or(1);
            if (matching < least && !report("Array must contain at least " + std::to_string(least) +
                                            " matching items, found " + std::to_string(matching))) {
                return false;
            }
            if (node.max_contains && matching > *node.max_contains &&
                !report("Array must contain at most " + std::to_string(*node.max_contains) +
                        " matching items, found " + std::to_string(matching))) {
                return false;
            }
        }
        if (node.unique_items && !unique(array) && !report("Array items are not unique")) {
            return false;
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

    /// Checks @p member, named @p name, against @p node, at its own location.
    bool check_member(const Node& node, const std::string& name, const nlohmann::json& member) {
        const LocationScope scope(*this, Location{&name, 0});
        return check(node, member);
    }

    bool check_members(const Node& node, const nlohmann::json& object) {
        for (const auto& [name, member] : object.items()) {
            charge(work_, 1 + name.size() / g_NAME_BYTES_PER_UNIT);
            bool evaluated = false;
            const auto found =
                std::lower_bound(node.property_names.begin(), node.property_names.end(), name);
            if (found != node.property_names.end() && *found == name) {
                evaluated = true;
                const auto index = static_cast<std::size_t>(found - node.property_names.begin());
                if (!check_member(node.property_schemas[index], name, member)) {
                    return false;
                }
            }
            for (std::size_t i = 0; i < node.pattern_regexes.size(); ++i) {
                if (matches(node.pattern_regexes[i], name)) {
                    evaluated = true;
                    if (!check_member(node.pattern_schemas[i], name, member)) {
                        return false;
                    }
                }
            }
            if (evaluated || !node.additional_properties) {
                continue;
            }
            if (node.additional_properties->reject_all) {
                const LocationScope scope(*this, Location{&name, 0});
                if (!report("Additional property '" + name + "' is not allowed")) {
                    return false;
                }
            } else if (!check_member(*node.additional_properties, name, member)) {
                return false;
            }
        }
        return true;
    }

    bool check_object(const Node& node, const nlohmann::json& object) {
        for (const auto& name : node.required) {
            if (!object.contains(name) && !report("Required property '" + name + "' is missing")) {
                return false;
            }
        }
        for (std::size_t i = 0; i < node.dependent_required_names.size(); ++i) {
            const auto& present = node.dependent_required_names[i];
            if (!object.contains(present)) {
                continue;
            }
            for (const auto& name : node.dependent_required_lists[i]) {
                charge(work_, 1);
                if (!object.contains(name) &&
                    !report("Property '" + name + "' is required when '" + present + "' is present")) {
                    return false;
                }
            }
        }
        if (!node.property_names.empty() || !node.pattern_regexes.empty() ||
            node.additional_properties) {
            if (!check_members(node, object)) {
                return false;
            }
        }
        if (node.property_names_schema) {
            for (const auto& [name, member] : object.items()) {
                charge(work_, 1 + name.size() / g_NAME_BYTES_PER_UNIT);
                if (!check(*node.property_names_schema, nlohmann::json(name))) {
                    return false;
                }
            }
        }
        for (std::size_t i = 0; i < node.dependent_schema_names.size(); ++i) {
            if (object.contains(node.dependent_schema_names[i]) &&
                !check(node.dependent_schemas[i], object)) {
                return false;
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

    /// Reports @p message, then as many of @p violations as the current sink takes.
    bool report_with(const std::string& message, const std::vector<SchemaViolation>& violations) {
        if (!report(message)) {
            return false;
        }
        for (const auto& violation : violations) {
            if (sink_->violations == nullptr || sink_->violations->size() >= sink_->limit) {
                return false;
            }
            sink_->violations->push_back(violation);
        }
        return sink_->violations == nullptr || sink_->violations->size() < sink_->limit;
    }

    bool check_applicators(const Node& node, const nlohmann::json& value) {
        for (const auto& branch : node.all_of) {
            if (!check(branch, value)) {
                return false;
            }
        }
        const auto buffer_size =
            std::min(mcp::detail::constants::schema::g_MAX_BRANCH_VIOLATIONS, sink_->limit);
        if (!node.any_of.empty()) {
            std::vector<SchemaViolation> first;
            bool matched = false;
            for (std::size_t i = 0; i < node.any_of.size() && !matched; ++i) {
                std::vector<SchemaViolation> violations;
                matched = passes(node.any_of[i], value, &violations, buffer_size);
                if (i == 0) {
                    first = std::move(violations);
                }
            }
            if (!matched && !report_with("Value does not match any of the anyOf schemas", first)) {
                return false;
            }
        }
        if (!node.one_of.empty()) {
            std::vector<SchemaViolation> first;
            std::size_t matched = 0;
            for (std::size_t i = 0; i < node.one_of.size() && matched < 2; ++i) {
                std::vector<SchemaViolation> violations;
                if (passes(node.one_of[i], value, &violations, buffer_size)) {
                    ++matched;
                }
                if (i == 0) {
                    first = std::move(violations);
                }
            }
            if (matched == 0 && !report_with("Value does not match any of the oneOf schemas", first)) {
                return false;
            }
            if (matched > 1 && !report("Value matches more than one of the oneOf schemas")) {
                return false;
            }
        }
        if (node.not_schema && passes(*node.not_schema, value) &&
            !report("Value must not be valid against the not schema")) {
            return false;
        }
        if (node.if_schema) {
            const Node* branch =
                passes(*node.if_schema, value) ? node.then_schema.get() : node.else_schema.get();
            if (branch != nullptr && !check(*branch, value)) {
                return false;
            }
        }
        return true;
    }

    /// The JSON Pointer of the current location, escaped as RFC 6901 requires.
    [[nodiscard]] std::string path() const {
        std::string path;
        for (std::size_t step = 0; step < locations_.size(); ++step) {
            const auto& location = locations_[step];
            path += '/';
            if (location.name == nullptr) {
                path += std::to_string(location.index);
                continue;
            }
            for (const char character : *location.name) {
                if (character == '~') {
                    path += "~0";
                } else if (character == '/') {
                    path += "~1";
                } else {
                    path += character;
                }
            }
        }
        return path;
    }

    bool report(const std::string& message) {
        sink_->valid = false;
        if (sink_->violations == nullptr) {
            return false;
        }
        if (sink_->violations->size() < sink_->limit) {
            sink_->violations->push_back(
                {sanitize_for_diagnostics(path()), sanitize_for_diagnostics(message)});
        }
        return sink_->violations->size() < sink_->limit;
    }

    Sink root_;
    Sink* sink_;
    std::size_t& pattern_steps_;
    std::size_t& work_;
    std::size_t depth_ = 0;
    Locations locations_;  ///< The path to the value being checked; a report builds it.
};

}  // namespace

NativeSchema::NativeSchema(std::shared_ptr<const Node> root) : root_(std::move(root)) {}

std::optional<NativeSchema> NativeSchema::compile(const nlohmann::json& schema) {
    auto root = std::make_unique<Node>();
    if (!compile_into(schema, true, *root)) {
        return std::nullopt;
    }
    return NativeSchema(std::shared_ptr<const Node>(std::move(root)));
}

void NativeSchema::validate(const nlohmann::json& instance, std::size_t max_violations,
                            std::size_t pattern_steps, std::size_t work,
                            ValidationResult& result) const {
    const auto stop = [&result](const char* reason) {
        result.valid = false;
        result.violations.clear();
        result.violations.push_back({"", std::string("validation stopped: ") + reason});
    };
    try {
        Evaluation evaluation(max_violations, pattern_steps, work, result);
        (void)evaluation.check(*root_, instance);
        result.valid = evaluation.valid();
    } catch (const PatternBudgetExceeded&) {
        stop("pattern matching takes too many steps");
    } catch (const WorkBudgetExceeded&) {
        stop("schema evaluation takes too much work");
    } catch (const EvaluationTooDeep&) {
        stop("schema evaluation nests too deeply");
    }
}

}  // namespace mcp::detail
