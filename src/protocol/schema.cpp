#include <mcp/protocol/schema.hpp>

#include "../detail/diagnostic_text.hpp"

// The only translation unit that includes jsoncons. The vendored copy lives in the
// mcp_detail_jsoncons namespace (see scripts/vendor_jsoncons.py), so none of it can collide with a
// jsoncons the application uses.
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>

namespace mcp {

namespace jc = mcp_detail_jsoncons;
namespace jcs = mcp_detail_jsoncons::jsonschema;

namespace {

/// Deepest nesting accepted in a schema document. Checking a schema against its metaschema and
/// compiling it both recurse with the schema, so this bounds the stack they use.
constexpr std::size_t g_max_schema_depth = 64;

/// The `$schema` values this SDK implements, without a trailing empty fragment.
const std::array<std::string_view, 5>& supported_dialects() {
    static const std::array<std::string_view, 5> dialects = {
        "https://json-schema.org/draft/2020-12/schema", "https://json-schema.org/draft/2019-09/schema",
        "http://json-schema.org/draft-07/schema",       "http://json-schema.org/draft-06/schema",
        "http://json-schema.org/draft-04/schema",
    };
    return dialects;
}

/// The supported dialect `$schema` names, or an empty view when it names none. "...schema#" and
/// "...schema" name the same dialect.
std::string_view find_dialect(std::string_view uri) {
    if (!uri.empty() && uri.back() == '#') {
        uri.remove_suffix(1);
    }
    for (auto dialect : supported_dialects()) {
        if (uri == dialect) {
            return dialect;
        }
    }
    return {};
}

/// Whether @p value nests objects and arrays deeper than @p limit. Iterative, so it is safe on any
/// value.
bool exceeds_depth(const nlohmann::json& value, std::size_t limit) {
    std::vector<std::pair<const nlohmann::json*, std::size_t>> pending{{&value, 0}};
    while (!pending.empty()) {
        const auto [node, depth] = pending.back();
        pending.pop_back();
        if (!node->is_structured()) {
            continue;
        }
        if (depth + 1 > limit) {
            return true;
        }
        for (const auto& child : *node) {
            pending.emplace_back(&child, depth + 1);
        }
    }
    return false;
}

/// Copies an nlohmann value into a jsoncons value. Recurses once per level, so callers check the
/// depth first.
jc::json to_jsoncons(const nlohmann::json& value) {
    switch (value.type()) {
        case nlohmann::json::value_t::null:
            return jc::json::null();
        case nlohmann::json::value_t::boolean:
            return jc::json(value.get<bool>());
        case nlohmann::json::value_t::number_integer:
            return jc::json(value.get<std::int64_t>());
        case nlohmann::json::value_t::number_unsigned:
            return jc::json(value.get<std::uint64_t>());
        case nlohmann::json::value_t::number_float:
            return jc::json(value.get<double>());
        case nlohmann::json::value_t::string:
            return jc::json(value.get_ref<const std::string&>());
        case nlohmann::json::value_t::array: {
            jc::json array(jc::json_array_arg);
            array.reserve(value.size());
            for (const auto& element : value) {
                array.push_back(to_jsoncons(element));
            }
            return array;
        }
        case nlohmann::json::value_t::object: {
            jc::json object(jc::json_object_arg);
            for (const auto& [key, member] : value.items()) {
                object.try_emplace(key, to_jsoncons(member));
            }
            return object;
        }
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            break;
    }
    throw std::invalid_argument("value has no JSON representation");
}

/// The compiled metaschema of @p dialect, built once.
const jcs::json_schema<jc::json>& metaschema(std::string_view dialect) {
    auto compile_meta = [](std::string_view uri) {
        return jcs::make_json_schema(jcs::meta_resolver<jc::json>(jc::uri(std::string(uri))));
    };
    static const std::array<jcs::json_schema<jc::json>, 5> compiled = {
        compile_meta(supported_dialects()[0]), compile_meta(supported_dialects()[1]),
        compile_meta(supported_dialects()[2]), compile_meta(supported_dialects()[3]),
        compile_meta(supported_dialects()[4]),
    };
    for (std::size_t i = 0; i < supported_dialects().size(); ++i) {
        if (supported_dialects()[i] == dialect) {
            return compiled[i];
        }
    }
    return compiled[0];
}

std::string describe(std::string_view prefix, std::string_view detail) {
    return std::string(prefix) + detail::sanitize_for_diagnostics(detail);
}

}  // namespace

struct CompiledSchemaImpl {
    nlohmann::json source;
    SchemaOptions options;
    jcs::json_schema<jc::json> compiled;
};

SchemaError::SchemaError(Category category, const std::string& message)
    : std::invalid_argument(message), category_(category) {}

CompiledSchema::CompiledSchema(std::shared_ptr<const CompiledSchemaImpl> impl)
    : impl_(std::move(impl)) {}

CompiledSchema CompiledSchema::compile(const nlohmann::json& schema, const SchemaOptions& options) {
    if (!schema.is_object() && !schema.is_boolean()) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          "JSON Schema must be an object or a boolean");
    }
    if (exceeds_depth(schema, g_max_schema_depth)) {
        throw SchemaError(
            SchemaError::Category::eInvalidSchema,
            "JSON Schema nests deeper than " + std::to_string(g_max_schema_depth) + " levels");
    }

    std::string_view dialect = supported_dialects()[0];
    if (schema.is_object()) {
        const auto declared = schema.find("$schema");
        if (declared != schema.end()) {
            if (!declared->is_string()) {
                throw SchemaError(SchemaError::Category::eInvalidSchema, "$schema must be a string");
            }
            dialect = find_dialect(declared->get_ref<const std::string&>());
            if (dialect.empty()) {
                throw SchemaError(SchemaError::Category::eUnsupportedDialect,
                                  describe("unsupported JSON Schema dialect: ",
                                           declared->get_ref<const std::string&>()));
            }
        }
    }

    auto converted = to_jsoncons(schema);

    SchemaViolation first_problem;
    bool schema_invalid = false;
    metaschema(dialect).validate(converted, [&](const jcs::validation_message& message) {
        if (!schema_invalid) {
            first_problem = {message.instance_location().string(), message.message()};
            schema_invalid = true;
        }
        return jcs::walk_state::abort;
    });
    if (schema_invalid) {
        throw SchemaError(
            SchemaError::Category::eInvalidSchema,
            describe("JSON Schema is invalid at '" +
                         detail::sanitize_for_diagnostics(first_problem.instance_path) + "': ",
                     first_problem.message));
    }

    try {
        auto evaluation =
            jcs::evaluation_options{}.default_version(std::string(supported_dialects()[0]));
        auto compiled = jcs::make_json_schema(std::move(converted), evaluation);
        return CompiledSchema(std::make_shared<const CompiledSchemaImpl>(
            CompiledSchemaImpl{schema, options, std::move(compiled)}));
    } catch (const std::exception& error) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          describe("JSON Schema cannot be compiled: ", error.what()));
    }
}

ValidationResult CompiledSchema::validate(const nlohmann::json& instance) const {
    ValidationResult result;
    const auto& options = impl_->options;
    if (exceeds_depth(instance, options.max_instance_depth)) {
        result.valid = false;
        result.violations.push_back(
            {"", "value nests deeper than " + std::to_string(options.max_instance_depth) + " levels"});
        return result;
    }

    const auto converted = to_jsoncons(instance);
    impl_->compiled.validate(converted, [&](const jcs::validation_message& message) {
        result.valid = false;
        if (result.violations.size() < options.max_reported_violations) {
            result.violations.push_back(
                {detail::sanitize_for_diagnostics(message.instance_location().string()),
                 detail::sanitize_for_diagnostics(message.message())});
        }
        return result.violations.size() < options.max_reported_violations ? jcs::walk_state::advance
                                                                          : jcs::walk_state::abort;
    });
    return result;
}

const nlohmann::json& CompiledSchema::schema() const noexcept { return impl_->source; }

}  // namespace mcp
