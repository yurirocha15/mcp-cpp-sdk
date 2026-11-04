#include <mcp/protocol/schema.hpp>

#include <mcp/detail/json_dump.hpp>
#include <mcp/detail/regex.hpp>
#include "../detail/diagnostic_text.hpp"
#include "native_schema.hpp"
#include "schema_cache.hpp"

// The only translation unit that includes jsoncons. The vendored copy lives in the
// mcp_detail_jsoncons namespace (see scripts/vendor_jsoncons.py), so none of it can collide with a
// jsoncons the application uses.
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

/// Evaluation work every validation may do. The vendored jsoncons charges a step one more than the
/// length of its evaluation path, which is what the step costs, and charges copies of locations
/// and whole-string keywords by their length (see scripts/vendor_jsoncons.py).
constexpr std::size_t g_base_work = 1'000'000;
/// Work allowed per unit of the value (see ValueShape::units), before the allowance for each node
/// of the schema below.
constexpr std::size_t g_work_per_unit = 256;
/// Further work allowed per unit of the value for each node of the schema: a step that compares
/// the value with a schema value, such as an `enum` member, costs 8.
constexpr std::size_t g_work_per_unit_and_schema_node = 8;
/// Memory the messages kept for failed branches may hold at once during one validation.
constexpr std::size_t g_max_kept_message_bytes = 32 * 1024 * 1024;

std::size_t saturating_add(std::size_t left, std::size_t right) {
    return left > std::numeric_limits<std::size_t>::max() - right
               ? std::numeric_limits<std::size_t>::max()
               : left + right;
}

std::size_t saturating_multiply(std::size_t left, std::size_t right) {
    return right != 0 && left > std::numeric_limits<std::size_t>::max() / right
               ? std::numeric_limits<std::size_t>::max()
               : left * right;
}

struct ValueShape {
    bool too_deep = false;
    bool non_finite = false;  ///< Holds a NaN or an infinity, which JSON cannot represent.
    std::size_t nodes = 0;    ///< Values in the value, itself included.
    std::size_t levels = 0;   ///< Sum over the value's nodes of their depth plus one.
    /// Sum over the value's nodes of the bytes of the property names on their path, as much of
    /// each name as a location keeps.
    std::size_t path_bytes = 0;
    std::size_t text_bytes = 0;  ///< Bytes of the strings and property names in the value.

    /// The size of the value as evaluation work grows with it. A step at a node costs what copying
    /// its location costs, one per level and one per 32 bytes of the names on its path, and every
    /// 16 bytes of text cost one, as reading the text does.
    [[nodiscard]] std::size_t units() const {
        return saturating_add(saturating_add(levels, path_bytes / 32), text_bytes / 16);
    }

    /// The evaluation work a validation of the value may do against a schema of @p schema_nodes
    /// nodes, at most @p max_work. It grows with the product of the two sizes, which pays for each
    /// part of the schema evaluating each part of the value a few times, and stops evaluations
    /// whose work grows exponentially with the value's depth.
    [[nodiscard]] std::size_t evaluation_work(std::size_t schema_nodes, std::size_t max_work) const {
        const auto per_unit = saturating_add(
            g_work_per_unit, saturating_multiply(g_work_per_unit_and_schema_node, schema_nodes));
        const auto work = saturating_add(g_base_work, saturating_multiply(units(), per_unit));
        return std::min(work, max_work);
    }

    /// The pattern matching steps (see mcp::detail::Regex::search) a validation of the value may
    /// take under @p options. Typical patterns take a handful of steps per byte; one that runs many
    /// threads at once over a long string, such as an unanchored ".{1,1000}x", takes thousands.
    [[nodiscard]] std::size_t pattern_steps(const SchemaOptions& options) const {
        const auto limit = std::numeric_limits<std::size_t>::max();
        const auto per_byte = options.pattern_steps_per_text_byte;
        if (per_byte != 0 && text_bytes > limit / per_byte) {
            return limit;
        }
        const auto allowance = per_byte * text_bytes;
        return allowance > limit - options.base_pattern_steps ? limit
                                                              : options.base_pattern_steps + allowance;
    }
};

/// Measures @p value and checks its nesting against @p depth_limit. Iterative, so it is safe on any
/// value.
ValueShape measure(const nlohmann::json& value, std::size_t depth_limit) {
    ValueShape shape;
    struct Pending {
        const nlohmann::json* node;
        std::size_t depth;
        std::size_t path_bytes;
    };
    std::vector<Pending> pending{{&value, 0, 0}};
    while (!pending.empty()) {
        const auto [node, depth, path_bytes] = pending.back();
        pending.pop_back();
        ++shape.nodes;
        shape.levels += depth + 1;
        shape.path_bytes = saturating_add(shape.path_bytes, path_bytes);
        if (!node->is_structured()) {
            shape.non_finite =
                shape.non_finite || (node->is_number_float() && !std::isfinite(node->get<double>()));
            if (node->is_string()) {
                shape.text_bytes += node->get_ref<const std::string&>().size();
            }
            continue;
        }
        if (depth + 1 > depth_limit) {
            shape.too_deep = true;
            return shape;
        }
        if (node->is_object()) {
            for (const auto& [key, child] : node->items()) {
                shape.text_bytes += key.size();
                const auto kept = std::min(key.size(), jcs::evaluation_limits::max_location_token);
                pending.push_back({&child, depth + 1, path_bytes + kept});
            }
        } else {
            for (const auto& child : *node) {
                pending.push_back({&child, depth + 1, path_bytes});
            }
        }
    }
    return shape;
}

/// Sets the evaluation work, pattern matching and kept message budgets for the validations this
/// thread runs while it is alive (see the evaluation limits scripts/vendor_jsoncons.py adds to
/// jsoncons).
class WorkBudget {
   public:
    WorkBudget(const ValueShape& shape, std::size_t schema_nodes, const SchemaOptions& options)
        : saved_work_(jcs::evaluation_limits::work_left()),
          saved_pattern_steps_(jcs::evaluation_limits::pattern_steps_left()),
          saved_message_bytes_(jcs::evaluation_limits::kept_message_bytes_left()) {
        jcs::evaluation_limits::work_left() =
            shape.evaluation_work(schema_nodes, options.max_evaluation_work);
        jcs::evaluation_limits::pattern_steps_left() = shape.pattern_steps(options);
        jcs::evaluation_limits::kept_message_bytes_left() = g_max_kept_message_bytes;
    }
    ~WorkBudget() {
        jcs::evaluation_limits::work_left() = saved_work_;
        jcs::evaluation_limits::pattern_steps_left() = saved_pattern_steps_;
        jcs::evaluation_limits::kept_message_bytes_left() = saved_message_bytes_;
    }
    WorkBudget(const WorkBudget&) = delete;
    WorkBudget& operator=(const WorkBudget&) = delete;

   private:
    std::size_t saved_work_;
    std::size_t saved_pattern_steps_;
    std::size_t saved_message_bytes_;
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
        case nlohmann::json::value_t::number_float: {
            // JSON cannot carry NaN or Infinity; nlohmann writes them as null, which is what a
            // peer would receive, so that is what is checked.
            const auto number = value.get<double>();
            return std::isfinite(number) ? jc::json(number) : jc::json::null();
        }
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

/// The innermost problem @p message reports. The 2020-12 and 2019-09 metaschemas combine their
/// vocabularies with `allOf`, so the outermost message only says that a subschema failed.
const jcs::validation_message& innermost(const jcs::validation_message& message) {
    const auto* current = &message;
    while (!current->details().empty()) {
        current = &current->details().front();
    }
    return *current;
}

/// Whether @p document has a `$schema` member whose value is @p uri, compared without an empty
/// trailing fragment. Iterative, so it is safe on any document.
bool declares_dialect(const nlohmann::json& document, std::string_view uri) {
    const auto without_fragment = [](std::string_view text) {
        if (!text.empty() && text.back() == '#') {
            text.remove_suffix(1);
        }
        return text;
    };
    uri = without_fragment(uri);
    std::vector<const nlohmann::json*> pending{&document};
    while (!pending.empty()) {
        const auto* node = pending.back();
        pending.pop_back();
        if (node->is_object()) {
            for (const auto& [key, member] : node->items()) {
                if (key == "$schema" && member.is_string() &&
                    without_fragment(member.get_ref<const std::string&>()) == uri) {
                    return true;
                }
                pending.push_back(&member);
            }
        } else if (node->is_array()) {
            for (const auto& member : *node) {
                pending.push_back(&member);
            }
        }
    }
    return false;
}

std::string describe(std::string_view prefix, std::string_view detail) {
    return std::string(prefix) + detail::sanitize_for_diagnostics(detail);
}

/// The error for @p error, a pattern of the schema that cannot be used. A pattern that is not a
/// regular expression makes the schema invalid; one that is valid but cannot be run here is
/// reported as unsupported.
SchemaError pattern_error(const detail::RegexError& error) {
    if (error.kind() == detail::RegexError::Kind::eInvalid) {
        return SchemaError(SchemaError::Category::eInvalidSchema,
                           describe("JSON Schema pattern is not a regular expression: ", error.what()));
    }
    return SchemaError(SchemaError::Category::eUnsupportedPattern,
                       describe("JSON Schema pattern cannot be used: ", error.what()));
}

}  // namespace

struct CompiledSchemaImpl {
    nlohmann::json source;
    SchemaOptions options;
    std::size_t schema_nodes;  ///< Values in the schema document, itself included.
    jcs::json_schema<jc::json> compiled;
    /// Set when the schema uses only the keywords NativeSchema implements; validation then walks
    /// the value directly instead of converting it for jsoncons.
    std::optional<detail::NativeSchema> native;
};

namespace {

bool same_options(const SchemaOptions& left, const SchemaOptions& right) {
    return left.max_instance_depth == right.max_instance_depth &&
           left.max_reported_violations == right.max_reported_violations &&
           left.base_pattern_steps == right.base_pattern_steps &&
           left.pattern_steps_per_text_byte == right.pattern_steps_per_text_byte &&
           left.max_evaluation_work == right.max_evaluation_work &&
           left.max_pattern_instructions == right.max_pattern_instructions;
}

/// Whether @p left and @p right are the same document, down to how each number is written: an
/// integer is never the same as a float, and floats are the same only bit for bit. nlohmann's
/// operator== takes 1 and 1.0, or 0.0 and -0.0, as equal, but they are written differently and a
/// compiled schema reports them as written. Recursive: a schema is compiled, and so compared,
/// only once measure() has bounded its depth.
bool same_document(const nlohmann::json& left, const nlohmann::json& right) {
    using value_t = nlohmann::json::value_t;
    // An integer is written by its value whether it is held signed or unsigned.
    const auto as_unsigned = [](const nlohmann::json& value) -> std::optional<std::uint64_t> {
        if (value.is_number_unsigned()) {
            return value.get<std::uint64_t>();
        }
        const auto number = value.get<std::int64_t>();
        return number < 0 ? std::nullopt : std::optional<std::uint64_t>(number);
    };
    if (left.is_number_integer() && right.is_number_integer() && left.type() != right.type()) {
        const auto a = as_unsigned(left);
        return a && a == as_unsigned(right);
    }
    if (left.type() != right.type()) {
        return false;
    }
    switch (left.type()) {
        case value_t::object: {
            const auto& a = left.get_ref<const nlohmann::json::object_t&>();
            const auto& b = right.get_ref<const nlohmann::json::object_t&>();
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
                       return x.first == y.first && same_document(x.second, y.second);
                   });
        }
        case value_t::array: {
            const auto& a = left.get_ref<const nlohmann::json::array_t&>();
            const auto& b = right.get_ref<const nlohmann::json::array_t&>();
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), same_document);
        }
        case value_t::string:
            return left.get_ref<const std::string&>() == right.get_ref<const std::string&>();
        case value_t::boolean:
            return left.get<bool>() == right.get<bool>();
        case value_t::number_float: {
            const auto a = left.get<double>();
            const auto b = right.get<double>();
            return a == b && std::signbit(a) == std::signbit(b);
        }
        default:
            return left == right;
    }
}

void mix(std::size_t& hash, std::size_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}

void mix_document(std::size_t& hash, const nlohmann::json& node) {
    using value_t = nlohmann::json::value_t;
    const auto type = node.type();
    mix(hash,
        static_cast<std::size_t>(type == value_t::number_unsigned ? value_t::number_integer : type));
    switch (type) {
        case value_t::object:
            mix(hash, node.size());
            for (const auto& [key, value] : node.get_ref<const nlohmann::json::object_t&>()) {
                mix(hash, std::hash<std::string_view>{}(key));
                mix_document(hash, value);
            }
            break;
        case value_t::array:
            mix(hash, node.size());
            for (const auto& value : node.get_ref<const nlohmann::json::array_t&>()) {
                mix_document(hash, value);
            }
            break;
        case value_t::string:
            mix(hash, std::hash<std::string_view>{}(node.get_ref<const std::string&>()));
            break;
        case value_t::boolean:
            mix(hash, node.get<bool>() ? 1 : 0);
            break;
        case value_t::number_integer:
            mix(hash, static_cast<std::size_t>(node.get<std::int64_t>()));
            break;
        case value_t::number_unsigned:
            mix(hash, static_cast<std::size_t>(node.get<std::uint64_t>()));
            break;
        case value_t::number_float:
            mix(hash, std::hash<double>{}(node.get<double>()));
            break;
        default:
            break;
    }
}

/// A hash of @p schema that same_document() documents share, and of @p options. Recursive, like
/// same_document().
std::size_t document_hash(const nlohmann::json& schema, const SchemaOptions& options) {
    std::size_t hash = 0;
    for (const auto option : {options.max_instance_depth, options.max_reported_violations,
                              options.base_pattern_steps, options.pattern_steps_per_text_byte,
                              options.max_evaluation_work, options.max_pattern_instructions}) {
        mix(hash, option);
    }
    mix_document(hash, schema);
    return hash;
}

/// Compiled schemas by their document and options. A server that builds one `Server` per session
/// registers the same tools every time; compiling is far dearer than validating, so a repeat
/// compile of the same schema returns the one already built. An entry is found by a hash of the
/// document and confirmed by comparing it with the schema the compiled form holds, so a lookup
/// neither serializes the schema nor allocates. The cache holds a schema only weakly: it serves a
/// repeat while a session still holds the schema, and releases the compiled form with the last
/// holder, so a schema taken from a peer, such as a client's tool listing, is not kept for the life
/// of the process. An entry whose schema is gone holds only its hash until a later insert drops
/// it; the cache is bounded in entries and evicts the oldest first.
class CompiledSchemaCache {
   public:
    std::shared_ptr<const CompiledSchemaImpl> find(std::size_t hash, const nlohmann::json& schema,
                                                   const SchemaOptions& options) {
        std::lock_guard lock(mutex_);
        return find_locked(hash, schema, options);
    }

    void insert(std::size_t hash, const std::shared_ptr<const CompiledSchemaImpl>& compiled) {
        std::lock_guard lock(mutex_);
        if (find_locked(hash, compiled->source, compiled->options)) {
            return;
        }
        // Every entry whose schema is gone goes first, then the oldest if the cache is still full.
        for (auto entry = order_.begin(); entry != order_.end();) {
            const auto current = entry++;
            if (current->compiled.expired()) {
                erase(current);
            }
        }
        while (order_.size() >= g_max_entries) {
            erase(order_.begin());
        }
        order_.push_back(Entry{hash, compiled});
        index_.emplace(hash, std::prev(order_.end()));
    }

    std::size_t live_entries() {
        std::lock_guard lock(mutex_);
        std::size_t live = 0;
        for (const auto& entry : order_) {
            if (!entry.compiled.expired()) {
                ++live;
            }
        }
        return live;
    }

   private:
    struct Entry {
        std::size_t hash;
        std::weak_ptr<const CompiledSchemaImpl> compiled;
    };
    using Order = std::list<Entry>;

    static constexpr std::size_t g_max_entries = 256;

    std::shared_ptr<const CompiledSchemaImpl> find_locked(std::size_t hash,
                                                          const nlohmann::json& schema,
                                                          const SchemaOptions& options) {
        auto [it, end] = index_.equal_range(hash);
        while (it != end) {
            const auto entry = it->second;
            ++it;
            auto compiled = entry->compiled.lock();
            if (!compiled) {
                erase(entry);
                continue;
            }
            if (same_options(compiled->options, options) && same_document(compiled->source, schema)) {
                return compiled;
            }
        }
        return nullptr;
    }

    void erase(Order::iterator entry) {
        auto [it, end] = index_.equal_range(entry->hash);
        for (; it != end; ++it) {
            if (it->second == entry) {
                index_.erase(it);
                break;
            }
        }
        order_.erase(entry);
    }

    std::mutex mutex_;
    /// Oldest first. Nodes do not move, so the iterators in index_ stay valid until the node goes.
    Order order_;
    std::unordered_multimap<std::size_t, Order::iterator> index_;
};

CompiledSchemaCache& compiled_schema_cache() {
    static CompiledSchemaCache cache;
    return cache;
}

}  // namespace

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
    if (schema_shape.non_finite) {
        // Serialized for a peer, it would carry null in that place: not the schema checked here.
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          "JSON Schema contains NaN or Infinity, which JSON cannot represent");
    }

    const auto hash = document_hash(schema, options);
    if (auto cached = compiled_schema_cache().find(hash, schema, options)) {
        return CompiledSchema(std::move(cached));
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
        // The metaschema is the schema here; its size is fixed, so the value alone sets the budget.
        const WorkBudget budget(schema_shape, 0, options);
        metaschema(dialect).validate(converted, [&](const jcs::validation_message& message) {
            if (!schema_invalid) {
                const auto& problem = innermost(message);
                first_problem = {problem.instance_location().string(), problem.message()};
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
    // Patterns that cannot be used are collected instead of ending the compile, so that any other
    // problem the schema has is found and reported first.
    detail::PatternBudget patterns(options.max_pattern_instructions);
    try {
        auto evaluation =
            jcs::evaluation_options{}.default_version(std::string(supported_dialects()[0]));
        compiled.emplace(jcs::make_json_schema(std::move(converted), refuse, evaluation));
    } catch (const detail::RegexError& error) {
        throw pattern_error(error);
    } catch (const std::exception& error) {
        if (!refused.empty()) {
            // jsoncons also asks for the metaschema of an embedded resource's `$schema`.
            if (declares_dialect(schema, refused.front())) {
                throw SchemaError(SchemaError::Category::eUnsupportedDialect,
                                  describe("unsupported JSON Schema dialect: ", refused.front()));
            }
            throw SchemaError(SchemaError::Category::eUnresolvedRef,
                              describe("JSON Schema reference cannot be resolved in the document: ",
                                       without_default_base(refused.front())));
        }
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          describe("JSON Schema cannot be compiled: ", error.what()));
    }
    // A reference cycle that never descends into the value exceeds the evaluation limits whatever
    // the value is. Trying a few trivial values finds it now instead of on every validation. A
    // pattern that could not be compiled stands in as one that matches nothing until then.
    ValueShape trivial;
    trivial.nodes = 1;
    trivial.levels = 1;
    for (const auto& probe :
         {jc::json::null(), jc::json(jc::json_object_arg), jc::json(jc::json_array_arg), jc::json(""),
          jc::json(0), jc::json(true)}) {
        try {
            const WorkBudget budget(trivial, schema_shape.nodes, options);
            (void)compiled->is_valid(probe);
        } catch (const jcs::evaluation_limit_error& error) {
            throw SchemaError(
                SchemaError::Category::eInvalidSchema,
                describe("evaluating the JSON Schema against a trivial value: ", error.what()));
        }
    }
    if (patterns.problem()) {
        throw pattern_error(*patterns.problem());
    }

    // NativeSchema compiles the patterns again, within a budget of its own; one it cannot compile
    // leaves the schema to jsoncons.
    auto native = [&schema, &options] {
        const detail::PatternBudget native_patterns(options.max_pattern_instructions);
        return detail::NativeSchema::compile(schema);
    }();
    auto result = std::make_shared<const CompiledSchemaImpl>(CompiledSchemaImpl{
        schema, options, schema_shape.nodes, std::move(*compiled), std::move(native)});
    compiled_schema_cache().insert(hash, result);
    return CompiledSchema(std::move(result));
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

    if (impl_->native) {
        impl_->native->validate(instance, options.max_reported_violations, shape.pattern_steps(options),
                                shape.evaluation_work(impl_->schema_nodes, options.max_evaluation_work),
                                result);
        return result;
    }

    const auto converted = to_jsoncons(instance);
    try {
        const WorkBudget budget(shape, impl_->schema_nodes, options);
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

namespace detail {

std::size_t compiled_schema_cache_live_entries() { return compiled_schema_cache().live_entries(); }

}  // namespace detail

}  // namespace mcp
