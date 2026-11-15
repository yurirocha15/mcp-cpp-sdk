#include <mcp/protocol/elicitation.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace mcp {

ValidationResult validate_elicitation_content(const nlohmann::json& requested_schema,
                                              const std::optional<nlohmann::json>& content) {
    const auto compiled = CompiledSchema::compile(requested_schema);
    return compiled.validate(content ? *content : nlohmann::json::object());
}

std::string describe_violations(const ValidationResult& result) {
    std::string text;
    for (const auto& violation : result.violations) {
        if (!text.empty()) {
            text += "; ";
        }
        if (!violation.instance_path.empty()) {
            text += violation.instance_path + ": ";
        }
        text += violation.message;
    }
    return text;
}

namespace {

ElicitedValue decode_value(const PrimitiveSchemaDefinition& field, const nlohmann::json& value) {
    return std::visit(
        [&value](const auto& schema) -> ElicitedValue {
            using Schema = std::decay_t<decltype(schema)>;
            if constexpr (std::is_same_v<Schema, NumberSchema>) {
                if (schema.integer) {
                    return value.get<std::int64_t>();
                }
                return value.get<double>();
            } else if constexpr (std::is_same_v<Schema, BooleanSchema>) {
                return value.get<bool>();
            } else if constexpr (std::is_same_v<Schema, MultiSelectEnumSchema> ||
                                 std::is_same_v<Schema, TitledMultiSelectEnumSchema>) {
                return value.get<std::vector<std::string>>();
            } else {
                return value.get<std::string>();
            }
        },
        field);
}

}  // namespace

ElicitedContent decode_elicitation_content(const ElicitationSchema& schema,
                                           const nlohmann::json& content) {
    nlohmann::json requested = schema;
    // The form schema is flat; nothing outside its fields may come back.
    requested["additionalProperties"] = false;
    // In place: with nlohmann_json before 3.12 a const json converts to std::optional<json> two
    // ways.
    const auto result =
        validate_elicitation_content(requested, std::optional<nlohmann::json>(std::in_place, content));
    if (!result.valid) {
        throw std::invalid_argument("The elicitation content does not match its schema: " +
                                    describe_violations(result));
    }
    ElicitedContent decoded;
    for (const auto& field : schema.fields()) {
        const auto found = content.find(field.first);
        if (found != content.end()) {
            decoded.values_.emplace(field.first, decode_value(field.second, *found));
        }
    }
    return decoded;
}

namespace detail {
namespace {

void put_annotations(nlohmann::json& j, const std::optional<std::string>& title,
                     const std::optional<std::string>& description) {
    if (title) {
        j["title"] = *title;
    }
    if (description) {
        j["description"] = *description;
    }
}

void read_annotations(const nlohmann::json& j, std::optional<std::string>& title,
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
void read_count(const nlohmann::json& j, const char* key, std::optional<std::uint64_t>& out) {
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
std::int64_t integer_bound(double value, bool is_minimum) {
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
std::int64_t integer_default(double value) {
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

}  // namespace
}  // namespace detail

void to_json(nlohmann::json& j, const EnumOption& option) {
    j = nlohmann::json{{"const", option.value}, {"title", option.title}};
}

void from_json(const nlohmann::json& j, EnumOption& option) {
    j.at("const").get_to(option.value);
    j.at("title").get_to(option.title);
}

void to_json(nlohmann::json& j, const StringSchema& s) {
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

void from_json(const nlohmann::json& j, StringSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_count(j, "minLength", s.minLength);
    detail::read_count(j, "maxLength", s.maxLength);
    detail::read_optional(j, "format", s.format);
    detail::read_optional(j, "default", s.defaultValue);
}

void to_json(nlohmann::json& j, const NumberSchema& s) {
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

void from_json(const nlohmann::json& j, NumberSchema& s) {
    std::string type;
    j.at("type").get_to(type);
    s.integer = type == "integer";
    detail::read_annotations(j, s.title, s.description);
    detail::read_optional(j, "minimum", s.minimum);
    detail::read_optional(j, "maximum", s.maximum);
    detail::read_optional(j, "default", s.defaultValue);
}

void to_json(nlohmann::json& j, const BooleanSchema& s) {
    j = nlohmann::json{{"type", "boolean"}};
    detail::put_annotations(j, s.title, s.description);
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}

void from_json(const nlohmann::json& j, BooleanSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_optional(j, "default", s.defaultValue);
}

void to_json(nlohmann::json& j, const EnumSchema& s) {
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

void from_json(const nlohmann::json& j, EnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    j.at("enum").get_to(s.enumValues);
    detail::read_optional(j, "enumNames", s.enumNames);
    detail::read_optional(j, "default", s.defaultValue);
}

void to_json(nlohmann::json& j, const TitledEnumSchema& s) {
    j = nlohmann::json{{"type", "string"}};
    detail::put_annotations(j, s.title, s.description);
    j["oneOf"] = s.options;
    if (s.defaultValue) {
        j["default"] = *s.defaultValue;
    }
}

void from_json(const nlohmann::json& j, TitledEnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    j.at("oneOf").get_to(s.options);
    detail::read_optional(j, "default", s.defaultValue);
}

void to_json(nlohmann::json& j, const MultiSelectEnumSchema& s) {
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

void from_json(const nlohmann::json& j, MultiSelectEnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_count(j, "minItems", s.minItems);
    detail::read_count(j, "maxItems", s.maxItems);
    j.at("items").at("enum").get_to(s.enumValues);
    detail::read_optional(j, "default", s.defaultValue);
}

void to_json(nlohmann::json& j, const TitledMultiSelectEnumSchema& s) {
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

void from_json(const nlohmann::json& j, TitledMultiSelectEnumSchema& s) {
    detail::read_annotations(j, s.title, s.description);
    detail::read_count(j, "minItems", s.minItems);
    detail::read_count(j, "maxItems", s.maxItems);
    j.at("items").at("anyOf").get_to(s.options);
    detail::read_optional(j, "default", s.defaultValue);
}

void to_json(nlohmann::json& j, const PrimitiveSchemaDefinition& def) {
    std::visit([&j](const auto& schema) { j = schema; }, def);
}

void from_json(const nlohmann::json& j, PrimitiveSchemaDefinition& def) {
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

void to_json(nlohmann::json& j, const ElicitationSchema& schema) {
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

void from_json(const nlohmann::json& j, ElicitationSchema& schema) {
    schema = ElicitationSchema::from_json_schema(j);
}

void to_json(nlohmann::json& json_obj, const ElicitRequestURLParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode}, {"message", params.message}, {"url", params.url}};
    if (params.elicitationId) {
        json_obj["elicitationId"] = *params.elicitationId;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

void from_json(const nlohmann::json& json_obj, ElicitRequestURLParams& params) {
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

void to_json(nlohmann::json& json_obj, const ElicitRequestFormParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode},
                              {"message", params.message},
                              {"requestedSchema", params.requestedSchema}};
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

void from_json(const nlohmann::json& json_obj, ElicitRequestFormParams& params) {
    if (detail::has_json_value(json_obj, "mode")) {
        json_obj.at("mode").get_to(params.mode);
    }
    json_obj.at("message").get_to(params.message);
    json_obj.at("requestedSchema").get_to(params.requestedSchema);
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const ElicitRequestParams& params) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, params);
}

void from_json(const nlohmann::json& json_obj, ElicitRequestParams& params) {
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

void to_json(nlohmann::json& json_obj, const ElicitRequest& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, ElicitRequest& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& json_obj, const ElicitAction& action) {
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

void from_json(const nlohmann::json& json_obj, ElicitAction& action) {
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

void to_json(nlohmann::json& json_obj, const ElicitResult& result) {
    json_obj = nlohmann::json{{"action", result.action}};
    if (result.content) {
        json_obj["content"] = *result.content;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

void from_json(const nlohmann::json& json_obj, ElicitResult& result) {
    json_obj.at("action").get_to(result.action);
    if (json_obj.contains("content")) {
        result.content = json_obj.at("content").get<nlohmann::json>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

ElicitationSchema ElicitationSchema::from_json_schema(const nlohmann::json& requested) {
    if (!requested.is_object() || requested.value("type", "") != "object") {
        throw std::invalid_argument("An elicitation schema must be an object schema");
    }
    ElicitationSchema schema;
    if (detail::has_json_value(requested, "$schema")) {
        schema.dialect = requested.at("$schema").get<std::string>();
    }
    if (const auto properties = requested.find("properties"); properties != requested.end()) {
        for (const auto& entry : properties->items()) {
            schema.fields_.emplace_back(entry.key(), entry.value().get<PrimitiveSchemaDefinition>());
        }
    }
    if (detail::has_json_value(requested, "required")) {
        requested.at("required").get_to(schema.required_);
    }
    return schema;
}

}  // namespace mcp
