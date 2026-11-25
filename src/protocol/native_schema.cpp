#include "native_schema.hpp"

#include "mcp/detail/constants.hpp"
#include "mcp/detail/protocol/schema_uri.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"
#include "mcp/detail/text/encoding.hpp"
#include "schema_checker.hpp"
#include "schema_dialect.hpp"
#include "schema_values.hpp"

#include "mcp/detail/text/regex.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The evaluation recurses through a few functions for every subschema it enters. Message building
// stays out of them, in functions that are not inlined, so that their frames stay small in every
// build and the deepest evaluation fits a small thread stack.
#ifdef _MSC_VER
#define MCP_SCHEMA_NOINLINE __declspec(noinline)
#else
#define MCP_SCHEMA_NOINLINE __attribute__((noinline))
#endif

namespace mcp::detail {

struct NativeSchema::Resource {
    std::string uri;  ///< Absolute, without a fragment.
    SchemaDialect dialect = SchemaDialect::eDraft202012;
    /// The resource's root when it sets `$recursiveAnchor` to true (2019-09).
    const Node* recursive_root = nullptr;
    /// `$dynamicAnchor` names of the resource and the schemas that declare them.
    std::map<std::string, const Node*, std::less<>> dynamic_anchors;
};

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
    std::vector<const Node*> prefix_items;
    const Node* items = nullptr;
    const Node* contains = nullptr;
    std::optional<std::size_t> min_contains;
    std::optional<std::size_t> max_contains;
    bool unique_items = false;
    std::optional<std::size_t> min_items;
    std::optional<std::size_t> max_items;
    std::vector<std::string> property_names;  ///< Sorted; property_schemas is in the same order.
    std::vector<const Node*> property_schemas;
    std::vector<Regex> pattern_regexes;  ///< `patternProperties`; pattern_schemas in the same order.
    std::vector<const Node*> pattern_schemas;
    const Node* additional_properties = nullptr;
    const Node* property_names_schema = nullptr;
    std::vector<std::string> required;
    std::vector<std::string> dependent_required_names;  ///< Same order as dependent_required_lists.
    std::vector<std::vector<std::string>> dependent_required_lists;
    std::vector<std::string> dependent_schema_names;  ///< Same order as dependent_schemas.
    std::vector<const Node*> dependent_schemas;
    std::optional<std::size_t> min_properties;
    std::optional<std::size_t> max_properties;
    std::vector<const Node*> all_of;
    std::vector<const Node*> any_of;
    std::vector<const Node*> one_of;
    const Node* not_schema = nullptr;
    const Node* if_schema = nullptr;
    const Node* then_schema = nullptr;
    const Node* else_schema = nullptr;
    const Node* ref = nullptr;  ///< `$ref`'s target.
    /// `$dynamicRef`'s target when no resource in the dynamic scope declares dynamic_name.
    const Node* dynamic_ref = nullptr;
    /// The `$dynamicAnchor` name `$dynamicRef` looks for in the dynamic scope; empty when its
    /// target declares no such anchor, so that it works as `$ref`.
    std::string dynamic_name;
    const Node* unevaluated_properties = nullptr;
    const Node* unevaluated_items = nullptr;
    /// `$recursiveRef`'s target before the dynamic scope is consulted (2019-09).
    const Node* recursive_ref = nullptr;
    /// `contentEncoding` and `contentMediaType`, asserted in draft-07 and earlier.
    std::optional<std::string> content_encoding;
    std::optional<std::string> content_media_type;
    /// Set for the target of a reference to a dialect's metaschema: the value must be a schema of
    /// that dialect.
    std::optional<SchemaDialect> metaschema;
    const Resource* resource = nullptr;  ///< The schema resource the schema belongs to.
    bool has_applicators = false;        ///< Any of all_of to dynamic_ref is set.
};

/// The schemas of one compiled document, which point at each other.
struct NativeSchema::Document {
    std::deque<Node> nodes;  ///< A deque, so that a node never moves once added.
    std::deque<Resource> resources;
    const Node* root = nullptr;
    bool references = false;     ///< The document uses `$ref` or `$dynamicRef`.
    bool dynamic_scope = false;  ///< A `$dynamicRef` looks for a `$dynamicAnchor` when evaluated.
};

namespace {

using Node = NativeSchema::Node;
using Resource = NativeSchema::Resource;
using Document = NativeSchema::Document;

/// Whether @p keyword is a keyword of @p dialect; any other is ignored there, as an annotation or
/// an unknown keyword. `$schema` and the identifier keywords are read before the others.
bool has_keyword(std::string_view keyword, SchemaDialect dialect) {
    struct Span {
        std::string_view keyword;
        SchemaDialect first;
        SchemaDialect last;
    };
    using D = SchemaDialect;
    static constexpr std::array<Span, 20> s_spans = {
        {
            {.keyword = "$anchor", .first = D::eDraft201909, .last = D::eDraft202012},
            {.keyword = "$defs", .first = D::eDraft201909, .last = D::eDraft202012},
            {.keyword = "$dynamicAnchor", .first = D::eDraft202012, .last = D::eDraft202012},
            {.keyword = "$dynamicRef", .first = D::eDraft202012, .last = D::eDraft202012},
            {.keyword = "$recursiveAnchor", .first = D::eDraft201909, .last = D::eDraft201909},
            {.keyword = "$recursiveRef", .first = D::eDraft201909, .last = D::eDraft201909},
            {.keyword = "additionalItems", .first = D::eDraft4, .last = D::eDraft201909},
            {.keyword = "const", .first = D::eDraft6, .last = D::eDraft202012},
            {.keyword = "contains", .first = D::eDraft6, .last = D::eDraft202012},
            {.keyword = "contentEncoding", .first = D::eDraft4, .last = D::eDraft7},
            {.keyword = "contentMediaType", .first = D::eDraft4, .last = D::eDraft7},
            {.keyword = "dependencies", .first = D::eDraft4, .last = D::eDraft7},
            {.keyword = "dependentRequired", .first = D::eDraft201909, .last = D::eDraft202012},
            {.keyword = "dependentSchemas", .first = D::eDraft201909, .last = D::eDraft202012},
            {.keyword = "if", .first = D::eDraft7, .last = D::eDraft202012},
            {.keyword = "maxContains", .first = D::eDraft201909, .last = D::eDraft202012},
            {.keyword = "minContains", .first = D::eDraft201909, .last = D::eDraft202012},
            {.keyword = "prefixItems", .first = D::eDraft202012, .last = D::eDraft202012},
            {.keyword = "propertyNames", .first = D::eDraft6, .last = D::eDraft202012},
            {.keyword = "unevaluatedItems", .first = D::eDraft201909, .last = D::eDraft202012},
        },
    };
    if (keyword == "then" || keyword == "else") {
        return dialect >= D::eDraft7;
    }
    if (keyword == "unevaluatedProperties") {
        return dialect >= D::eDraft201909;
    }
    for (const auto& span : s_spans) {
        if (span.keyword == keyword) {
            return dialect >= span.first && dialect <= span.last;
        }
    }
    return true;
}

/// Whether @p value has a `$ref` or `$dynamicRef` member at any depth.
bool mentions_reference(const nlohmann::json& value) {
    if (value.is_array()) {
        return std::ranges::any_of(value.get_ref<const nlohmann::json::array_t&>(),
                                   [](const nlohmann::json& item) { return mentions_reference(item); });
    }
    if (!value.is_object()) {
        return false;
    }
    return std::ranges::any_of(value.get_ref<const nlohmann::json::object_t&>(), [](const auto& entry) {
        const auto& [name, member] = entry;
        return name == "$ref" || name == "$dynamicRef" || name == "$recursiveRef" ||
               mentions_reference(member);
    });
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

/// @p uri as the schema author wrote it when it is relative to the document without `$id`.
std::string_view without_default_base(std::string_view uri) {
    const auto base = mcp::detail::constants::schema::g_DEFAULT_BASE_URI;
    if (uri.starts_with(base)) {
        uri.remove_prefix(base.size());
        if (!uri.empty() && uri.front() == '/') {
            uri.remove_prefix(1);
        }
    }
    return uri;
}

/// Compiles a schema document, and the registered documents its references reach, into a
/// Document. compile() returns false for a schema it cannot compile, and problem() says why.
class Compiler {
   public:
    Compiler(Document& document, const SchemaDocuments& documents)
        : document_(document), documents_(documents) {}

    bool compile(const nlohmann::json& schema) {
        // Locations and anchors matter only to references; a document without any skips them.
        track_ = mentions_reference(schema);
        document_.references = track_;
        root_dialect_ = declared_dialect(schema).value_or(SchemaDialect::eDraft202012);
        Resource* resource = add_resource(
            std::string(mcp::detail::constants::schema::g_DEFAULT_BASE_URI), schema, root_dialect_);
        const Node* root = compile_node(schema, *resource, std::string(), true);
        if (root == nullptr) {
            return false;
        }
        document_.root = root;
        return resolve_references();
    }

    /// Why compile() returned false: the first problem recorded, or a generic one.
    [[nodiscard]] SchemaError problem() const {
        return problem_.value_or(
            SchemaError(SchemaError::Category::eInvalidSchema, "JSON Schema cannot be compiled"));
    }

   private:
    /// Where a compiled schema is: its resource and its JSON Pointer within it.
    struct Place {
        Resource* resource;
        std::string pointer;
    };

    struct ResourceRoot {
        const nlohmann::json* value;
        Resource* resource;
    };

    enum class ReferenceKind : unsigned char {
        eRef,
        eDynamic,
        eRecursive,
    };

    struct PendingReference {
        Node* node;
        std::string target;  ///< Absolute, with the fragment as written.
        ReferenceKind kind;
    };

    /// What compile_into() reads after the other keywords of a schema.
    struct Deferred {
        const nlohmann::json* items = nullptr;
        const nlohmann::json* additional_items = nullptr;
        bool exclusive_minimum = false;  ///< draft-04's boolean form.
        bool exclusive_maximum = false;
    };

    /// The dialect @p schema's `$schema` names: nothing when it has none or names none.
    static std::optional<SchemaDialect> declared_dialect(const nlohmann::json& schema) {
        if (!schema.is_object()) {
            return std::nullopt;
        }
        const auto declared = schema.find("$schema");
        if (declared == schema.end() || !declared->is_string()) {
            return std::nullopt;
        }
        return find_schema_dialect(declared->get_ref<const std::string&>());
    }

    /// A new resource at @p uri whose root is @p value; null when another has that URI.
    Resource* add_resource(std::string uri, const nlohmann::json& value, SchemaDialect dialect) {
        if (roots_.contains(uri)) {
            return nullptr;
        }
        auto& resource = document_.resources.emplace_back();
        resource.uri = uri;
        resource.dialect = dialect;
        roots_.emplace(std::move(uri), ResourceRoot{.value = &value, .resource = &resource});
        return &resource;
    }

    std::string child(const std::string& pointer, std::string_view token) const {
        if (!track_) {
            return {};
        }
        std::string result = pointer;
        result += '/';
        result += escape_pointer_token(token);
        return result;
    }

    Node* compile_node(const nlohmann::json& schema, Resource& resource, std::string pointer,
                       bool document_root) {
        Node& node = document_.nodes.emplace_back();
        if (!compile_into(schema, resource, std::move(pointer), document_root, node)) {
            return nullptr;
        }
        return &node;
    }

    bool compile_schema(const nlohmann::json& value, Resource& resource, std::string pointer,
                        const Node*& target) {
        target = compile_node(value, resource, std::move(pointer), false);
        return target != nullptr;
    }

    /// Compiles the non-empty array of schemas @p value into @p nodes; false when it is not one.
    bool compile_list(const nlohmann::json& value, Resource& resource, const std::string& pointer,
                      std::vector<const Node*>& nodes) {
        if (!value.is_array() || value.empty()) {
            return false;
        }
        nodes.reserve(value.size());
        for (std::size_t index = 0; index < value.size(); ++index) {
            const Node* node =
                compile_node(value[index], resource, child(pointer, std::to_string(index)), false);
            if (node == nullptr) {
                return false;
            }
            nodes.push_back(node);
        }
        return true;
    }

    /// Compiles the object of schemas @p value into @p names (in key order) and @p nodes.
    bool compile_map(const nlohmann::json& value, Resource& resource, const std::string& pointer,
                     std::vector<std::string>& names, std::vector<const Node*>& nodes) {
        if (!value.is_object()) {
            return false;
        }
        // nlohmann objects iterate in key order, so the names come out sorted.
        for (const auto& [name, item] : value.items()) {
            const Node* node = compile_node(item, resource, child(pointer, name), false);
            if (node == nullptr) {
                return false;
            }
            names.push_back(name);
            nodes.push_back(node);
        }
        return true;
    }

    static bool compile_number(const nlohmann::json& value, std::optional<nlohmann::json>& target) {
        if (!value.is_number()) {
            return false;
        }
        target = value;
        return true;
    }

    static bool compile_size(const nlohmann::json& value, std::optional<std::size_t>& target) {
        target = size_value(value);
        return target.has_value();
    }

    void record(const std::string& key, Node& node, Resource& resource, const std::string& pointer) {
        locations_.emplace(key, &node);
        places_.emplace(&node, Place{.resource = &resource, .pointer = pointer});
    }

    /// Compiles @p schema, at @p pointer in @p outer, into @p node; false when it is not supported.
    bool compile_into(const nlohmann::json& schema, Resource& outer, std::string pointer,
                      bool document_root, Node& node) {
        Resource* resource = &outer;
        bool reference_only = false;
        if (schema.is_object()) {
            auto dialect = outer.dialect;
            const auto declared = schema.find("$schema");
            if (declared != schema.end()) {
                if (!declared->is_string()) {
                    return invalid_keyword("$schema");
                }
                const auto named = declared_dialect(schema);
                if (!named) {
                    return fail(SchemaError::Category::eUnsupportedDialect,
                                "unsupported JSON Schema dialect: " +
                                    sanitize_for_diagnostics(declared->get_ref<const std::string&>()));
                }
                dialect = *named;
            }
            // In draft-07 and earlier a schema with $ref is that reference alone.
            reference_only = dialect <= SchemaDialect::eDraft7 && schema.contains("$ref");
            const auto id = reference_only
                                ? schema.end()
                                : schema.find(dialect == SchemaDialect::eDraft4 ? "id" : "$id");
            if (id != schema.end()) {
                if (!id->is_string()) {
                    return invalid_keyword(id.key());
                }
                auto [uri, fragment] =
                    split_fragment(resolve_uri(outer.uri, id->get_ref<const std::string&>()));
                if (!fragment.empty() && dialect >= SchemaDialect::eDraft201909) {
                    return invalid_keyword(id.key());
                }
                if (uri != outer.uri) {
                    if (track_) {
                        locations_.emplace(outer.uri + "#" + pointer, &node);
                    }
                    resource = add_resource(uri, schema, dialect);
                    if (resource == nullptr) {
                        return fail(SchemaError::Category::eInvalidSchema,
                                    "JSON Schema declares the resource " +
                                        sanitize_for_diagnostics(without_default_base(uri)) + " twice");
                    }
                    pointer.clear();
                }
                // Before 2019-09, "$id": "#name" names the schema, as $anchor does since.
                if (!fragment.empty() &&
                    !anchors_.emplace(resource->uri + "#" + fragment, &node).second) {
                    return duplicate_anchor(fragment);
                }
            }
            // $schema changes the dialect only at the root of a resource.
            if (declared != schema.end() && resource == &outer && !document_root &&
                dialect != outer.dialect) {
                return fail(SchemaError::Category::eInvalidSchema,
                            "JSON Schema changes its dialect outside the root of a resource");
            }
        }
        node.resource = resource;
        if (track_) {
            record(resource->uri + "#" + pointer, node, *resource, pointer);
        }
        if (schema.is_boolean()) {
            node.reject_all = !schema.get<bool>();
            return true;
        }
        if (!schema.is_object()) {
            return false;
        }
        Deferred deferred;
        for (const auto& [keyword, value] : schema.items()) {
            if (reference_only && keyword != "$ref" && keyword != "definitions") {
                continue;
            }
            if (!compile_keyword(keyword, value, *resource, pointer, node, deferred)) {
                return invalid_keyword(keyword);
            }
        }
        if (!compile_deferred(deferred, *resource, pointer, node)) {
            return false;
        }
        if (resource != &outer || document_root) {
            const auto recursive = schema.find("$recursiveAnchor");
            if (recursive != schema.end() && resource->dialect == SchemaDialect::eDraft201909) {
                if (!recursive->is_boolean()) {
                    return invalid_keyword("$recursiveAnchor");
                }
                if (recursive->get<bool>()) {
                    resource->recursive_root = &node;
                }
            }
        }
        node.has_applicators = !node.all_of.empty() || !node.any_of.empty() || !node.one_of.empty() ||
                               node.not_schema != nullptr || node.if_schema != nullptr;
        return true;
    }

    /// The keywords that depend on others of the schema: draft-04's boolean exclusive bounds, and
    /// `items` with `additionalItems` before 2020-12.
    bool compile_deferred(const Deferred& deferred, Resource& resource, const std::string& pointer,
                          Node& node) {
        if (deferred.exclusive_minimum && node.minimum) {
            node.exclusive_minimum = std::move(node.minimum);
            node.minimum.reset();
        }
        if (deferred.exclusive_maximum && node.maximum) {
            node.exclusive_maximum = std::move(node.maximum);
            node.maximum.reset();
        }
        if (deferred.items != nullptr) {
            // An array of items is 2020-12's prefixItems; 2020-12 itself has no such form.
            const bool compiled =
                deferred.items->is_array()
                    ? resource.dialect < SchemaDialect::eDraft202012 &&
                          compile_list(*deferred.items, resource, child(pointer, "items"),
                                       node.prefix_items)
                    : compile_schema(*deferred.items, resource, child(pointer, "items"), node.items);
            if (!compiled) {
                return invalid_keyword("items");
            }
        }
        if (deferred.additional_items == nullptr) {
            return true;
        }
        // After an array of items, additionalItems is 2020-12's items; otherwise it applies to
        // nothing.
        const auto here = child(pointer, "additionalItems");
        const bool compiled =
            deferred.items != nullptr && deferred.items->is_array()
                ? compile_schema(*deferred.additional_items, resource, here, node.items)
                : compile_node(*deferred.additional_items, resource, here, false) != nullptr;
        return compiled || invalid_keyword("additionalItems");
    }

    bool compile_keyword(const std::string& keyword, const nlohmann::json& value, Resource& resource,
                         const std::string& pointer, Node& node, Deferred& deferred) {
        if (keyword == "$schema" || keyword == "$id" || keyword == "id" ||
            keyword == "$recursiveAnchor" || !has_keyword(keyword, resource.dialect)) {
            return true;  // compile_into() reads the first ones; the others are not keywords here
        }
        if (keyword == "$ref" || keyword == "$dynamicRef" || keyword == "$recursiveRef") {
            if (!value.is_string()) {
                return false;
            }
            auto kind = ReferenceKind::eRecursive;
            if (keyword == "$ref") {
                kind = ReferenceKind::eRef;
            } else if (keyword == "$dynamicRef") {
                kind = ReferenceKind::eDynamic;
            }
            pending_.push_back({
                .node = &node,
                .target = resolve_uri(resource.uri, value.get_ref<const std::string&>()),
                .kind = kind,
            });
            return true;
        }
        if (keyword == "$anchor" || keyword == "$dynamicAnchor") {
            if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
                return false;
            }
            const auto& name = value.get_ref<const std::string&>();
            if (keyword == "$dynamicAnchor" && !resource.dynamic_anchors.emplace(name, &node).second) {
                return duplicate_anchor(name);
            }
            return anchors_.emplace(resource.uri + "#" + name, &node).second || duplicate_anchor(name);
        }
        if (keyword == "$defs" || keyword == "definitions") {
            if (!value.is_object()) {
                return false;
            }
            const auto defs = child(pointer, keyword);
            for (const auto& [name, item] : value.items()) {
                if (compile_node(item, resource, child(defs, name), false) == nullptr) {
                    return false;
                }
            }
            return true;
        }
        const auto here = child(pointer, keyword);
        if (keyword == "type") {
            if (value.is_string()) {
                node.types.push_back(value.get<std::string>());
            } else if (auto types = string_list(value)) {
                node.types = std::move(*types);
            } else {
                return false;
            }
            return true;
        }
        if (keyword == "enum") {
            node.enum_values = value;
            return value.is_array();
        }
        if (keyword == "const") {
            node.const_value = value;
            return true;
        }
        if (keyword == "minimum") {
            return compile_number(value, node.minimum);
        }
        if (keyword == "maximum") {
            return compile_number(value, node.maximum);
        }
        if (keyword == "exclusiveMinimum" || keyword == "exclusiveMaximum") {
            if (resource.dialect == SchemaDialect::eDraft4) {
                // A boolean that makes minimum or maximum exclusive.
                if (!value.is_boolean()) {
                    return false;
                }
                (keyword == "exclusiveMinimum" ? deferred.exclusive_minimum
                                               : deferred.exclusive_maximum) = value.get<bool>();
                return true;
            }
            return compile_number(
                value, keyword == "exclusiveMinimum" ? node.exclusive_minimum : node.exclusive_maximum);
        }
        if (keyword == "contentEncoding" || keyword == "contentMediaType") {
            if (!value.is_string()) {
                return false;
            }
            (keyword == "contentEncoding" ? node.content_encoding : node.content_media_type) =
                value.get<std::string>();
            return true;
        }
        if (keyword == "dependencies") {
            if (!value.is_object()) {
                return false;
            }
            const auto here = child(pointer, keyword);
            for (const auto& [name, dependency] : value.items()) {
                if (dependency.is_array()) {
                    auto names = string_list(dependency);
                    if (!names) {
                        return false;
                    }
                    node.dependent_required_names.push_back(name);
                    node.dependent_required_lists.push_back(std::move(*names));
                    continue;
                }
                const Node* schema = compile_node(dependency, resource, child(here, name), false);
                if (schema == nullptr) {
                    return false;
                }
                node.dependent_schema_names.push_back(name);
                node.dependent_schemas.push_back(schema);
            }
            return true;
        }
        if (keyword == "multipleOf") {
            if (!value.is_number() || !(value.get<double>() > 0)) {
                return false;
            }
            node.multiple_of = value.get<double>();
            return true;
        }
        if (keyword == "minLength") {
            return compile_size(value, node.min_length);
        }
        if (keyword == "maxLength") {
            return compile_size(value, node.max_length);
        }
        if (keyword == "pattern") {
            if (!value.is_string()) {
                return false;
            }
            node.pattern.emplace(PatternBudget::compile(value.get_ref<const std::string&>()));
            return true;
        }
        if (keyword == "prefixItems") {
            return compile_list(value, resource, here, node.prefix_items);
        }
        if (keyword == "items") {
            deferred.items = &value;
            return true;
        }
        if (keyword == "additionalItems") {
            deferred.additional_items = &value;
            return true;
        }
        if (keyword == "contains") {
            return compile_schema(value, resource, here, node.contains);
        }
        if (keyword == "minContains") {
            return compile_size(value, node.min_contains);
        }
        if (keyword == "maxContains") {
            return compile_size(value, node.max_contains);
        }
        if (keyword == "uniqueItems") {
            node.unique_items = value.is_boolean() && value.get<bool>();
            return value.is_boolean();
        }
        if (keyword == "minItems") {
            return compile_size(value, node.min_items);
        }
        if (keyword == "maxItems") {
            return compile_size(value, node.max_items);
        }
        if (keyword == "properties") {
            return compile_map(value, resource, here, node.property_names, node.property_schemas);
        }
        if (keyword == "patternProperties") {
            std::vector<std::string> patterns;
            if (!compile_map(value, resource, here, patterns, node.pattern_schemas)) {
                return false;
            }
            for (const auto& pattern : patterns) {
                node.pattern_regexes.push_back(PatternBudget::compile(pattern));
            }
            return true;
        }
        if (keyword == "additionalProperties") {
            return compile_schema(value, resource, here, node.additional_properties);
        }
        if (keyword == "propertyNames") {
            return compile_schema(value, resource, here, node.property_names_schema);
        }
        if (keyword == "required") {
            auto names = string_list(value);
            if (!names) {
                return false;
            }
            node.required = std::move(*names);
            return true;
        }
        if (keyword == "dependentRequired") {
            if (!value.is_object()) {
                return false;
            }
            for (const auto& [name, list] : value.items()) {
                auto names = string_list(list);
                if (!names) {
                    return false;
                }
                node.dependent_required_names.push_back(name);
                node.dependent_required_lists.push_back(std::move(*names));
            }
            return true;
        }
        if (keyword == "dependentSchemas") {
            return compile_map(value, resource, here, node.dependent_schema_names,
                               node.dependent_schemas);
        }
        if (keyword == "minProperties") {
            return compile_size(value, node.min_properties);
        }
        if (keyword == "maxProperties") {
            return compile_size(value, node.max_properties);
        }
        if (keyword == "allOf") {
            return compile_list(value, resource, here, node.all_of);
        }
        if (keyword == "anyOf") {
            return compile_list(value, resource, here, node.any_of);
        }
        if (keyword == "oneOf") {
            return compile_list(value, resource, here, node.one_of);
        }
        if (keyword == "not") {
            return compile_schema(value, resource, here, node.not_schema);
        }
        if (keyword == "if") {
            return compile_schema(value, resource, here, node.if_schema);
        }
        if (keyword == "then") {
            return compile_schema(value, resource, here, node.then_schema);
        }
        if (keyword == "else") {
            return compile_schema(value, resource, here, node.else_schema);
        }
        if (keyword == "unevaluatedProperties") {
            return compile_schema(value, resource, here, node.unevaluated_properties);
        }
        if (keyword == "unevaluatedItems") {
            return compile_schema(value, resource, here, node.unevaluated_items);
        }
        // Anything else is an annotation, such as `format` and, from 2019-09 on,
        // `contentEncoding` and `contentMediaType`.
        return true;
    }

    /// Points every `$ref` and `$dynamicRef` at its target. Resolving one may compile more of a
    /// document, which may add references; each location compiles once, so this ends.
    bool resolve_references() {
        while (!pending_.empty()) {
            const auto reference = std::move(pending_.back());
            pending_.pop_back();
            const auto [base, written] = split_fragment(reference.target);
            const auto fragment = percent_decode(written);
            const Node* target = fragment ? locate(base, *fragment) : nullptr;
            if (target == nullptr) {
                return fail(SchemaError::Category::eUnresolvedRef,
                            "JSON Schema reference cannot be resolved in the document: " +
                                sanitize_for_diagnostics(without_default_base(reference.target)));
            }
            Node& node = *reference.node;
            node.has_applicators = true;
            if (reference.kind == ReferenceKind::eRef) {
                node.ref = target;
                continue;
            }
            if (reference.kind == ReferenceKind::eRecursive) {
                node.recursive_ref = target;
                if (target->resource->recursive_root == target) {
                    document_.dynamic_scope = true;
                }
                continue;
            }
            node.dynamic_ref = target;
            // A $dynamicRef looks through the dynamic scope only when it lands on a $dynamicAnchor.
            if (!fragment->empty() && fragment->front() != '/') {
                const auto& anchors = target->resource->dynamic_anchors;
                const auto anchor = anchors.find(*fragment);
                if (anchor != anchors.end() && anchor->second == target) {
                    node.dynamic_name = *fragment;
                    document_.dynamic_scope = true;
                }
            }
        }
        return true;
    }

    /// The schema at @p fragment (decoded) of the resource @p base, compiled if nothing reached it
    /// yet; null when there is none or it cannot be compiled. A registered document is compiled the
    /// first time a reference reaches it; no other document is ever looked for.
    const Node* locate(const std::string& base, const std::string& fragment) {
        auto root = roots_.find(base);
        if (root == roots_.end()) {
            const auto registered = documents_.find(base);
            if (registered == documents_.end()) {
                const auto dialect = find_schema_dialect(base);
                return dialect && fragment.empty() ? metaschema_node(*dialect) : nullptr;
            }
            Resource* resource = add_resource(
                base, registered->second, declared_dialect(registered->second).value_or(root_dialect_));
            if (resource == nullptr ||
                compile_node(registered->second, *resource, std::string(), true) == nullptr) {
                return nullptr;
            }
            root = roots_.find(base);
        }
        if (!fragment.empty() && fragment.front() != '/') {
            const auto anchor = anchors_.find(base + "#" + fragment);
            return anchor == anchors_.end() ? nullptr : anchor->second;
        }
        const auto key = base + "#" + fragment;
        const auto compiled = locations_.find(key);
        if (compiled != locations_.end()) {
            return compiled->second;
        }
        const auto tokens = json_pointer_tokens(fragment);
        if (!tokens) {
            return nullptr;
        }
        // Walk to the target. A schema compiled on the way sets the resource the target is in.
        const nlohmann::json* value = root->second.value;
        Resource* resource = root->second.resource;
        std::string pointer;
        std::string walked;
        for (const auto& token : *tokens) {
            if (value->is_object()) {
                const auto member = value->find(token);
                if (member == value->end()) {
                    return nullptr;
                }
                value = &*member;
            } else if (value->is_array()) {
                const auto index = array_index(token);
                if (!index || *index >= value->size()) {
                    return nullptr;
                }
                value = &(*value)[*index];
            } else {
                return nullptr;
            }
            walked += '/';
            walked += escape_pointer_token(token);
            pointer += '/';
            pointer += escape_pointer_token(token);
            std::string location = base;
            location += '#';
            location += walked;
            const auto on_the_way = locations_.find(location);
            if (on_the_way != locations_.end()) {
                const auto& place = places_.at(on_the_way->second);
                resource = place.resource;
                pointer = place.pointer;
            }
        }
        if (!value->is_object() && !value->is_boolean()) {
            return nullptr;
        }
        Node* node = compile_node(*value, *resource, pointer, false);
        if (node != nullptr) {
            locations_.emplace(key, node);
        }
        return node;
    }

    /// The schema a reference to the metaschema of @p dialect reaches, made once per dialect.
    const Node* metaschema_node(SchemaDialect dialect) {
        auto& node = metaschemas_[static_cast<std::size_t>(dialect)];
        if (node == nullptr) {
            auto& resource = document_.resources.emplace_back();
            resource.uri = std::string(dialect_uri(dialect));
            resource.dialect = dialect;
            node = &document_.nodes.emplace_back();
            node->metaschema = dialect;
            node->resource = &resource;
        }
        return node;
    }

    /// Records @p category and @p message as the problem unless one is recorded; returns false.
    bool fail(SchemaError::Category category, const std::string& message) {
        if (!problem_) {
            problem_.emplace(category, message);
        }
        return false;
    }

    bool duplicate_anchor(const std::string& name) {
        return fail(SchemaError::Category::eInvalidSchema,
                    "JSON Schema declares the anchor '" + sanitize_for_diagnostics(name) + "' twice");
    }

    bool invalid_keyword(const std::string& keyword) {
        return fail(
            SchemaError::Category::eInvalidSchema,
            "JSON Schema keyword '" + sanitize_for_diagnostics(keyword) + "' cannot be compiled");
    }

    /// The array index @p token names: decimal digits without a leading zero.
    static std::optional<std::size_t> array_index(const std::string& token) {
        constexpr std::size_t g_most_digits = 18;
        if (token.empty() || token.size() > g_most_digits ||
            (token.size() > 1 && token.front() == '0') ||
            !std::ranges::all_of(token, [](char digit) { return digit >= '0' && digit <= '9'; })) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(std::stoull(token));
    }

    Document& document_;
    const SchemaDocuments& documents_;
    bool track_ = false;
    SchemaDialect root_dialect_ = SchemaDialect::eDraft202012;
    std::map<std::string, ResourceRoot, std::less<>> roots_;
    std::unordered_map<std::string, Node*> locations_;
    std::unordered_map<const Node*, Place> places_;
    std::unordered_map<std::string, const Node*> anchors_;
    std::vector<PendingReference> pending_;
    static constexpr std::size_t dialect_count =
        static_cast<std::size_t>(SchemaDialect::eDraft202012) + 1;
    std::array<Node*, dialect_count> metaschemas_{};  ///< By SchemaDialect.
    std::optional<SchemaError> problem_;
};

/// Whether @p value is a multiple of @p divisor, decided in doubles, with zero a multiple of
/// everything and a quotient that overflows a multiple of nothing.
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

/// Thrown when pattern matching needs more steps than the validation's budget holds. Not a
/// std::exception, as WorkBudgetExceeded is not.
struct PatternBudgetExceeded {};

/// Work charged for reading a whole string: one unit per this many bytes.
constexpr std::size_t g_STRING_BYTES_PER_UNIT = 16;

/// Work charged for a property name looked up or added to a location: one unit, and one more per
/// this many bytes of the name.
constexpr std::size_t g_NAME_BYTES_PER_UNIT = 64;

/// Work charged for checking `contentEncoding` or `contentMediaType`: one unit, and one more per
/// this many bytes of the string.
constexpr std::size_t g_CONTENT_BYTES_PER_UNIT = 4;

/// @p text decoded as base64: the characters before the first '=' must all be base64 characters,
/// and what follows it is ignored. Nothing when one is not.
std::optional<std::string> lenient_base64_decode(std::string_view text) {
    namespace encoding = mcp::detail::constants::encoding;
    std::string decoded;
    unsigned buffer = 0;
    unsigned bits = 0;
    for (const char character : text.substr(0, text.find('='))) {
        const int digit = encoding::g_BASE64_VALUES[static_cast<unsigned char>(character)];
        if (digit < 0) {
            return std::nullopt;
        }
        buffer = (buffer << encoding::g_BASE64_DIGIT_BITS) | static_cast<unsigned>(digit);
        bits += encoding::g_BASE64_DIGIT_BITS;
        if (bits >= encoding::g_BITS_PER_BYTE) {
            bits -= encoding::g_BITS_PER_BYTE;
            decoded.push_back(static_cast<char>((buffer >> bits) & encoding::g_BYTE_MASK));
        }
    }
    return decoded;
}

/// The members and items of a value that schemas evaluated at its location, which
/// `unevaluatedProperties` and `unevaluatedItems` leave alone.
struct Annotations {
    std::vector<char> members;     ///< By member index, in the object's order; empty when none.
    std::size_t items = 0;         ///< Items [0, items) were evaluated.
    std::vector<char> item_flags;  ///< By item index, items `contains` matched; empty when none.

    void member(std::size_t index, std::size_t count) {
        if (members.empty()) {
            members.assign(count, 0);
        }
        members[index] = 1;
    }

    void item(std::size_t index, std::size_t count) {
        if (item_flags.empty()) {
            item_flags.assign(count, 0);
        }
        item_flags[index] = 1;
    }

    [[nodiscard]] bool has_member(std::size_t index) const {
        return index < members.size() && members[index] != 0;
    }

    [[nodiscard]] bool has_item(std::size_t index) const {
        return index < items || (index < item_flags.size() && item_flags[index] != 0);
    }

    void merge(const Annotations& other) {
        if (members.size() < other.members.size()) {
            members.resize(other.members.size(), 0);
        }
        for (std::size_t i = 0; i < other.members.size(); ++i) {
            members[i] = static_cast<char>(members[i] != 0 || other.members[i] != 0);
        }
        items = std::max(items, other.items);
        if (item_flags.size() < other.item_flags.size()) {
            item_flags.resize(other.item_flags.size(), 0);
        }
        for (std::size_t i = 0; i < other.item_flags.size(); ++i) {
            item_flags[i] = static_cast<char>(item_flags[i] != 0 || other.item_flags[i] != 0);
        }
    }
};

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
    Evaluation(const Document& document, std::size_t max_violations, std::size_t& pattern_steps,
               std::size_t& work, ValidationResult& result)
        : root_{.violations = &result.violations, .limit = max_violations},
          sink_(&root_),
          pattern_steps_(pattern_steps),
          work_(work),
          dynamic_scope_(document.dynamic_scope) {}

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
        const ScopeEntry scope(*this, node.resource);
        if (node.unevaluated_properties != nullptr || node.unevaluated_items != nullptr) {
            return check_unevaluated(node, value);
        }
        return check_keywords(node, value);
    }

   private:
    /// Checks every keyword of @p node but `unevaluatedProperties` and `unevaluatedItems`.
    bool check_keywords(const Node& node, const nlohmann::json& value) {
        if (node.metaschema) {
            return check_metaschema(*node.metaschema, value);
        }
        if (!check_value_keywords(node, value)) {
            return false;
        }
        if (value.is_array()) {
            if (!check_array(node, value)) {
                return false;
            }
        } else if (value.is_object() && !check_object(node, value)) {
            return false;
        }

        return !node.has_applicators || check_applicators(node, value);
    }

    /// `contentEncoding` and `contentMediaType` in draft-07 and earlier: base64 is checked and any
    /// other encoding fails; JSON content must parse, after base64 decoding when the content is
    /// base64.
    MCP_SCHEMA_NOINLINE bool check_content(const Node& node, const std::string& text) {
        const bool base64 = node.content_encoding && *node.content_encoding == "base64";
        if (node.content_encoding) {
            charge(work_, 1 + (text.size() / g_CONTENT_BYTES_PER_UNIT));
            if (base64 && !lenient_base64_decode(text) && !report("Content is not a base64 string")) {
                return false;
            }
            if (!base64 && !node.content_encoding->empty() &&
                !report_concat("Content encoding '", *node.content_encoding, "' cannot be checked")) {
                return false;
            }
        }
        if (node.content_media_type) {
            charge(work_, 1 + (text.size() / g_CONTENT_BYTES_PER_UNIT));
            std::optional<std::string> decoded;
            if (base64) {
                decoded = lenient_base64_decode(text);
                if (!decoded) {
                    return true;
                }
            }
            if (*node.content_media_type == "application/json" &&
                !nlohmann::json::accept(decoded ? *decoded : text) && !report("Content is not JSON")) {
                return false;
            }
        }
        return true;
    }

    /// A reference to a dialect's metaschema: @p value must be a schema of @p dialect. The check
    /// nests within the evaluation's depth bound.
    MCP_SCHEMA_NOINLINE bool check_metaschema(SchemaDialect dialect, const nlohmann::json& value) {
        const auto problem = check_schema(value, dialect, work_,
                                          mcp::detail::constants::schema::g_MAX_EVAL_DEPTH - depth_);
        return !problem || report_concat("Value is not a schema of its dialect: ", problem->message,
                                         " at '", problem->path, "'");
    }

    /// The keywords that read only the value itself, kept out of check_keywords(), which the
    /// evaluation recurses through.
    MCP_SCHEMA_NOINLINE bool check_value_keywords(const Node& node, const nlohmann::json& value) {
        if (node.reject_all) {
            return report("No value is allowed here");
        }
        if ((!node.types.empty() &&
             std::ranges::none_of(node.types,
                                  [&](const std::string& type) { return has_type(value, type); })) &&
            (!report_type(node.types, value))) {
            return false;
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
        if (value.is_number()) {
            return check_number(node, value);
        }
        if (value.is_string()) {
            return check_string(node, value.get_ref<const std::string&>());
        }
        return true;
    }

    /// Adds @p resource to the dynamic scope until destroyed, when the document needs a dynamic
    /// scope and @p resource is not already the innermost entry.
    class ScopeEntry {
       public:
        ScopeEntry(Evaluation& evaluation, const Resource* resource)
            : evaluation_(evaluation),
              pushed_(evaluation.dynamic_scope_ &&
                      (evaluation.scope_.empty() || evaluation.scope_.back() != resource)) {
            if (pushed_) {
                evaluation_.scope_.push_back(resource);
            }
        }
        ~ScopeEntry() {
            if (pushed_) {
                evaluation_.scope_.pop_back();
            }
        }
        ScopeEntry(const ScopeEntry&) = delete;
        ScopeEntry& operator=(const ScopeEntry&) = delete;
        ScopeEntry(ScopeEntry&&) = delete;
        ScopeEntry& operator=(ScopeEntry&&) = delete;

       private:
        Evaluation& evaluation_;
        bool pushed_;
    };

    /// `$dynamicRef`'s target: the schema of the outermost resource in the dynamic scope that
    /// declares the anchor it looks for, or its static target.
    const Node& dynamic_target(const Node& node) {
        if (!node.dynamic_name.empty()) {
            for (const Resource* resource : scope_) {
                charge(work_, 1);
                const auto found = resource->dynamic_anchors.find(node.dynamic_name);
                if (found != resource->dynamic_anchors.end()) {
                    return *found->second;
                }
            }
        }
        return *node.dynamic_ref;
    }

    /// `$recursiveRef`'s target: when its static target sets `$recursiveAnchor`, the root of the
    /// outermost resource in the dynamic scope that sets it too.
    const Node& recursive_target(const Node& node) {
        if (node.recursive_ref->resource->recursive_root == node.recursive_ref) {
            for (const Resource* resource : scope_) {
                charge(work_, 1);
                if (resource->recursive_root != nullptr) {
                    return *resource->recursive_root;
                }
            }
        }
        return *node.recursive_ref;
    }

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

    /// One level of evaluation nesting, charged one unit of work more than the levels it is
    /// nested in, so that a deep recursion that branches is stopped after a number of steps that
    /// grows with its depth.
    class Frame {
       public:
        explicit Frame(Evaluation& evaluation) : evaluation_(evaluation) {
            charge(evaluation_.work_, evaluation_.depth_ + 1);
            if (++evaluation_.depth_ > mcp::detail::constants::schema::g_MAX_EVAL_DEPTH) {
                // NOLINTNEXTLINE(bugprone-std-exception-baseclass): see the declaration.
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

    /// Checks @p node, then its `unevaluatedProperties` and `unevaluatedItems` against what the
    /// other keywords and the subschemas evaluated in place left unevaluated.
    bool check_unevaluated(const Node& node, const nlohmann::json& value) {
        Annotations local;
        Annotations* outer = annotations_;
        bool more = false;
        {
            const AnnotationScope scope(*this, &local);
            more = check_keywords(node, value);
        }
        if (more && node.unevaluated_properties != nullptr && value.is_object()) {
            std::size_t index = 0;
            for (const auto& [name, member] : value.get_ref<const nlohmann::json::object_t&>()) {
                if (!local.has_member(index)) {
                    charge(work_, 1 + (name.size() / g_NAME_BYTES_PER_UNIT));
                    if (node.unevaluated_properties->reject_all) {
                        const LocationScope location(*this, Location{.name = &name, .index = 0});
                        more = report_concat("Unevaluated property '", name, "' is not allowed");
                    } else {
                        more = check_member(*node.unevaluated_properties, name, member);
                    }
                    if (!more) {
                        break;
                    }
                    local.member(index, value.size());
                }
                ++index;
            }
        }
        if (more && node.unevaluated_items != nullptr && value.is_array()) {
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (!local.has_item(index)) {
                    charge(work_, 1);
                    if (node.unevaluated_items->reject_all) {
                        const LocationScope location(*this, Location{.name = {}, .index = index});
                        more = report_size("Unevaluated item ", index, " is not allowed");
                    } else {
                        more = check_item(*node.unevaluated_items, value[index], index);
                    }
                    if (!more) {
                        break;
                    }
                }
            }
            local.items = value.size();
        }
        if (outer != nullptr) {
            outer->merge(local);
        }
        return more;
    }

    /// Makes @p annotations where the schemas evaluated at the current location record what they
    /// evaluated, until destroyed; null records nothing.
    class AnnotationScope {
       public:
        AnnotationScope(Evaluation& evaluation, Annotations* annotations)
            : evaluation_(evaluation), outer_(evaluation.annotations_) {
            evaluation_.annotations_ = annotations;
        }
        ~AnnotationScope() { evaluation_.annotations_ = outer_; }
        AnnotationScope(const AnnotationScope&) = delete;
        AnnotationScope& operator=(const AnnotationScope&) = delete;
        AnnotationScope(AnnotationScope&&) = delete;
        AnnotationScope& operator=(AnnotationScope&&) = delete;

       private:
        Evaluation& evaluation_;
        Annotations* outer_;
    };

    /// Whether @p value passes @p node, its violations going to @p buffer (up to @p limit) or, when
    /// @p buffer is null, nowhere, and what it evaluated to @p annotations when not null. The
    /// evaluation stops at the first violation it cannot keep.
    bool passes(const Node& node, const nlohmann::json& value,
                std::vector<SchemaViolation>* buffer = nullptr, std::size_t limit = 0,
                Annotations* annotations = nullptr) {
        Sink sink{.violations = buffer, .limit = limit};
        const SinkScope scope(*this, sink);
        const AnnotationScope annotation_scope(*this, annotations);
        (void)check(node, value);
        return sink.valid;
    }

    /// passes(), merging what @p node evaluated into the current annotations when it passes.
    bool passes_annotated(const Node& node, const nlohmann::json& value,
                          std::vector<SchemaViolation>* buffer = nullptr, std::size_t limit = 0) {
        if (annotations_ == nullptr) {
            return passes(node, value, buffer, limit);
        }
        Annotations branch;
        const bool passed = passes(node, value, buffer, limit, &branch);
        if (passed) {
            annotations_->merge(branch);
        }
        return passed;
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
            !report_number("Value must be at least ", *node.minimum)) {
            return false;
        }
        if (node.maximum && compare_numbers(value, *node.maximum) > 0 &&
            !report_number("Value must be at most ", *node.maximum)) {
            return false;
        }
        if (node.exclusive_minimum && compare_numbers(value, *node.exclusive_minimum) <= 0 &&
            !report_number("Value must be greater than ", *node.exclusive_minimum)) {
            return false;
        }
        if (node.exclusive_maximum && compare_numbers(value, *node.exclusive_maximum) >= 0 &&
            !report_number("Value must be less than ", *node.exclusive_maximum)) {
            return false;
        }
        if (node.multiple_of && !is_multiple_of(value.get<double>(), *node.multiple_of) &&
            !report_number("Value is not a multiple of ", nlohmann::json(*node.multiple_of))) {
            return false;
        }
        return true;
    }

    bool matches(const Regex& regex, const std::string& text) {
        const auto matched = regex.search(text, pattern_steps_);
        if (!matched) {
            // NOLINTNEXTLINE(bugprone-std-exception-baseclass): see the declaration.
            throw PatternBudgetExceeded();
        }
        return *matched;
    }

    bool check_string(const Node& node, const std::string& text) {
        if ((node.content_encoding || node.content_media_type) && !check_content(node, text)) {
            return false;
        }
        if (node.min_length || node.max_length) {
            charge(work_, 1 + (text.size() / g_STRING_BYTES_PER_UNIT));
            const auto length = code_point_count(text);
            if (node.min_length && length < *node.min_length &&
                !report_size("Number of characters must be at least ", *node.min_length)) {
                return false;
            }
            if (node.max_length && length > *node.max_length &&
                !report_size("Number of characters must be at most ", *node.max_length)) {
                return false;
            }
        }
        if (node.pattern && !matches(*node.pattern, text) &&
            !report_concat("String does not match the pattern ", node.pattern->pattern())) {
            return false;
        }
        return true;
    }

    /// Checks @p item, element @p index of the array being checked, against @p node.
    bool check_item(const Node& node, const nlohmann::json& item, std::size_t index) {
        const LocationScope scope(*this, Location{.name = {}, .index = index});
        if (annotations_ == nullptr) {
            return check(node, item);
        }
        const AnnotationScope annotations(*this, nullptr);
        return check(node, item);
    }

    bool unique(const nlohmann::json& array) { return all_unique(array, work_); }

    bool check_array(const Node& node, const nlohmann::json& array) {
        const auto prefix = std::min(node.prefix_items.size(), array.size());
        for (std::size_t index = 0; index < prefix; ++index) {
            if (!check_item(*node.prefix_items[index], array[index], index)) {
                return false;
            }
        }
        if (node.items != nullptr) {
            for (std::size_t index = node.prefix_items.size(); index < array.size(); ++index) {
                if (!check_item(*node.items, array[index], index)) {
                    return false;
                }
            }
        }
        if (annotations_ != nullptr) {
            annotations_->items =
                std::max(annotations_->items, node.items != nullptr ? array.size() : prefix);
        }
        if ((node.contains != nullptr) && !check_contains(node, array)) {
            return false;
        }
        return (!node.unique_items && !node.min_items && !node.max_items) ||
               check_array_size(node, array);
    }

    MCP_SCHEMA_NOINLINE bool check_contains(const Node& node, const nlohmann::json& array) {
        std::size_t matching = 0;
        for (std::size_t index = 0; index < array.size(); ++index) {
            if (passes(*node.contains, array[index])) {
                ++matching;
                if (annotations_ != nullptr) {
                    annotations_->item(index, array.size());
                }
            }
        }
        const auto least = node.min_contains.value_or(1);
        if (matching < least &&
            !report_count("Array must contain at least ", least, " matching items, found ", matching)) {
            return false;
        }
        if (node.max_contains && matching > *node.max_contains &&
            !report_count("Array must contain at most ", *node.max_contains, " matching items, found ",
                          matching)) {
            return false;
        }
        return true;
    }

    MCP_SCHEMA_NOINLINE bool check_array_size(const Node& node, const nlohmann::json& array) {
        if (node.unique_items && !unique(array) && !report("Array items are not unique")) {
            return false;
        }
        if (node.min_items && array.size() < *node.min_items &&
            !report_count("Array must have at least ", *node.min_items, " items, found ",
                          array.size())) {
            return false;
        }
        if (node.max_items && array.size() > *node.max_items &&
            !report_count("Array must have at most ", *node.max_items, " items, found ",
                          array.size())) {
            return false;
        }
        return true;
    }

    /// Checks @p member, named @p name, against @p node, at its own location.
    bool check_member(const Node& node, const std::string& name, const nlohmann::json& member) {
        const LocationScope scope(*this, Location{.name = &name, .index = 0});
        if (annotations_ == nullptr) {
            return check(node, member);
        }
        const AnnotationScope annotations(*this, nullptr);
        return check(node, member);
    }

    bool check_members(const Node& node, const nlohmann::json& object) {
        std::size_t position = 0;
        for (const auto& [name, member] : object.get_ref<const nlohmann::json::object_t&>()) {
            charge(work_, 1 + (name.size() / g_NAME_BYTES_PER_UNIT));
            const auto here = position++;
            bool evaluated = false;
            if (const Node* property = find_property(node, name)) {
                evaluated = true;
                if (!check_member(*property, name, member)) {
                    return false;
                }
            }
            if (!node.pattern_regexes.empty() &&
                !check_pattern_members(node, name, member, evaluated)) {
                return false;
            }
            if ((evaluated || node.additional_properties != nullptr) && annotations_ != nullptr) {
                annotations_->member(here, object.size());
            }
            if (evaluated || (node.additional_properties == nullptr)) {
                continue;
            }
            if (node.additional_properties->reject_all) {
                if (!report_additional(name)) {
                    return false;
                }
            } else if (!check_member(*node.additional_properties, name, member)) {
                return false;
            }
        }
        return true;
    }

    /// The `properties` schema of the member @p name, or null.
    MCP_SCHEMA_NOINLINE static const Node* find_property(const Node& node, const std::string& name) {
        const auto found = std::ranges::lower_bound(node.property_names, name);
        if (found == node.property_names.end() || *found != name) {
            return nullptr;
        }
        return node.property_schemas[static_cast<std::size_t>(found - node.property_names.begin())];
    }

    /// Checks the member @p name against every `patternProperties` schema whose pattern matches it,
    /// setting @p evaluated when one does.
    MCP_SCHEMA_NOINLINE bool check_pattern_members(const Node& node, const std::string& name,
                                                   const nlohmann::json& member, bool& evaluated) {
        for (std::size_t i = 0; i < node.pattern_regexes.size(); ++i) {
            if (matches(node.pattern_regexes[i], name)) {
                evaluated = true;
                if (!check_member(*node.pattern_schemas[i], name, member)) {
                    return false;
                }
            }
        }
        return true;
    }

    MCP_SCHEMA_NOINLINE bool report_additional(const std::string& name) {
        const LocationScope scope(*this, Location{.name = &name, .index = 0});
        return report_concat("Additional property '", name, "' is not allowed");
    }

    bool check_object(const Node& node, const nlohmann::json& object) {
        if ((!node.required.empty() || !node.dependent_required_names.empty()) &&
            !check_required(node, object)) {
            return false;
        }
        if ((!node.property_names.empty() || !node.pattern_regexes.empty() ||
             node.additional_properties != nullptr) &&
            (!check_members(node, object))) {
            return false;
        }

        if ((node.property_names_schema != nullptr) && !check_property_names(node, object)) {
            return false;
        }
        for (std::size_t i = 0; i < node.dependent_schema_names.size(); ++i) {
            if (object.contains(node.dependent_schema_names[i]) &&
                !check(*node.dependent_schemas[i], object)) {
                return false;
            }
        }
        return (!node.min_properties && !node.max_properties) || check_property_count(node, object);
    }

    MCP_SCHEMA_NOINLINE bool check_required(const Node& node, const nlohmann::json& object) {
        for (const auto& name : node.required) {
            if (!object.contains(name) && !report_concat("Required property '", name, "' is missing")) {
                return false;
            }
        }
        // Each entry is a lookup, whether or not its name is present.
        charge(work_, node.dependent_required_names.size());
        for (std::size_t i = 0; i < node.dependent_required_names.size(); ++i) {
            const auto& present = node.dependent_required_names[i];
            if (!object.contains(present)) {
                continue;
            }
            for (const auto& name : node.dependent_required_lists[i]) {
                charge(work_, 1);
                if (!object.contains(name) && !report_concat("Property '", name, "' is required when '",
                                                             present, "' is present")) {
                    return false;
                }
            }
        }
        return true;
    }

    MCP_SCHEMA_NOINLINE bool check_property_names(const Node& node, const nlohmann::json& object) {
        return std::ranges::all_of(object.get_ref<const nlohmann::json::object_t&>(),
                                   [this, &node](const auto& entry) {
                                       const std::string& name = entry.first;
                                       charge(work_, 1 + (name.size() / g_NAME_BYTES_PER_UNIT));
                                       const AnnotationScope annotations(*this, nullptr);
                                       return check(*node.property_names_schema, nlohmann::json(name));
                                   });
    }

    MCP_SCHEMA_NOINLINE bool check_property_count(const Node& node, const nlohmann::json& object) {
        if (node.min_properties && object.size() < *node.min_properties &&
            !report_count("Object must have at least ", *node.min_properties, " properties, found ",
                          object.size())) {
            return false;
        }
        if (node.max_properties && object.size() > *node.max_properties &&
            !report_count("Object must have at most ", *node.max_properties, " properties, found ",
                          object.size())) {
            return false;
        }
        return true;
    }

    /// Reports @p message, then as many of @p violations as the current sink takes.
    MCP_SCHEMA_NOINLINE bool report_with(std::string_view message,
                                         const std::vector<SchemaViolation>& violations) {
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

    /// `anyOf`: kept out of check_applicators() so that the branch buffer is not on the stack of
    /// every applicator.
    MCP_SCHEMA_NOINLINE bool check_any_of(const Node& node, const nlohmann::json& value) {
        const auto buffer_size =
            std::min(mcp::detail::constants::schema::g_MAX_BRANCH_VIOLATIONS, sink_->limit);
        std::vector<SchemaViolation> first;
        bool matched = false;
        // Only the first branch's violations are ever reported, so the others are evaluated
        // for their validity alone.
        matched = passes_annotated(*node.any_of[0], value, &first, buffer_size);
        // What every passing branch evaluated counts, so all of them are evaluated then.
        for (std::size_t i = 1; i < node.any_of.size() && (!matched || annotations_ != nullptr); ++i) {
            matched = passes_annotated(*node.any_of[i], value) || matched;
        }
        return matched || report_with("Value does not match any of the anyOf schemas", first);
    }

    /// `oneOf`, kept out of check_applicators() like check_any_of().
    MCP_SCHEMA_NOINLINE bool check_one_of(const Node& node, const nlohmann::json& value) {
        const auto buffer_size =
            std::min(mcp::detail::constants::schema::g_MAX_BRANCH_VIOLATIONS, sink_->limit);
        std::vector<SchemaViolation> first;
        std::size_t matched = 0;
        if (passes_annotated(*node.one_of[0], value, &first, buffer_size)) {
            ++matched;
        }
        for (std::size_t i = 1; i < node.one_of.size() && matched < 2; ++i) {
            if (passes_annotated(*node.one_of[i], value)) {
                ++matched;
            }
        }
        if (matched == 0 && !report_with("Value does not match any of the oneOf schemas", first)) {
            return false;
        }
        if (matched > 1 && !report("Value matches more than one of the oneOf schemas")) {
            return false;
        }
        return true;
    }

    bool check_applicators(const Node& node, const nlohmann::json& value) {
        for (const Node* branch : node.all_of) {
            if (!check(*branch, value)) {
                return false;
            }
        }
        if (!node.any_of.empty() && !check_any_of(node, value)) {
            return false;
        }
        if (!node.one_of.empty() && !check_one_of(node, value)) {
            return false;
        }
        if ((node.not_schema != nullptr) && passes(*node.not_schema, value) &&
            !report("Value must not be valid against the not schema")) {
            return false;
        }
        if (node.if_schema != nullptr) {
            const Node* branch =
                passes_annotated(*node.if_schema, value) ? node.then_schema : node.else_schema;
            if (branch != nullptr && !check(*branch, value)) {
                return false;
            }
        }
        if (node.ref != nullptr && !check(*node.ref, value)) {
            return false;
        }
        if (node.dynamic_ref != nullptr && !check(dynamic_target(node), value)) {
            return false;
        }
        if (node.recursive_ref != nullptr && !check(recursive_target(node), value)) {
            return false;
        }
        return true;
    }

    /// The JSON Pointer of the current location, escaped as RFC 6901 requires.
    [[nodiscard]] std::string path() const {
        std::string path;
        constexpr std::size_t typical_step_bytes = 8;
        path.reserve(locations_.size() * typical_step_bytes);
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

    MCP_SCHEMA_NOINLINE bool report_type(const std::vector<std::string>& types,
                                         const nlohmann::json& value) {
        return report(type_message(types, value));
    }

    /// Reports the concatenation of the parts.
    MCP_SCHEMA_NOINLINE bool report_concat(std::string_view first, std::string_view second,
                                           std::string_view third = {}, std::string_view fourth = {},
                                           std::string_view fifth = {}) {
        std::string message;
        message.reserve(first.size() + second.size() + third.size() + fourth.size() + fifth.size());
        message.append(first).append(second).append(third).append(fourth).append(fifth);
        return report(message);
    }

    /// Reports @p before, @p size and @p after.
    MCP_SCHEMA_NOINLINE bool report_size(std::string_view before, std::size_t size,
                                         std::string_view after = {}) {
        return report_concat(before, std::to_string(size), after);
    }

    /// Reports @p before, @p bound, @p middle and @p found.
    MCP_SCHEMA_NOINLINE bool report_count(std::string_view before, std::size_t bound,
                                          std::string_view middle, std::size_t found) {
        return report_concat(before, std::to_string(bound), middle, std::to_string(found));
    }

    MCP_SCHEMA_NOINLINE bool report_number(std::string_view before, const nlohmann::json& bound) {
        return report(std::string(before) + bound.dump());
    }

    /// Reports @p message at the current location; false once the current sink takes no more.
    MCP_SCHEMA_NOINLINE bool report(std::string_view message) {
        sink_->valid = false;
        if (sink_->violations == nullptr) {
            return false;
        }
        if (sink_->violations->size() < sink_->limit) {
            sink_->violations->push_back({
                .instance_path = sanitize_for_diagnostics(path()),
                .message = sanitize_for_diagnostics(message),
            });
        }
        return sink_->violations->size() < sink_->limit;
    }

    Sink root_;
    Sink* sink_;
    std::size_t& pattern_steps_;
    std::size_t& work_;
    std::size_t depth_ = 0;
    bool dynamic_scope_;
    std::vector<const Resource*> scope_;  ///< The dynamic scope, outermost first.
    /// Where the schemas evaluated at the current location record what they evaluated; null when
    /// nothing reads it.
    Annotations* annotations_ = nullptr;
    Locations locations_;  ///< The path to the value being checked; a report builds it.
};

}  // namespace

NativeSchema::NativeSchema(std::shared_ptr<const Document> document) : document_(std::move(document)) {}

NativeSchema NativeSchema::compile(const nlohmann::json& schema, const SchemaDocuments& documents) {
    auto document = std::make_shared<Document>();
    Compiler compiler(*document, documents);
    if (!compiler.compile(schema)) {
        throw compiler.problem();
    }
    return NativeSchema(std::move(document));
}

bool NativeSchema::has_references() const { return document_->references; }

std::optional<std::string_view> NativeSchema::evaluate(const nlohmann::json& instance,
                                                       std::size_t max_violations,
                                                       std::size_t pattern_steps, std::size_t work,
                                                       ValidationResult& result) const {
    try {
        Evaluation evaluation(*document_, max_violations, pattern_steps, work, result);
        (void)evaluation.check(*document_->root, instance);
        result.valid = evaluation.valid();
    } catch (const PatternBudgetExceeded&) {
        return "pattern matching takes too many steps";
    } catch (const WorkBudgetExceeded&) {
        return "schema evaluation takes too much work";
    } catch (const EvaluationTooDeep&) {
        return "schema evaluation nests too deeply";
    }
    return std::nullopt;
}

void NativeSchema::validate(const nlohmann::json& instance, std::size_t max_violations,
                            std::size_t pattern_steps, std::size_t work,
                            ValidationResult& result) const {
    if (const auto reason = evaluate(instance, max_violations, pattern_steps, work, result)) {
        result.valid = false;
        result.violations.clear();
        result.violations.push_back(
            {.instance_path = "", .message = "validation stopped: " + std::string(*reason)});
    }
}

std::optional<std::string_view> NativeSchema::stop_reason(const nlohmann::json& instance,
                                                          std::size_t pattern_steps,
                                                          std::size_t work) const {
    ValidationResult ignored;
    return evaluate(instance, 0, pattern_steps, work, ignored);
}

}  // namespace mcp::detail
