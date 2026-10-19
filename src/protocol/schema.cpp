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
#include <optional>
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

/// Evaluation work every validation may do, plus `g_work_per_level` for each node of the value and
/// each level that node is nested at. The vendored jsoncons charges a step one more than the
/// length of its evaluation path, which is what the step costs; in an optimized build one unit
/// takes about 15 ns. Ordinary schemas, including recursive ones through `anyOf`, take at most
/// about 64 units per node and level, so the budget only stops evaluations whose work grows
/// exponentially with the value's depth.
constexpr std::size_t g_base_work = 1'000'000;
constexpr std::size_t g_work_per_level = 256;

struct ValueShape {
    bool too_deep = false;
    std::size_t levels = 0;  ///< Sum over the value's nodes of their depth plus one.
};

/// Measures @p value and checks its nesting against @p depth_limit. Iterative, so it is safe on any
/// value.
ValueShape measure(const nlohmann::json& value, std::size_t depth_limit) {
    ValueShape shape;
    std::vector<std::pair<const nlohmann::json*, std::size_t>> pending{{&value, 0}};
    while (!pending.empty()) {
        const auto [node, depth] = pending.back();
        pending.pop_back();
        shape.levels += depth + 1;
        if (!node->is_structured()) {
            continue;
        }
        if (depth + 1 > depth_limit) {
            shape.too_deep = true;
            return shape;
        }
        for (const auto& child : *node) {
            pending.emplace_back(&child, depth + 1);
        }
    }
    return shape;
}

/// Sets the evaluation work budget for the validations this thread runs while it is alive (see
/// the evaluation limits scripts/vendor_jsoncons.py adds to jsoncons).
class WorkBudget {
   public:
    explicit WorkBudget(std::size_t levels) : saved_(jcs::evaluation_limits::work_left()) {
        jcs::evaluation_limits::work_left() = g_base_work + g_work_per_level * levels;
    }
    ~WorkBudget() { jcs::evaluation_limits::work_left() = saved_; }
    WorkBudget(const WorkBudget&) = delete;
    WorkBudget& operator=(const WorkBudget&) = delete;

   private:
    std::size_t saved_;
};

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

/// Base URI jsoncons gives a schema without `$id`; references are reported relative to it.
constexpr std::string_view g_default_base_uri = "https://jsoncons.com";

/// @p uri as the schema author wrote it when it is relative to the document without `$id`.
std::string_view without_default_base(std::string_view uri) {
    if (uri.substr(0, g_default_base_uri.size()) == g_default_base_uri) {
        uri.remove_prefix(g_default_base_uri.size());
        if (!uri.empty() && uri.front() == '/') {
            uri.remove_prefix(1);
        }
    }
    return uri;
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
    const auto schema_shape = measure(schema, g_max_schema_depth);
    if (schema_shape.too_deep) {
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
    try {
        const WorkBudget budget(schema_shape.levels);
        metaschema(dialect).validate(converted, [&](const jcs::validation_message& message) {
            if (!schema_invalid) {
                first_problem = {message.instance_location().string(), message.message()};
                schema_invalid = true;
            }
            return jcs::walk_state::abort;
        });
    } catch (const jcs::evaluation_limit_error& error) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          describe("checking the JSON Schema against its metaschema: ", error.what()));
    }
    if (schema_invalid) {
        throw SchemaError(
            SchemaError::Category::eInvalidSchema,
            describe("JSON Schema is invalid at '" +
                         detail::sanitize_for_diagnostics(first_problem.instance_path) + "': ",
                     first_problem.message));
    }

    // jsoncons asks this resolver for every $ref target it cannot find in the document or the
    // bundled metaschemas. It refuses all of them, so nothing is ever fetched, and records what it
    // refused so the error can say so.
    std::vector<std::string> refused;
    auto refuse = [&refused](const jc::uri& uri) {
        if (find_dialect(uri.base().string()).empty()) {
            refused.push_back(uri.string());
        }
        return jc::json::null();
    };
    std::optional<jcs::json_schema<jc::json>> compiled;
    try {
        auto evaluation =
            jcs::evaluation_options{}.default_version(std::string(supported_dialects()[0]));
        compiled.emplace(jcs::make_json_schema(std::move(converted), refuse, evaluation));
    } catch (const std::exception& error) {
        if (!refused.empty()) {
            throw SchemaError(SchemaError::Category::eUnresolvedRef,
                              describe("JSON Schema reference cannot be resolved in the document: ",
                                       without_default_base(refused.front())));
        }
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          describe("JSON Schema cannot be compiled: ", error.what()));
    }

    // A reference cycle that never descends into the value exceeds the evaluation limits whatever
    // the value is. Trying a few trivial values finds it now instead of on every validation.
    for (const auto& probe :
         {jc::json::null(), jc::json(jc::json_object_arg), jc::json(jc::json_array_arg), jc::json(""),
          jc::json(0), jc::json(true)}) {
        try {
            const WorkBudget budget(1);
            (void)compiled->is_valid(probe);
        } catch (const jcs::evaluation_limit_error& error) {
            throw SchemaError(
                SchemaError::Category::eInvalidSchema,
                describe("evaluating the JSON Schema against a trivial value: ", error.what()));
        }
    }

    return CompiledSchema(std::make_shared<const CompiledSchemaImpl>(
        CompiledSchemaImpl{schema, options, std::move(*compiled)}));
}

ValidationResult CompiledSchema::validate(const nlohmann::json& instance) const {
    ValidationResult result;
    const auto& options = impl_->options;
    const auto shape = measure(instance, options.max_instance_depth);
    if (shape.too_deep) {
        result.valid = false;
        result.violations.push_back(
            {"", "value nests deeper than " + std::to_string(options.max_instance_depth) + " levels"});
        return result;
    }

    const auto converted = to_jsoncons(instance);
    try {
        const WorkBudget budget(shape.levels);
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
    } catch (const jcs::evaluation_limit_error& error) {
        result.valid = false;
        result.violations.clear();
        result.violations.push_back({"", std::string("validation stopped: ") + error.what()});
    }
    return result;
}

const nlohmann::json& CompiledSchema::schema() const noexcept { return impl_->source; }

}  // namespace mcp
