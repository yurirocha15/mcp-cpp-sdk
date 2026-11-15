#include <mcp/protocol/elicitation.hpp>

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

}  // namespace mcp
