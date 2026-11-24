#include "schema_checker.hpp"

#include "mcp/detail/protocol/schema_uri.hpp"
#include "schema_values.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp::detail {

namespace {

using D = SchemaDialect;

bool is_ascii_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

/// Whether @p name matches the `$anchor` pattern of @p dialect: `^[A-Za-z][-A-Za-z0-9.:_]*$` in
/// 2019-09, `^[A-Za-z_][-A-Za-z0-9._]*$` in 2020-12.
bool is_anchor_name(std::string_view name, SchemaDialect dialect) {
    const bool modern = dialect >= D::eDraft202012;
    if (name.empty() || (!is_ascii_letter(name.front()) && (!modern || name.front() != '_'))) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [modern](char c) {
        return is_ascii_letter(c) || is_ascii_digit(c) || c == '-' || c == '.' || c == '_' ||
               (!modern && c == ':');
    });
}

/// Whether @p id matches `^[^#]*#?$`: no fragment, or an empty one.
bool has_no_fragment(std::string_view id) {
    const auto hash = id.find('#');
    return hash == std::string_view::npos || hash + 1 == id.size();
}

bool is_simple_type(std::string_view type) {
    static constexpr std::array<std::string_view, 7> s_types = {"array",  "boolean", "integer", "null",
                                                                "number", "object",  "string"};
    return std::ranges::find(s_types, type) != s_types.end();
}

/// Checks one schema document against the rules of one dialect.
class Checker {
   public:
    Checker(SchemaDialect dialect, std::size_t& work, std::size_t max_depth)
        : dialect_(dialect), work_(work), max_depth_(max_depth) {}

    /// Checks @p value as a schema at the current path; false when it breaks a rule.
    bool schema(const nlohmann::json& value) {
        charge(work_, 1);
        if (value.is_boolean()) {
            return dialect_ != D::eDraft4 || fail(expected("object", value));
        }
        if (!value.is_object()) {
            return fail(expected(dialect_ == D::eDraft4 ? "object" : "object or boolean", value));
        }
        const Level level(*this);
        for (const auto& [keyword, member] : value.items()) {
            const Step step(*this, keyword);
            if (!this->keyword(keyword, member)) {
                return false;
            }
        }
        if (dialect_ == D::eDraft4) {
            // The draft-04 metaschema's `dependencies`: a boolean exclusive bound needs its bound.
            for (const auto& [exclusive, bound] : {
                     std::pair<const char*, const char*>{"exclusiveMaximum", "maximum"},
                     std::pair<const char*, const char*>{"exclusiveMinimum", "minimum"},
                 }) {
                if (value.contains(exclusive) && !value.contains(bound)) {
                    return fail(std::string(exclusive) + " requires " + bound);
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<SchemaProblem> problem() && { return std::move(problem_); }

   private:
    /// One level of subschema nesting.
    class Level {
       public:
        explicit Level(Checker& checker) : checker_(checker) {
            if (++checker_.depth_ > checker_.max_depth_) {
                // NOLINTNEXTLINE(bugprone-std-exception-baseclass): see the declaration.
                throw EvaluationTooDeep{};
            }
        }
        ~Level() { --checker_.depth_; }
        Level(const Level&) = delete;
        Level& operator=(const Level&) = delete;
        Level(Level&&) = delete;
        Level& operator=(Level&&) = delete;

       private:
        Checker& checker_;
    };

    /// One step of the current path, a member name or an array index.
    class Step {
       public:
        Step(Checker& checker, std::string token) : checker_(checker) {
            checker_.path_.push_back(std::move(token));
        }
        Step(Checker& checker, std::size_t index) : Step(checker, std::to_string(index)) {}
        ~Step() { checker_.path_.pop_back(); }
        Step(const Step&) = delete;
        Step& operator=(const Step&) = delete;
        Step(Step&&) = delete;
        Step& operator=(Step&&) = delete;

       private:
        Checker& checker_;
    };

    bool keyword(const std::string& keyword, const nlohmann::json& value) {
        if (keyword == "$schema" || keyword == "title" || keyword == "description" ||
            keyword == "pattern" || keyword == "format") {
            return string(value);
        }
        if (keyword == "default") {
            return true;
        }
        if (keyword == "multipleOf") {
            return number(value) &&
                   (compare_numbers(value, nlohmann::json(0)) > 0 || fail("must be greater than 0"));
        }
        if (keyword == "maximum" || keyword == "minimum") {
            return number(value);
        }
        if (keyword == "exclusiveMaximum" || keyword == "exclusiveMinimum") {
            return dialect_ == D::eDraft4 ? boolean(value) : number(value);
        }
        if (keyword == "maxLength" || keyword == "minLength" || keyword == "maxItems" ||
            keyword == "minItems" || keyword == "maxProperties" || keyword == "minProperties") {
            return non_negative_integer(value);
        }
        if (keyword == "uniqueItems") {
            return boolean(value);
        }
        if (keyword == "required") {
            return string_array(value);
        }
        if (keyword == "enum") {
            return enum_values(value);
        }
        if (keyword == "type") {
            return type(value);
        }
        if (keyword == "items") {
            if (dialect_ <= D::eDraft201909 && value.is_array()) {
                return schema_array(value);
            }
            return schema(value);
        }
        if (keyword == "additionalItems") {
            if (dialect_ >= D::eDraft202012) {
                return true;
            }
            return (dialect_ == D::eDraft4 && value.is_boolean()) || schema(value);
        }
        if (keyword == "additionalProperties") {
            return (dialect_ == D::eDraft4 && value.is_boolean()) || schema(value);
        }
        if (keyword == "definitions" || keyword == "properties" || keyword == "patternProperties") {
            return schema_map(value);
        }
        if (keyword == "dependencies") {
            return dependencies(value);
        }
        if (keyword == "allOf" || keyword == "anyOf" || keyword == "oneOf") {
            return schema_array(value);
        }
        if (keyword == "not") {
            return schema(value);
        }
        if (dialect_ == D::eDraft4) {
            return keyword != "id" || string(value);
        }
        // draft-06 and later.
        if (keyword == "$ref") {
            return string(value);
        }
        if (keyword == "$id") {
            if (!string(value)) {
                return false;
            }
            return dialect_ <= D::eDraft7 || has_no_fragment(value.get_ref<const std::string&>()) ||
                   fail("must not have a non-empty fragment");
        }
        if (keyword == "examples") {
            return array(value);
        }
        if (keyword == "contains" || keyword == "propertyNames") {
            return schema(value);
        }
        if (dialect_ == D::eDraft6) {
            return true;
        }
        // draft-07 and later.
        if (keyword == "$comment" || keyword == "contentMediaType" || keyword == "contentEncoding") {
            return string(value);
        }
        if (keyword == "readOnly" || keyword == "writeOnly") {
            return boolean(value);
        }
        if (keyword == "if" || keyword == "then" || keyword == "else") {
            return schema(value);
        }
        if (dialect_ == D::eDraft7) {
            return true;
        }
        // 2019-09 and later.
        if (keyword == "$anchor") {
            return anchor(value);
        }
        if (keyword == "$vocabulary") {
            return vocabulary(value);
        }
        if (keyword == "$defs" || keyword == "dependentSchemas") {
            return schema_map(value);
        }
        if (keyword == "unevaluatedItems" || keyword == "unevaluatedProperties" ||
            keyword == "contentSchema") {
            return schema(value);
        }
        if (keyword == "maxContains" || keyword == "minContains") {
            return non_negative_integer(value);
        }
        if (keyword == "dependentRequired") {
            return string_array_map(value);
        }
        if (keyword == "deprecated") {
            return boolean(value);
        }
        if (keyword == "$recursiveRef") {
            return string(value);
        }
        if (dialect_ == D::eDraft201909) {
            return keyword != "$recursiveAnchor" || boolean(value);
        }
        // 2020-12.
        if (keyword == "$dynamicRef") {
            return string(value);
        }
        if (keyword == "$dynamicAnchor" || keyword == "$recursiveAnchor") {
            return anchor(value);
        }
        if (keyword == "prefixItems") {
            return schema_array(value);
        }
        return true;
    }

    bool string(const nlohmann::json& value) {
        return value.is_string() || fail(expected("string", value));
    }

    bool boolean(const nlohmann::json& value) {
        return value.is_boolean() || fail(expected("boolean", value));
    }

    bool number(const nlohmann::json& value) {
        return value.is_number() || fail(expected("number", value));
    }

    bool array(const nlohmann::json& value) {
        return value.is_array() || fail(expected("array", value));
    }

    bool object(const nlohmann::json& value) {
        return value.is_object() || fail(expected("object", value));
    }

    bool non_negative_integer(const nlohmann::json& value) {
        if (!has_type(value, "integer")) {
            return fail(expected("integer", value));
        }
        return compare_numbers(value, nlohmann::json(0)) >= 0 || fail("must not be negative");
    }

    bool unique(const nlohmann::json& array) {
        return all_unique(array, work_) || fail("items must be unique");
    }

    /// An array of strings, unique, and in draft-04 not empty.
    bool string_array(const nlohmann::json& value) {
        if (!array(value)) {
            return false;
        }
        if (dialect_ == D::eDraft4 && value.empty()) {
            return fail("must not be empty");
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            const Step step(*this, index);
            if (!string(value[index])) {
                return false;
            }
        }
        return unique(value);
    }

    bool enum_values(const nlohmann::json& value) {
        if (!array(value)) {
            return false;
        }
        if (dialect_ >= D::eDraft201909) {
            return true;
        }
        return (!value.empty() || fail("must not be empty")) && unique(value);
    }

    bool type(const nlohmann::json& value) {
        if (value.is_string()) {
            return simple_type(value);
        }
        if (!value.is_array()) {
            return fail(expected("string or array", value));
        }
        if (value.empty()) {
            return fail("must not be empty");
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            const Step step(*this, index);
            if (!simple_type(value[index])) {
                return false;
            }
        }
        return unique(value);
    }

    bool simple_type(const nlohmann::json& value) {
        if (!string(value)) {
            return false;
        }
        return is_simple_type(value.get_ref<const std::string&>()) ||
               fail("'" + value.get<std::string>() + "' is not a JSON Schema type");
    }

    /// A non-empty array of schemas.
    bool schema_array(const nlohmann::json& value) {
        if (!array(value)) {
            return false;
        }
        if (value.empty()) {
            return fail("must not be empty");
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            const Step step(*this, index);
            if (!schema(value[index])) {
                return false;
            }
        }
        return true;
    }

    /// An object whose members are schemas.
    bool schema_map(const nlohmann::json& value) {
        if (!object(value)) {
            return false;
        }
        return std::ranges::all_of(value.get_ref<const nlohmann::json::object_t&>(),
                                   [this](const auto& entry) {
                                       const auto& [name, member] = entry;
                                       const Step step(*this, name);
                                       return schema(member);
                                   });
    }

    /// An object whose members are string arrays.
    bool string_array_map(const nlohmann::json& value) {
        if (!object(value)) {
            return false;
        }
        return std::ranges::all_of(value.get_ref<const nlohmann::json::object_t&>(),
                                   [this](const auto& entry) {
                                       const auto& [name, member] = entry;
                                       const Step step(*this, name);
                                       return string_array(member);
                                   });
    }

    /// An object whose members are schemas or string arrays.
    bool dependencies(const nlohmann::json& value) {
        if (!object(value)) {
            return false;
        }
        return std::ranges::all_of(value.get_ref<const nlohmann::json::object_t&>(),
                                   [this](const auto& entry) {
                                       const auto& [name, member] = entry;
                                       const Step step(*this, name);
                                       return member.is_array() ? string_array(member) : schema(member);
                                   });
    }

    bool anchor(const nlohmann::json& value) {
        if (!string(value)) {
            return false;
        }
        return is_anchor_name(value.get_ref<const std::string&>(), dialect_) ||
               fail("'" + value.get<std::string>() + "' is not an anchor name");
    }

    /// An object whose members are booleans.
    bool vocabulary(const nlohmann::json& value) {
        if (!object(value)) {
            return false;
        }
        return std::ranges::all_of(value.get_ref<const nlohmann::json::object_t&>(),
                                   [this](const auto& entry) {
                                       const auto& [name, member] = entry;
                                       const Step step(*this, name);
                                       return boolean(member);
                                   });
    }

    static std::string expected(std::string_view wanted, const nlohmann::json& found) {
        std::string message = "expected ";
        message.append(wanted).append(", found ").append(type_name(found));
        return message;
    }

    bool fail(std::string message) {
        std::string pointer;
        for (const auto& token : path_) {
            pointer += '/';
            pointer += escape_pointer_token(token);
        }
        problem_ = SchemaProblem{.path = std::move(pointer), .message = std::move(message)};
        return false;
    }

    SchemaDialect dialect_;
    std::size_t& work_;
    std::size_t max_depth_;
    std::size_t depth_ = 0;
    std::vector<std::string> path_;
    std::optional<SchemaProblem> problem_;
};

}  // namespace

std::optional<SchemaProblem> check_schema(const nlohmann::json& schema, SchemaDialect dialect,
                                          std::size_t& work, std::size_t max_depth) {
    Checker checker(dialect, work, max_depth);
    (void)checker.schema(schema);
    return std::move(checker).problem();
}

}  // namespace mcp::detail
