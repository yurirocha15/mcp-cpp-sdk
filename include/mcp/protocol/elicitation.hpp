#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/base.hpp>
#include <mcp/protocol/schema.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

namespace detail {

inline void put_annotations(nlohmann::json& j, const std::optional<std::string>& title,
                            const std::optional<std::string>& description) {
    if (title) {
        j["title"] = *title;
    }
    if (description) {
        j["description"] = *description;
    }
}

inline void read_annotations(const nlohmann::json& j, std::optional<std::string>& title,
                             std::optional<std::string>& description) {
    if (has_json_value(j, "title")) {
        title = j.at("title").get<std::string>();
    }
    if (has_json_value(j, "description")) {
        description = j.at("description").get<std::string>();
    }
}

/// Reads a non-negative integer keyword such as `minLength` or `maxItems`; anything else throws
/// std::invalid_argument.
inline void read_count(const nlohmann::json& j, const char* key, std::optional<std::uint64_t>& out) {
    if (!has_json_value(j, key)) {
        return;
    }
    const auto& value = j.at(key);
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
    } else if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        out = static_cast<std::uint64_t>(value.get<std::int64_t>());
    } else {
        throw std::invalid_argument(std::string("The elicitation schema keyword `") + key +
                                    "` must be a non-negative integer");
    }
}

/// The int64 for an integer field's bound. A minimum rounds up and a maximum rounds down, which
/// keeps the set of accepted integers unchanged; a bound beyond the int64 range on the open side
/// clamps, and one that no int64 can satisfy throws std::invalid_argument.
inline std::int64_t integer_bound(double value, bool is_minimum) {
    constexpr double k_limit = 9223372036854775808.0;  // 2^63
    const double rounded = is_minimum ? std::ceil(value) : std::floor(value);
    if (std::isnan(rounded)) {
        throw std::invalid_argument("An integer elicitation field's bound is not a number");
    }
    if (rounded >= k_limit) {
        if (is_minimum) {
            throw std::invalid_argument(
                "An integer elicitation field's minimum exceeds the int64 range");
        }
        return std::numeric_limits<std::int64_t>::max();
    }
    if (rounded < -k_limit) {
        if (!is_minimum) {
            throw std::invalid_argument(
                "An integer elicitation field's maximum is below the int64 range");
        }
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(rounded);
}

/// The int64 for an integer field's default; a value that is not an integer in the int64 range
/// throws std::invalid_argument.
inline std::int64_t integer_default(double value) {
    constexpr double k_limit = 9223372036854775808.0;
    if (!(value >= -k_limit && value < k_limit) || std::trunc(value) != value) {
        throw std::invalid_argument("An integer elicitation field's default is not an int64 integer");
    }
    return static_cast<std::int64_t>(value);
}

template <typename T>
void read_optional(const nlohmann::json& j, const char* key, std::optional<T>& out) {
    if (has_json_value(j, key)) {
        out = j.at(key).get<T>();
    }
}

}  // namespace detail

/**
 * @brief One option of a titled enum: the value that goes on the wire and the label shown for it.
 */
struct EnumOption {
    std::string value;  ///< The `const` of the option.
    std::string title;  ///< Display label.
};

inline void to_json(nlohmann::json& j, const EnumOption& option) {
    j = nlohmann::json{{"const", option.value}, {"title", option.title}};
}
inline void from_json(const nlohmann::json& j, EnumOption& option) {
    j.at("const").get_to(option.value);
    j.at("title").get_to(option.title);
}

/**
 * @brief A string field: `{"type": "string"}` with optional length limits and format.
 */
struct StringSchema {
    /// `title`: human-readable name of the field.
    std::optional<std::string> title;
    /// `description`: explanation of the field shown to the user.
    std::optional<std::string> description;
    /// `minLength`: fewest characters the value may have.
    std::optional<std::uint64_t> minLength;
    /// `maxLength`: most characters the value may have.
    std::optional<std::uint64_t> maxLength;
    /// One of `email`, `uri`, `date` or `date-time` in the specification; not checked here.
    std::optional<std::string> format;
    std::optional<std::string> defaultValue;  ///< `default`.
};

inline void to_json(nlohmann::json& j, const StringSchema& s) {
    j = nlohmann::json{{"type", "string"}};
    detail::put_annotations(j, s.title, s.description);
    if (s.minLength) {
        j["minLength"] = *s.minLength;
    }
    if (s.maxLength) {
        j["maxLength"] = *s.maxLength;
    }
    if (s.format) {
        j["format"] = *s.format;
    }
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}
inline void from_json(const nlohmann::json& j, StringSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_count(j, "minLength", s.minLength);
    detail::read_count(j, "maxLength", s.maxLength);
    detail::read_optional(j, "format", s.format);
    detail::read_optional(j, "default", s.defaultValue);
}

/**
 * @brief A numeric field: `{"type": "number"}`, or `{"type": "integer"}` when #integer is set.
 */
struct NumberSchema {
    bool integer = false;  ///< `integer` rather than `number`.
    /// `title`: human-readable name of the field.
    std::optional<std::string> title;
    /// `description`: explanation of the field shown to the user.
    std::optional<std::string> description;
    /// `minimum`: smallest value the field accepts, inclusive.
    std::optional<double> minimum;
    /// `maximum`: largest value the field accepts, inclusive.
    std::optional<double> maximum;
    std::optional<double> defaultValue;  ///< `default`.
};

inline void to_json(nlohmann::json& j, const NumberSchema& s) {
    j = nlohmann::json{{"type", s.integer ? "integer" : "number"}};
    detail::put_annotations(j, s.title, s.description);
    // An integer field's bounds and default are written as integers; a value no int64 can hold
    // throws std::invalid_argument.
    if (s.minimum) {
        j["minimum"] = s.integer ? nlohmann::json(detail::integer_bound(*s.minimum, true))
                                 : nlohmann::json(*s.minimum);
    }
    if (s.maximum) {
        j["maximum"] = s.integer ? nlohmann::json(detail::integer_bound(*s.maximum, false))
                                 : nlohmann::json(*s.maximum);
    }
    if (s.defaultValue) {
        j["default"] = s.integer ? nlohmann::json(detail::integer_default(*s.defaultValue))
                                 : nlohmann::json(*s.defaultValue);
    }
}
inline void from_json(const nlohmann::json& j, NumberSchema& s) {
    std::string type;
    j.at("type").get_to(type);
    s.integer = type == "integer";
    detail::read_annotations(j, s.title, s.description);
    detail::read_optional(j, "minimum", s.minimum);
    detail::read_optional(j, "maximum", s.maximum);
    detail::read_optional(j, "default", s.defaultValue);
}

/**
 * @brief A boolean field.
 */
struct BooleanSchema {
    /// `title`: human-readable name of the field.
    std::optional<std::string> title;
    /// `description`: explanation of the field shown to the user.
    std::optional<std::string> description;
    std::optional<bool> defaultValue;  ///< `default`.
};

inline void to_json(nlohmann::json& j, const BooleanSchema& s) {
    j = nlohmann::json{{"type", "boolean"}};
    detail::put_annotations(j, s.title, s.description);
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}
inline void from_json(const nlohmann::json& j, BooleanSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_optional(j, "default", s.defaultValue);
}

/**
 * @brief A single-select enum without display labels: `{"type": "string", "enum": [...]}`.
 *
 * @details #enumNames is the pre-2025-11-25 way to label the options: a list that parallels
 * #enumValues. It is read and written as given. Prefer TitledEnumSchema for new schemas; to_titled()
 * converts.
 */
struct EnumSchema {
    /// `title`: human-readable name of the field.
    std::optional<std::string> title;
    /// `description`: explanation of the field shown to the user.
    std::optional<std::string> description;
    std::vector<std::string> enumValues;                ///< `enum`.
    std::optional<std::vector<std::string>> enumNames;  ///< Legacy labels, one per value.
    std::optional<std::string> defaultValue;            ///< `default`.
};

inline void to_json(nlohmann::json& j, const EnumSchema& s) {
    j = nlohmann::json{{"type", "string"}};
    detail::put_annotations(j, s.title, s.description);
    j["enum"] = s.enumValues;
    if (s.enumNames) {
        j["enumNames"] = *s.enumNames;
    }
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}
inline void from_json(const nlohmann::json& j, EnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    j.at("enum").get_to(s.enumValues);
    detail::read_optional(j, "enumNames", s.enumNames);
    detail::read_optional(j, "default", s.defaultValue);
}

/**
 * @brief A single-select enum with a label for every option:
 * `{"type": "string", "oneOf": [{"const", "title"}, ...]}`.
 */
struct TitledEnumSchema {
    /// `title`: human-readable name of the field.
    std::optional<std::string> title;
    /// `description`: explanation of the field shown to the user.
    std::optional<std::string> description;
    std::vector<EnumOption> options;          ///< `oneOf`.
    std::optional<std::string> defaultValue;  ///< `default`.
};

inline void to_json(nlohmann::json& j, const TitledEnumSchema& s) {
    j = nlohmann::json{{"type", "string"}};
    detail::put_annotations(j, s.title, s.description);
    j["oneOf"] = s.options;
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}
inline void from_json(const nlohmann::json& j, TitledEnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    j.at("oneOf").get_to(s.options);
    detail::read_optional(j, "default", s.defaultValue);
}

/**
 * @brief The TitledEnumSchema for @p schema: its #EnumSchema::enumNames become the option titles,
 * and a value with no name is its own title.
 */
inline TitledEnumSchema to_titled(const EnumSchema& schema) {
    TitledEnumSchema titled;
    titled.title = schema.title;
    titled.description = schema.description;
    titled.defaultValue = schema.defaultValue;
    for (std::size_t i = 0; i < schema.enumValues.size(); ++i) {
        const bool named = schema.enumNames && i < schema.enumNames->size();
        titled.options.push_back(
            {schema.enumValues[i], named ? (*schema.enumNames)[i] : schema.enumValues[i]});
    }
    return titled;
}

/**
 * @brief A multi-select enum without display labels:
 * `{"type": "array", "items": {"type": "string", "enum": [...]}}`.
 */
struct MultiSelectEnumSchema {
    /// `title`: human-readable name of the field.
    std::optional<std::string> title;
    /// `description`: explanation of the field shown to the user.
    std::optional<std::string> description;
    /// `minItems`: fewest options the user must select.
    std::optional<std::uint64_t> minItems;
    /// `maxItems`: most options the user may select.
    std::optional<std::uint64_t> maxItems;
    std::vector<std::string> enumValues;                   ///< `items.enum`.
    std::optional<std::vector<std::string>> defaultValue;  ///< `default`.
};

inline void to_json(nlohmann::json& j, const MultiSelectEnumSchema& s) {
    j = nlohmann::json{{"type", "array"}};
    detail::put_annotations(j, s.title, s.description);
    if (s.minItems) {
        j["minItems"] = *s.minItems;
    }
    if (s.maxItems) {
        j["maxItems"] = *s.maxItems;
    }
    j["items"] = nlohmann::json{{"type", "string"}, {"enum", s.enumValues}};
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}
inline void from_json(const nlohmann::json& j, MultiSelectEnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_count(j, "minItems", s.minItems);
    detail::read_count(j, "maxItems", s.maxItems);
    j.at("items").at("enum").get_to(s.enumValues);
    detail::read_optional(j, "default", s.defaultValue);
}

/**
 * @brief A multi-select enum with a label for every option:
 * `{"type": "array", "items": {"anyOf": [{"const", "title"}, ...]}}`.
 */
struct TitledMultiSelectEnumSchema {
    /// `title`: human-readable name of the field.
    std::optional<std::string> title;
    /// `description`: explanation of the field shown to the user.
    std::optional<std::string> description;
    /// `minItems`: fewest options the user must select.
    std::optional<std::uint64_t> minItems;
    /// `maxItems`: most options the user may select.
    std::optional<std::uint64_t> maxItems;
    std::vector<EnumOption> options;                       ///< `items.anyOf`.
    std::optional<std::vector<std::string>> defaultValue;  ///< `default`.
};

inline void to_json(nlohmann::json& j, const TitledMultiSelectEnumSchema& s) {
    j = nlohmann::json{{"type", "array"}};
    detail::put_annotations(j, s.title, s.description);
    if (s.minItems) {
        j["minItems"] = *s.minItems;
    }
    if (s.maxItems) {
        j["maxItems"] = *s.maxItems;
    }
    j["items"] = nlohmann::json{{"anyOf", s.options}};
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}
inline void from_json(const nlohmann::json& j, TitledMultiSelectEnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_count(j, "minItems", s.minItems);
    detail::read_count(j, "maxItems", s.maxItems);
    j.at("items").at("anyOf").get_to(s.options);
    detail::read_optional(j, "default", s.defaultValue);
}

/**
 * @brief Any field a form-mode elicitation may ask for.
 */
using PrimitiveSchemaDefinition =
    std::variant<StringSchema, NumberSchema, BooleanSchema, EnumSchema, TitledEnumSchema,
                 MultiSelectEnumSchema, TitledMultiSelectEnumSchema>;

inline void to_json(nlohmann::json& j, const PrimitiveSchemaDefinition& def) {
    std::visit([&j](const auto& schema) { j = schema; }, def);
}

/// Picks the alternative from `type` and the keys that tell the enum forms apart. A schema that is
/// none of the specification's primitive types throws std::invalid_argument.
inline void from_json(const nlohmann::json& j, PrimitiveSchemaDefinition& def) {
    if (!j.is_object() || !detail::has_json_value(j, "type") || !j.at("type").is_string()) {
        throw std::invalid_argument("An elicitation field schema needs a string `type`");
    }
    const auto type = j.at("type").get<std::string>();
    if (type == "string") {
        if (j.contains("oneOf")) {
            def = j.get<TitledEnumSchema>();
        } else if (j.contains("enum")) {
            def = j.get<EnumSchema>();
        } else {
            def = j.get<StringSchema>();
        }
    } else if (type == "number" || type == "integer") {
        def = j.get<NumberSchema>();
    } else if (type == "boolean") {
        def = j.get<BooleanSchema>();
    } else if (type == "array") {
        const auto items = j.find("items");
        if (items == j.end() || !items->is_object()) {
            throw std::invalid_argument("An elicitation array field needs an `items` object");
        }
        if (items->contains("anyOf")) {
            def = j.get<TitledMultiSelectEnumSchema>();
        } else if (items->contains("enum")) {
            def = j.get<MultiSelectEnumSchema>();
        } else {
            throw std::invalid_argument("An elicitation array field must list enum options in `items`");
        }
    } else {
        throw std::invalid_argument("Unsupported elicitation field type: " + type);
    }
}

/**
 * @brief One accepted form value, typed by its field: a string for string and single-select
 * fields, an integer for `integer`, a double for `number`, a bool, or the selected strings of a
 * multi-select field.
 */
using ElicitedValue = std::variant<std::string, std::int64_t, double, bool, std::vector<std::string>>;

class ElicitationSchema;
class ElicitedContent;
[[nodiscard]] MCP_API ElicitedContent decode_elicitation_content(const ElicitationSchema& schema,
                                                                 const nlohmann::json& content);

/**
 * @brief The values of an accepted form, decoded by ElicitationSchema::decode().
 */
class ElicitedContent {
   public:
    /// Whether the user supplied a value for @p name.
    [[nodiscard]] bool contains(const std::string& name) const { return values_.contains(name); }

    /// All values by field name.
    [[nodiscard]] const std::map<std::string, ElicitedValue>& values() const noexcept {
        return values_;
    }

    /**
     * @brief The value of @p name as a @p T, or nothing when the field is absent or holds another
     * type. @p T is one of the ElicitedValue alternatives. An `integer` field also reads as double.
     */
    template <typename T>
    [[nodiscard]] std::optional<T> get(const std::string& name) const {
        const auto found = values_.find(name);
        if (found == values_.end()) {
            return std::nullopt;
        }
        if (const auto* value = std::get_if<T>(&found->second)) {
            return *value;
        }
        if constexpr (std::is_same_v<T, double>) {
            if (const auto* integer = std::get_if<std::int64_t>(&found->second)) {
                return static_cast<double>(*integer);
            }
        }
        return std::nullopt;
    }

   private:
    friend ElicitedContent decode_elicitation_content(const ElicitationSchema& schema,
                                                      const nlohmann::json& content);
    std::map<std::string, ElicitedValue> values_;
};

/**
 * @brief The `requestedSchema` of a form-mode elicitation, built from typed fields.
 *
 * @details Converts to the flat object schema the specification defines: `type: "object"` with one
 * primitive property per field. JSON object order carries no meaning, so the order the fields
 * were added in is not preserved in the JSON. Assign it to ElicitRequestFormParams::requestedSchema,
 * which takes JSON, or pass it to nlohmann::json.
 *
 * @code
 * ElicitationSchema form;
 * form.add("name", StringSchema{.title = "Name", .minLength = 1}, true)
 *     .add("color", TitledEnumSchema{.options = {{"#f00", "Red"}, {"#0f0", "Green"}}});
 * ElicitRequestFormParams params;
 * params.message = "Tell us about you";
 * params.requestedSchema = form;
 * @endcode
 */
class ElicitationSchema {
   public:
    /**
     * @brief Add a field.
     * @param name The property name.
     * @param schema The field's schema.
     * @param required Whether the user must supply it.
     * @return This schema, for chaining.
     * @throws std::invalid_argument When a field named @p name exists.
     */
    ElicitationSchema& add(std::string name, PrimitiveSchemaDefinition schema, bool required = false) {
        for (const auto& field : fields_) {
            if (field.first == name) {
                throw std::invalid_argument("Duplicate elicitation field: " + name);
            }
        }
        if (required) {
            required_.push_back(name);
        }
        fields_.emplace_back(std::move(name), std::move(schema));
        return *this;
    }

    /// The fields in the order they were added. The JSON form (`properties`) is an object whose
    /// members are sorted by name, so the order is not preserved on the wire or when parsing.
    [[nodiscard]] const std::vector<std::pair<std::string, PrimitiveSchemaDefinition>>& fields()
        const noexcept {
        return fields_;
    }

    /// The names of the required fields.
    [[nodiscard]] const std::vector<std::string>& required() const noexcept { return required_; }

    /// The `$schema` the object declares; unset leaves it out.
    std::optional<std::string> dialect;

    /**
     * @brief Check @p content against this schema and decode it into typed values.
     *
     * @details A form answer is valid when the schema validator accepts it: every required field is
     * present, no field is undeclared, and each value has its field's type, bounds and options.
     * `format` is annotation only and is not checked.
     *
     * @param content The `content` of an accepted ElicitResult.
     * @return The values by field name, in the field's type.
     * @throws std::invalid_argument When @p content does not conform; the message lists the
     *         violations.
     */
    [[nodiscard]] ElicitedContent decode(const nlohmann::json& content) const;

    /// Parses a requested schema. Throws std::invalid_argument for one that is not a flat object
    /// of primitive fields.
    static ElicitationSchema from_json_schema(const nlohmann::json& requested) {
        if (!requested.is_object() || requested.value("type", "") != "object") {
            throw std::invalid_argument("An elicitation schema must be an object schema");
        }
        ElicitationSchema schema;
        if (detail::has_json_value(requested, "$schema")) {
            schema.dialect = requested.at("$schema").get<std::string>();
        }
        if (const auto properties = requested.find("properties"); properties != requested.end()) {
            for (const auto& entry : properties->items()) {
                schema.fields_.emplace_back(entry.key(),
                                            entry.value().get<PrimitiveSchemaDefinition>());
            }
        }
        if (detail::has_json_value(requested, "required")) {
            requested.at("required").get_to(schema.required_);
        }
        return schema;
    }

   private:
    std::vector<std::pair<std::string, PrimitiveSchemaDefinition>> fields_;
    std::vector<std::string> required_;
};

inline void to_json(nlohmann::json& j, const ElicitationSchema& schema) {
    j = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    if (schema.dialect) {
        j["$schema"] = *schema.dialect;
    }
    for (const auto& field : schema.fields()) {
        j["properties"][field.first] = field.second;
    }
    if (!schema.required().empty()) {
        j["required"] = schema.required();
    }
}
inline void from_json(const nlohmann::json& j, ElicitationSchema& schema) {
    schema = ElicitationSchema::from_json_schema(j);
}

inline ElicitedContent ElicitationSchema::decode(const nlohmann::json& content) const {
    return decode_elicitation_content(*this, content);
}

/**
 * @brief Check the `content` of an accepted form answer against the request's `requestedSchema`.
 *
 * @param requested_schema The `requestedSchema` of the form request.
 * @param content The answer's `content`; an absent one is checked as an empty object.
 * @return The validator's result; `valid` is true when the answer conforms.
 * @throws SchemaError When the schema cannot be compiled, so nothing can be checked against it.
 */
[[nodiscard]] MCP_API ValidationResult validate_elicitation_content(
    const nlohmann::json& requested_schema, const std::optional<nlohmann::json>& content);

/// The violations of @p result as one line, as "path: message; path: message".
[[nodiscard]] MCP_API std::string describe_violations(const ValidationResult& result);

/**
 * @brief Parameters for a URL-mode elicitation request.
 */
struct ElicitRequestURLParams {
    std::string mode = "url";  ///< The elicitation mode (always "url").
    std::string message;       ///< The message to present to the user.
    std::string url;           ///< The URL that the user should navigate to.
    /// Unique ID for this elicitation. Required on the wire before 2026-07-28 (the legacy
    /// reverse-RPC path generates one when unset); a 2026-07-28 request never carries it.
    std::optional<std::string> elicitationId;
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitRequestURLParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode}, {"message", params.message}, {"url", params.url}};
    if (params.elicitationId) {
        json_obj["elicitationId"] = *params.elicitationId;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestURLParams& params) {
    json_obj.at("mode").get_to(params.mode);
    json_obj.at("message").get_to(params.message);
    json_obj.at("url").get_to(params.url);
    if (detail::has_json_value(json_obj, "elicitationId")) {
        params.elicitationId = json_obj.at("elicitationId").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for a form-mode elicitation request.
 */
struct ElicitRequestFormParams {
    std::string mode = "form";           ///< The elicitation mode (always "form").
    std::string message;                 ///< The message describing what info is requested.
    nlohmann::json requestedSchema;      ///< Restricted JSON Schema for the form.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitRequestFormParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode},
                              {"message", params.message},
                              {"requestedSchema", params.requestedSchema}};
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestFormParams& params) {
    if (detail::has_json_value(json_obj, "mode")) {
        json_obj.at("mode").get_to(params.mode);
    }
    json_obj.at("message").get_to(params.message);
    json_obj.at("requestedSchema").get_to(params.requestedSchema);
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for an elicitation request — either URL or form mode.
 */
using ElicitRequestParams = std::variant<ElicitRequestURLParams, ElicitRequestFormParams>;

inline void to_json(nlohmann::json& json_obj, const ElicitRequestParams& params) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, params);
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestParams& params) {
    // A form request may leave its mode out.
    const auto mode = detail::has_json_value(json_obj, "mode") ? json_obj.at("mode").get<std::string>()
                                                               : std::string("form");
    if (mode == "url") {
        params = json_obj.get<ElicitRequestURLParams>();
    } else if (mode == "form") {
        params = json_obj.get<ElicitRequestFormParams>();
    } else {
        throw std::invalid_argument("Unknown elicitation mode: " + mode);
    }
}

/**
 * @brief A request from the server to elicit additional information from the user.
 */
struct ElicitRequest {
    std::string method = "elicitation/create";  ///< The method name.
    ElicitRequestParams params;                 ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ElicitRequest, method, params)

/**
 * @brief The user action in response to an elicitation.
 */
enum class ElicitAction : std::uint8_t {
    eAccept,   ///< User submitted the form/confirmed the action.
    eDecline,  ///< User explicitly declined the action.
    eCancel    ///< User dismissed without making an explicit choice.
};

inline void to_json(nlohmann::json& json_obj, const ElicitAction& action) {
    switch (action) {
        case ElicitAction::eAccept:
            json_obj = "accept";
            return;
        case ElicitAction::eDecline:
            json_obj = "decline";
            return;
        case ElicitAction::eCancel:
            json_obj = "cancel";
            return;
    }
    throw std::invalid_argument("Unknown elicitation action");
}

/// Not NLOHMANN_JSON_SERIALIZE_ENUM, which gives a value it does not know the first entry: that
/// would turn a reply nobody understood into a person's consent.
inline void from_json(const nlohmann::json& json_obj, ElicitAction& action) {
    const auto text = json_obj.get<std::string>();
    if (text == "accept") {
        action = ElicitAction::eAccept;
    } else if (text == "decline") {
        action = ElicitAction::eDecline;
    } else if (text == "cancel") {
        action = ElicitAction::eCancel;
    } else {
        throw std::invalid_argument("Unknown elicitation action");
    }
}

/**
 * @brief The result returned by the client for an elicitation/create request.
 */
struct ElicitResult {
    ElicitAction action;                    ///< The user action.
    std::optional<nlohmann::json> content;  ///< Submitted form data (only when action is accept).
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitResult& result) {
    json_obj = nlohmann::json{{"action", result.action}};
    if (result.content) {
        json_obj["content"] = *result.content;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitResult& result) {
    json_obj.at("action").get_to(result.action);
    if (json_obj.contains("content")) {
        result.content = json_obj.at("content").get<nlohmann::json>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp
