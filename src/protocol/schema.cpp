#include <mcp/protocol/schema.hpp>

#include <mcp/detail/json_dump.hpp>
#include "mcp/detail/constants.hpp"
#include "mcp/detail/protocol/schema_cache.hpp"
#include "mcp/detail/protocol/schema_uri.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"
#include "mcp/detail/text/regex.hpp"
#include "native_schema.hpp"
#include "schema_checker.hpp"
#include "schema_dialect.hpp"
#include "schema_values.hpp"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

namespace {

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
    /// its location costs, one per level and one per g_PATH_BYTES_PER_UNIT bytes of the names on its
    /// path, and every g_TEXT_BYTES_PER_UNIT bytes of text cost one, as reading the text does.
    [[nodiscard]] std::size_t units() const {
        namespace limits = mcp::detail::constants::schema;
        return saturating_add(saturating_add(levels, path_bytes / limits::g_PATH_BYTES_PER_UNIT),
                              text_bytes / limits::g_TEXT_BYTES_PER_UNIT);
    }

    /// The evaluation work a validation of the value may do against a schema of @p schema_nodes
    /// nodes, at most @p max_work. It grows with the product of the two sizes, which pays for each
    /// part of the schema evaluating each part of the value a few times, and stops evaluations
    /// whose work grows exponentially with the value's depth.
    [[nodiscard]] std::size_t evaluation_work(std::size_t schema_nodes, std::size_t max_work) const {
        const auto per_unit = saturating_add(
            mcp::detail::constants::schema::g_WORK_PER_UNIT,
            saturating_multiply(mcp::detail::constants::schema::g_WORK_PER_UNIT_AND_SCHEMA_NODE,
                                schema_nodes));
        const auto work = saturating_add(mcp::detail::constants::schema::g_BASE_WORK,
                                         saturating_multiply(units(), per_unit));
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
    std::vector<Pending> pending{{.node = &value, .depth = 0, .path_bytes = 0}};
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
                const auto kept =
                    std::min(key.size(), mcp::detail::constants::schema::g_MAX_COUNTED_NAME_BYTES);
                pending.push_back(
                    {.node = &child, .depth = depth + 1, .path_bytes = path_bytes + kept});
            }
        } else {
            for (const auto& child : *node) {
                pending.push_back({.node = &child, .depth = depth + 1, .path_bytes = path_bytes});
            }
        }
    }
    return shape;
}

std::string describe(std::string_view prefix, std::string_view detail) {
    return std::string(prefix) + detail::sanitize_for_diagnostics(detail);
}

/// The error for @p error, a pattern of the schema that cannot be used. A pattern that is not a
/// regular expression makes the schema invalid; one that is valid but cannot be run here is
/// reported as unsupported.
SchemaError pattern_error(const detail::RegexError& error) {
    if (error.kind() == detail::RegexError::Kind::eInvalid) {
        return {SchemaError::Category::eInvalidSchema,
                describe("JSON Schema pattern is not a regular expression: ", error.what())};
    }
    return {SchemaError::Category::eUnsupportedPattern,
            describe("JSON Schema pattern cannot be used: ", error.what())};
}

}  // namespace

struct CompiledSchemaImpl {
    nlohmann::json source;
    SchemaOptions options;
    std::size_t schema_nodes;  ///< Values in the schema document, itself included.
    detail::NativeSchema native;
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
/// compiled schema reports them as written. Recursive: one side is always the source of a compiled
/// schema, which measure() has bounded, and the comparison goes no deeper than that side.
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

/// The classic boost::hash_combine step, with 2^64 divided by the golden ratio as its constant.
void mix(std::size_t& hash, std::size_t value) {
    constexpr std::size_t golden_ratio = 0x9e3779b97f4a7c15ULL;
    constexpr unsigned left_shift = 6;
    constexpr unsigned right_shift = 2;
    hash ^= value + golden_ratio + (hash << left_shift) + (hash >> right_shift);
}

/// Mixes @p node, found @p depth levels below the root, into @p hash. Returns false, leaving the
/// hash unfinished, when a container nests deeper than
/// mcp::detail::constants::schema::g_MAX_DEPTH, the depth at which measure() refuses a schema;
/// so the recursion is bounded.
bool mix_document(std::size_t& hash, const nlohmann::json& node, std::size_t depth) {
    using value_t = nlohmann::json::value_t;
    const auto type = node.type();
    mix(hash,
        static_cast<std::size_t>(type == value_t::number_unsigned ? value_t::number_integer : type));
    if (node.is_structured() && depth + 1 > mcp::detail::constants::schema::g_MAX_DEPTH) {
        return false;
    }
    switch (type) {
        case value_t::object:
            mix(hash, node.size());
            for (const auto& [key, value] : node.get_ref<const nlohmann::json::object_t&>()) {
                mix(hash, std::hash<std::string_view>{}(key));
                if (!mix_document(hash, value, depth + 1)) {
                    return false;
                }
            }
            break;
        case value_t::array:
            mix(hash, node.size());
            for (const auto& value : node.get_ref<const nlohmann::json::array_t&>()) {
                if (!mix_document(hash, value, depth + 1)) {
                    return false;
                }
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
    return true;
}

/// A hash of @p schema that same_document() documents share, and of @p options, or nothing when
/// the schema nests deeper than measure() allows.
std::optional<std::size_t> document_hash(const nlohmann::json& schema, const SchemaOptions& options) {
    std::size_t hash = 0;
    for (const auto option : {
             options.max_instance_depth,
             options.max_reported_violations,
             options.base_pattern_steps,
             options.pattern_steps_per_text_byte,
             options.max_evaluation_work,
             options.max_pattern_instructions,
         }) {
        mix(hash, option);
    }
    if (!mix_document(hash, schema, 0)) {
        return std::nullopt;
    }
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
        std::scoped_lock lock(mutex_);
        return find_locked(hash, schema, options);
    }

    void insert(std::size_t hash, const std::shared_ptr<const CompiledSchemaImpl>& compiled) {
        std::scoped_lock lock(mutex_);
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
        order_.push_back(Entry{.hash = hash, .compiled = compiled});
        index_.emplace(hash, std::prev(order_.end()));
    }

    std::size_t live_entries() {
        std::scoped_lock lock(mutex_);
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
    static CompiledSchemaCache s_cache;
    return s_cache;
}

/// A schema document that passed the checks every document gets.
struct CheckedDocument {
    ValueShape shape;
    detail::SchemaDialect dialect;
};

/// Checks @p schema's form, size and dialect, and checks it against its dialect's rules,
/// @p dialect when it has no `$schema`; @p where prefixes every message.
CheckedDocument check_document(const nlohmann::json& schema, const SchemaOptions& options,
                               detail::SchemaDialect dialect, const std::string& where) {
    if (!schema.is_object() && !schema.is_boolean()) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          where + "JSON Schema must be an object or a boolean");
    }
    CheckedDocument checked{.shape = measure(schema, mcp::detail::constants::schema::g_MAX_DEPTH),
                            .dialect = dialect};
    if (checked.shape.too_deep) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          where + "JSON Schema nests deeper than " +
                              std::to_string(mcp::detail::constants::schema::g_MAX_DEPTH) + " levels");
    }
    if (checked.shape.non_finite) {
        // Serialized for a peer, it would carry null in that place: not the schema checked here.
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          where + "JSON Schema contains NaN or Infinity, which JSON cannot represent");
    }

    if (schema.is_object()) {
        const auto declared = schema.find("$schema");
        if (declared != schema.end()) {
            if (!declared->is_string()) {
                throw SchemaError(SchemaError::Category::eInvalidSchema,
                                  where + "$schema must be a string");
            }
            const auto named = detail::find_schema_dialect(declared->get_ref<const std::string&>());
            if (!named) {
                throw SchemaError(SchemaError::Category::eUnsupportedDialect,
                                  where + describe("unsupported JSON Schema dialect: ",
                                                   declared->get_ref<const std::string&>()));
            }
            dialect = *named;
        }
    }

    // The check is charged as an evaluation of the schema as a value, against a schema of no nodes.
    std::size_t work = checked.shape.evaluation_work(0, options.max_evaluation_work);
    std::optional<detail::SchemaProblem> problem;
    try {
        // measure() has bounded the nesting, so the check sets no depth bound of its own.
        problem = detail::check_schema(schema, dialect, work, std::numeric_limits<std::size_t>::max());
    } catch (const detail::WorkBudgetExceeded&) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          where + "checking the JSON Schema takes too much work");
    }
    if (problem) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          where + describe("JSON Schema is invalid at '" +
                                               detail::sanitize_for_diagnostics(problem->path) + "': ",
                                           problem->message));
    }
    checked.dialect = dialect;
    return checked;
}

/// Checks the documents a schema of @p dialect may reach, each read in that dialect unless it
/// declares its own; returns the values they hold.
std::size_t check_documents(const SchemaDocuments& documents, const SchemaOptions& options,
                            detail::SchemaDialect dialect) {
    if (documents.size() > mcp::detail::constants::schema::g_MAX_SCHEMA_DOCUMENTS) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          "more than " +
                              std::to_string(mcp::detail::constants::schema::g_MAX_SCHEMA_DOCUMENTS) +
                              " schema documents");
    }
    std::size_t nodes = 0;
    for (const auto& [uri, document] : documents) {
        const auto reference = detail::parse_uri_reference(uri);
        if (!reference.scheme || reference.fragment) {
            throw SchemaError(
                SchemaError::Category::eInvalidSchema,
                describe("schema document URI is not absolute without a fragment: ", uri));
        }
        nodes = saturating_add(
            nodes, check_document(document, options, dialect, describe("schema document ", uri) + ": ")
                       .shape.nodes);
    }
    return nodes;
}

}  // namespace

SchemaError::SchemaError(Category category, const std::string& message)
    : std::invalid_argument(message), category_(category) {}

CompiledSchema::CompiledSchema(std::shared_ptr<const CompiledSchemaImpl> impl)
    : impl_(std::move(impl)) {}

CompiledSchema CompiledSchema::compile(const nlohmann::json& schema, const SchemaOptions& options,
                                       const SchemaDocuments& documents) {
    if (!schema.is_object() && !schema.is_boolean()) {
        throw SchemaError(SchemaError::Category::eInvalidSchema,
                          "JSON Schema must be an object or a boolean");
    }
    // A schema the cache holds passed every check below when it was compiled, and a repeat is the
    // same document, so a hit needs neither measuring nor checking. NaN, the one value unequal to
    // itself, never matches, and Infinity is never in the cache. A schema compiled with documents
    // depends on them too, so it neither reads nor fills the cache.
    const bool use_cache = documents.empty();
    const auto hash = document_hash(schema, options);
    if (hash && use_cache) {
        if (auto cached = compiled_schema_cache().find(*hash, schema, options)) {
            return CompiledSchema(std::move(cached));
        }
    }

    auto checked = check_document(schema, options, detail::SchemaDialect::eDraft202012, std::string());
    ValueShape schema_shape = checked.shape;
    schema_shape.nodes =
        saturating_add(schema_shape.nodes, check_documents(documents, options, checked.dialect));

    // Patterns that cannot be compiled are collected instead of ending the compile, so that any
    // other problem the schema has is found and reported first. One stands in as a pattern that
    // matches nothing until then.
    const detail::PatternBudget patterns(options.max_pattern_instructions);
    auto native = detail::NativeSchema::compile(schema, documents);
    if (native.has_references()) {
        // A reference cycle that never descends into the value exceeds the evaluation bounds
        // whatever the value is. Trying a few trivial values finds it now instead of on every
        // validation.
        ValueShape trivial;
        trivial.nodes = 1;
        trivial.levels = 1;
        for (const auto& probe : {
                 nlohmann::json(),
                 nlohmann::json::object(),
                 nlohmann::json::array(),
                 nlohmann::json(""),
                 nlohmann::json(0),
                 nlohmann::json(true),
             }) {
            if (const auto reason = native.stop_reason(
                    probe, trivial.pattern_steps(options),
                    trivial.evaluation_work(schema_shape.nodes, options.max_evaluation_work))) {
                throw SchemaError(
                    SchemaError::Category::eInvalidSchema,
                    describe("evaluating the JSON Schema against a trivial value: ", *reason));
            }
        }
    }
    if (patterns.problem()) {
        throw pattern_error(*patterns.problem());
    }
    auto result = std::make_shared<const CompiledSchemaImpl>(
        CompiledSchemaImpl{.source = schema,
                           .options = options,
                           .schema_nodes = schema_shape.nodes,
                           .native = std::move(native)});
    if (use_cache && hash) {
        compiled_schema_cache().insert(*hash, result);
    }
    return CompiledSchema(std::move(result));
}

ValidationResult CompiledSchema::validate(const nlohmann::json& instance) const {
    ValidationResult result;
    const auto& options = impl_->options;
    const auto shape = measure(instance, options.max_instance_depth);
    if (shape.too_deep) {
        result.valid = false;
        result.violations.push_back(
            {.instance_path = "",
             .message =
                 "value nests deeper than " + std::to_string(options.max_instance_depth) + " levels"});
        return result;
    }

    impl_->native.validate(instance, options.max_reported_violations, shape.pattern_steps(options),
                           shape.evaluation_work(impl_->schema_nodes, options.max_evaluation_work),
                           result);
    return result;
}

const nlohmann::json& CompiledSchema::schema() const noexcept { return impl_->source; }

namespace detail {

std::size_t compiled_schema_cache_live_entries() { return compiled_schema_cache().live_entries(); }

}  // namespace detail

}  // namespace mcp
