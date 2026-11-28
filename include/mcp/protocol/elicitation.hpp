#pragma once

#include <mcp/core/constants.hpp>
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

/**
 * @brief One option of a titled enum: the value that goes on the wire and the label shown for it.
 */
struct EnumOption {
    std::string value;  ///< The `const` of the option.
    std::string title;  ///< Display label.
};

MCP_API void to_json(nlohmann::json& j, const EnumOption& option);
MCP_API void from_json(const nlohmann::json& j, EnumOption& option);

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

MCP_API void to_json(nlohmann::json& j, const StringSchema& s);
MCP_API void from_json(const nlohmann::json& j, StringSchema& s);

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

MCP_API void to_json(nlohmann::json& j, const NumberSchema& s);
MCP_API void from_json(const nlohmann::json& j, NumberSchema& s);

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

MCP_API void to_json(nlohmann::json& j, const BooleanSchema& s);
MCP_API void from_json(const nlohmann::json& j, BooleanSchema& s);

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

MCP_API void to_json(nlohmann::json& j, const EnumSchema& s);
MCP_API void from_json(const nlohmann::json& j, EnumSchema& s);

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

MCP_API void to_json(nlohmann::json& j, const TitledEnumSchema& s);
MCP_API void from_json(const nlohmann::json& j, TitledEnumSchema& s);

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
        titled.options.push_back({
            .value = schema.enumValues[i],
            .title = named ? (*schema.enumNames)[i] : schema.enumValues[i],
        });
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

MCP_API void to_json(nlohmann::json& j, const MultiSelectEnumSchema& s);
MCP_API void from_json(const nlohmann::json& j, MultiSelectEnumSchema& s);

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

MCP_API void to_json(nlohmann::json& j, const TitledMultiSelectEnumSchema& s);
MCP_API void from_json(const nlohmann::json& j, TitledMultiSelectEnumSchema& s);

/**
 * @brief Any field a form-mode elicitation may ask for.
 */
using PrimitiveSchemaDefinition =
    std::variant<StringSchema, NumberSchema, BooleanSchema, EnumSchema, TitledEnumSchema,
                 MultiSelectEnumSchema, TitledMultiSelectEnumSchema>;

MCP_API void to_json(nlohmann::json& j, const PrimitiveSchemaDefinition& def);

/// Picks the alternative from `type` and the keys that tell the enum forms apart. A schema that is
/// none of the specification's primitive types throws std::invalid_argument.
MCP_API void from_json(const nlohmann::json& j, PrimitiveSchemaDefinition& def);

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
    friend MCP_API ElicitedContent decode_elicitation_content(const ElicitationSchema& schema,
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
    static MCP_API ElicitationSchema from_json_schema(const nlohmann::json& requested);

   private:
    std::vector<std::pair<std::string, PrimitiveSchemaDefinition>> fields_;
    std::vector<std::string> required_;
};

MCP_API void to_json(nlohmann::json& j, const ElicitationSchema& schema);
MCP_API void from_json(const nlohmann::json& j, ElicitationSchema& schema);

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

MCP_API void to_json(nlohmann::json& json_obj, const ElicitRequestURLParams& params);

MCP_API void from_json(const nlohmann::json& json_obj, ElicitRequestURLParams& params);

/**
 * @brief Parameters for a form-mode elicitation request.
 */
struct ElicitRequestFormParams {
    std::string mode = "form";           ///< The elicitation mode (always "form").
    std::string message;                 ///< The message describing what info is requested.
    nlohmann::json requestedSchema;      ///< Restricted JSON Schema for the form.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

MCP_API void to_json(nlohmann::json& json_obj, const ElicitRequestFormParams& params);

MCP_API void from_json(const nlohmann::json& json_obj, ElicitRequestFormParams& params);

/**
 * @brief Parameters for an elicitation request: either URL or form mode.
 */
using ElicitRequestParams = std::variant<ElicitRequestURLParams, ElicitRequestFormParams>;

MCP_API void to_json(nlohmann::json& json_obj, const ElicitRequestParams& params);

MCP_API void from_json(const nlohmann::json& json_obj, ElicitRequestParams& params);

/**
 * @brief A request from the server to elicit additional information from the user.
 */
struct ElicitRequest {
    std::string method{mcp::constants::methods::g_ELICITATION_CREATE};  ///< The method name.
    ElicitRequestParams params;                                         ///< The request parameters.
};

MCP_API void to_json(nlohmann::json& json_obj, const ElicitRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, ElicitRequest& value);

/**
 * @brief The user action in response to an elicitation.
 */
enum class ElicitAction : std::uint8_t {
    eAccept,   ///< User submitted the form/confirmed the action.
    eDecline,  ///< User explicitly declined the action.
    eCancel,   ///< User dismissed without making an explicit choice.
};

MCP_API void to_json(nlohmann::json& json_obj, const ElicitAction& action);

/// Not NLOHMANN_JSON_SERIALIZE_ENUM, which gives a value it does not know the first entry: that
/// would turn a reply nobody understood into a person's consent.
MCP_API void from_json(const nlohmann::json& json_obj, ElicitAction& action);

/**
 * @brief The result returned by the client for an elicitation/create request.
 */
struct ElicitResult {
    ElicitAction action;                    ///< The user action.
    std::optional<nlohmann::json> content;  ///< Submitted form data (only when action is accept).
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

MCP_API void to_json(nlohmann::json& json_obj, const ElicitResult& result);

MCP_API void from_json(const nlohmann::json& json_obj, ElicitResult& result);

}  // namespace mcp
