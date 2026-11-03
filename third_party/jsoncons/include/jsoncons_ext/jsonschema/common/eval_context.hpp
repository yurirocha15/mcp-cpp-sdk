// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_EXT_JSONSCHEMA_COMMON_EVAL_CONTEXT_HPP
#define MCP_DETAIL_JSONCONS_EXT_JSONSCHEMA_COMMON_EVAL_CONTEXT_HPP

#include <cstddef>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <mcp/detail/regex.hpp>

#include <jsoncons/config/jsoncons_config.hpp>
#include <jsoncons/utility/uri.hpp>
#include <jsoncons_ext/jsonpointer/jsonpointer.hpp>
#include <jsoncons_ext/jsonschema/jsonschema_error.hpp>

namespace mcp_detail_jsoncons {
namespace jsonschema {

template <typename Json>
class schema_validator;

enum class evaluation_flags : unsigned {require_evaluated_properties=1, require_evaluated_items=2};

MCP_DETAIL_JSONCONS_ATTRIBUTE_NODISCARD
constexpr evaluation_flags
operator|(evaluation_flags lhs, evaluation_flags rhs) noexcept
{ return (evaluation_flags)((unsigned)lhs | (unsigned)rhs); }

MCP_DETAIL_JSONCONS_ATTRIBUTE_NODISCARD
constexpr evaluation_flags
operator&(evaluation_flags lhs, evaluation_flags rhs) noexcept
{ return (evaluation_flags)((unsigned)lhs & (unsigned)rhs); }

MCP_DETAIL_JSONCONS_ATTRIBUTE_NODISCARD
constexpr evaluation_flags
operator^(evaluation_flags lhs, evaluation_flags rhs) noexcept
{ return (evaluation_flags)((unsigned)lhs ^ (unsigned)rhs); }

MCP_DETAIL_JSONCONS_ATTRIBUTE_NODISCARD
constexpr evaluation_flags
operator~(evaluation_flags flags) noexcept
{ return (evaluation_flags)~(unsigned)flags; }

constexpr evaluation_flags&
operator|=(evaluation_flags& lhs, evaluation_flags rhs) noexcept
{ return lhs = lhs | rhs; }

constexpr evaluation_flags&
operator&=(evaluation_flags& lhs, evaluation_flags rhs) noexcept
{ return lhs = lhs & rhs; }

constexpr evaluation_flags&
operator^=(evaluation_flags& lhs, evaluation_flags rhs) noexcept
{ return lhs = lhs ^ rhs; }

// Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): limits on one validation. A reference
// cycle that never descends into the value makes evaluation recurse without end, and branching
// applicators over recursive references can make the number of evaluation steps exponential in
// the value's depth. eval_context is created for every keyword evaluated, so it enforces both.
// Creating one copies the evaluation path and the dynamic scope, so a step costs work in
// proportion to the path's length, and the budget is charged accordingly. Copies of JSON
// pointers are charged to the same budget (see jsonpointer::evaluation_work).
using evaluation_limit_error = jsonpointer::evaluation_limit_error;

struct evaluation_limits
{
    // Longest evaluation path, counted in keywords and array indexes.
    static constexpr std::size_t max_depth = 192;

    // Messages one branching keyword keeps from its branches. A keyword counts every message, so
    // its verdict does not depend on how many are kept.
    static constexpr std::size_t max_kept_messages = 16;

    // Longest excerpt of a value or a schema quoted in a message, in bytes.
    static constexpr std::size_t max_excerpt = 256;

    // Evaluation work left for the validation running on this thread. A step with an evaluation
    // path of n tokens costs n + 1. The caller sets it before each validation; the initial value
    // does not limit anything.
    static std::size_t& work_left()
    {
        return jsonpointer::evaluation_work::left();
    }

    // Bytes that the messages kept by branching keywords may still take on this thread. The
    // caller sets it before each validation; the initial value does not limit anything.
    static std::size_t& kept_message_bytes_left()
    {
        static thread_local std::size_t bytes = static_cast<std::size_t>(-1);
        return bytes;
    }

    static void keep_message_bytes(std::size_t bytes)
    {
        std::size_t& left = kept_message_bytes_left();
        if (left < bytes)
        {
            throw evaluation_limit_error("the messages kept for failed branches take too much memory");
        }
        left -= bytes;
    }

    static void release_message_bytes(std::size_t bytes)
    {
        kept_message_bytes_left() += bytes;
    }

    static std::string_view excerpt(std::string_view text)
    {
        return text.substr(0, max_excerpt);
    }

    // Longest property name kept in an instance location or an evaluation path, in bytes. The SDK
    // reports only instance locations, flattened and cut to 256 bytes, and flattening turns at
    // most 3 bytes into 1, so the first 772 bytes of a name are all a report can show. Every step
    // under the name copies both, so a longer name would cost its whole length for each item
    // below it.
    static constexpr std::size_t max_location_token = 1024;

    static std::string_view location_token(std::string_view name)
    {
        return name.substr(0, max_location_token);
    }

    // A value as a message names it: a scalar quoted in full, a string cut to an excerpt, and an
    // array or object by its size, so a message takes the same time whatever the value holds.
    template <typename Json>
    static std::string describe(const Json& value)
    {
        if (value.is_string())
        {
            return "'" + std::string(excerpt(value.as_string_view())) + "'";
        }
        if (value.is_array())
        {
            return "An array of length " + std::to_string(value.size());
        }
        if (value.is_object())
        {
            return "An object of size " + std::to_string(value.size());
        }
        return "'" + value.template as<std::string>() + "'";
    }

    // Pattern matching steps left for the validation running on this thread, as
    // ::mcp::detail::Regex::search counts them. The caller sets it before each validation; the
    // initial value does not limit anything.
    static std::size_t& pattern_steps_left()
    {
        static thread_local std::size_t steps = static_cast<std::size_t>(-1);
        return steps;
    }

    static bool search(const ::mcp::detail::Regex& regex, std::string_view input)
    {
        const auto matched = regex.search(input, pattern_steps_left());
        if (!matched)
        {
            throw evaluation_limit_error("pattern matching takes too many steps");
        }
        return *matched;
    }

    // Charges work that does not create an eval_context, such as a comparison in a loop over a
    // keyword's values.
    static void charge(std::size_t units)
    {
        jsonpointer::evaluation_work::charge(units);
    }

    static void enter(std::size_t depth)
    {
        if (depth > max_depth)
        {
            throw evaluation_limit_error("schema evaluation nests too deeply");
        }
        std::size_t& work = work_left();
        if (work <= depth)
        {
            throw evaluation_limit_error("schema evaluation takes too many steps");
        }
        work -= depth + 1;
    }
};

template <typename Json>
class eval_context
{
private:
    std::vector<const schema_validator<Json>*> dynamic_scope_;
    jsonpointer::json_pointer eval_path_;
    evaluation_flags flags_;
public:
    eval_context()
        : flags_{}
    {
    }

    eval_context(const eval_context& other)
        : dynamic_scope_ { other.dynamic_scope_}, eval_path_{other.eval_path_},
          flags_(other.flags_)
    {
    }

    eval_context(eval_context&& other) noexcept
        : dynamic_scope_{std::move(other.dynamic_scope_)},eval_path_{std::move(other.eval_path_)},
          flags_(other.flags_)
    {
    }

    eval_context(const eval_context& parent, const schema_validator<Json> *validator)
        : dynamic_scope_ { parent.dynamic_scope_ }, eval_path_{ parent.eval_path_ },
          flags_(parent.flags_)
    {
        if (validator->id() || dynamic_scope_.empty())
        {
            dynamic_scope_.push_back(validator);
        }
    }

    eval_context(const eval_context& parent, const schema_validator<Json> *validator,
        evaluation_flags flags)
        : dynamic_scope_ { parent.dynamic_scope_ }, eval_path_{ parent.eval_path_ },
          flags_(flags)
    {
        if (validator->id() || dynamic_scope_.empty())
        {
            dynamic_scope_.push_back(validator);
        }
    }

    eval_context(const eval_context& parent, mcp_detail_jsoncons::string_view name)
        : dynamic_scope_{parent.dynamic_scope_}, eval_path_(parent.eval_path() / evaluation_limits::location_token(name)),
          flags_(parent.flags_)
          
    {
        evaluation_limits::enter(eval_path_.tokens().size());
    }

    eval_context(const eval_context& parent, mcp_detail_jsoncons::string_view name,
        evaluation_flags flags)
        : dynamic_scope_{parent.dynamic_scope_}, eval_path_(parent.eval_path() / evaluation_limits::location_token(name)),
          flags_(flags)
    {
        evaluation_limits::enter(eval_path_.tokens().size());
    }

    eval_context(const eval_context& parent, std::size_t index)
        : dynamic_scope_{parent.dynamic_scope_}, eval_path_(parent.eval_path() / index),
          flags_(parent.flags_)
    {
        evaluation_limits::enter(eval_path_.tokens().size());
    }

    eval_context(const eval_context& parent, std::size_t index,
        evaluation_flags flags)
        : dynamic_scope_{parent.dynamic_scope_}, eval_path_(parent.eval_path() / index),
          flags_(flags)
    {
        evaluation_limits::enter(eval_path_.tokens().size());
    }

    const std::vector<const schema_validator<Json>*>& dynamic_scope() const
    {
        return dynamic_scope_;
    }

    const jsonpointer::json_pointer& eval_path() const
    {
        return eval_path_;
    }
    
    evaluation_flags eval_flags() const
    {
        return flags_;
    }
    
    bool require_evaluated_properties() const
    {
        return (flags_ & evaluation_flags::require_evaluated_properties) == evaluation_flags::require_evaluated_properties;
    }

    bool require_evaluated_items() const
    {
        return (flags_ & evaluation_flags::require_evaluated_items) == evaluation_flags::require_evaluated_items;
    }
}; 

} // namespace jsonschema
} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_EXT_JSONSCHEMA_COMPILATION_CONTEXT_HPP
