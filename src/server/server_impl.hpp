#pragma once

/**
 * @file server_impl.hpp
 * @brief Server::Impl, the state behind a Server, and the helpers it uses. Private to the library.
 */

#include "handler_pool.hpp"
#include "mcp/detail/async/memory_transport_read.hpp"
#include "mcp/detail/async/spawn_awaitable.hpp"
#include "mcp/detail/async/strand_executor.hpp"
#include "mcp/detail/constants.hpp"
#include "mcp/detail/deprecation_reporter.hpp"
#include "mcp/detail/json/peer_json.hpp"
#include "mcp/detail/net/param_headers.hpp"
#include "mcp/detail/protocol/middleware_rejection.hpp"
#include "mcp/detail/protocol/modern_methods.hpp"
#include "mcp/detail/protocol/notification_budget.hpp"
#include "mcp/detail/protocol/request_meta.hpp"
#include "mcp/detail/server/run_control.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"
#include "mcp/detail/text/encoding.hpp"
#include "mcp/detail/transport/session_pipe.hpp"
#include "mrtr.hpp"
#include "task_run.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/detail/json_dump.hpp>
#include <mcp/protocol/extensions.hpp>
#include <mcp/protocol/schema.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/memory.hpp>
#include <mcp/transport/stdio.hpp>
#include "mcp/detail/async/serialized_transport_writer.hpp"
#include "mcp/detail/text/secure_random.hpp"

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Patterns used here:
//   [int64-id]           Keep request IDs as int64_t across suspensions; rebuild string after.
//   [string_view]        Use std::string_view (trivially copyable) for coroutine method params.
//   [scope-before-await] Build SSO-risky objects in {}, serialise to wire string, then co_await.
//   [wire-builders]      make_result_wire/make_error_wire are synchronous helpers;
//                        do NOT convert them to Task<T> coroutines.
//
// These conventions are the only guard. GCC 11 is also sensitive to the shape of the coroutine
// frames, so an unrelated refactor can move a failure in or out of existence; a green ubuntu-22.04
// means the bug is not currently tripped, not that it is fixed.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

/// The arguments a tool's input schema mirrors into `Mcp-Param-*` headers, with the compiled schema
/// they were read from.
struct ToolParamHeaders {
    CompiledSchema schema;
    std::vector<detail::ParamHeader> headers;
};

/// A schema as tools/list writes it, with the compiled schema it was written from.
struct ListedSchemaText {
    CompiledSchema schema;
    std::string text;
};

struct CompiledToolSchemas {
    CompiledSchema input;
    std::optional<CompiledSchema> output;
    std::shared_ptr<const ToolParamHeaders> param_headers;
};

/// What is derived from a compiled schema, by the identity of its compiled form. CompiledSchema
/// returns one shared compiled form for a repeat of a schema, so servers built per session that
/// register the same tools derive each value once. An entry is held weakly: while it is alive it
/// holds its compiled schema, so the address it is found by cannot be reused by another schema, and
/// it goes with the last holder.
template <typename Derived>
class BySchemaCache {
   public:
    std::shared_ptr<const Derived> find(const CompiledSchema& schema) {
        std::scoped_lock lock(mutex_);
        const auto entry = entries_.find(&schema.schema());
        return entry == entries_.end() ? nullptr : entry->second.lock();
    }

    void insert(const std::shared_ptr<const Derived>& derived) {
        std::scoped_lock lock(mutex_);
        if (entries_.size() >= g_max_entries) {
            std::erase_if(entries_, [](const auto& entry) { return entry.second.expired(); });
            if (entries_.size() >= g_max_entries) {
                return;
            }
        }
        entries_.insert_or_assign(&derived->schema.schema(), derived);
    }

   private:
    /// As many as CompiledSchema keeps, so every schema it shares can be found.
    static constexpr std::size_t g_max_entries = 256;

    std::mutex mutex_;
    std::unordered_map<const nlohmann::json*, std::weak_ptr<const Derived>> entries_;
};

inline BySchemaCache<ToolParamHeaders>& tool_param_headers_cache() {
    static BySchemaCache<ToolParamHeaders> s_cache;
    return s_cache;
}

inline BySchemaCache<ListedSchemaText>& listed_schema_text_cache() {
    static BySchemaCache<ListedSchemaText> s_cache;
    return s_cache;
}

/// Throws when the `structuredContent` of @p result does not conform to @p schema; the dispatcher
/// answers that with mcp::constants::jsonrpc::g_INTERNAL_ERROR, since the tool broke its own declared
/// contract.
inline void check_structured_content(const CompiledSchema& schema, std::string_view tool_name,
                                     const nlohmann::json& result) {
    const auto content = result.find("structuredContent");
    if (content == result.end()) {
        return;
    }
    const auto validation = schema.validate(*content);
    if (validation.valid) {
        return;
    }
    std::string text = "Tool '" + std::string(tool_name) +
                       "' returned structuredContent that does not conform to its outputSchema: ";
    for (std::size_t i = 0; i < validation.violations.size(); ++i) {
        const auto& violation = validation.violations[i];
        if (i > 0) {
            text += "; ";
        }
        if (!violation.instance_path.empty()) {
            text += violation.instance_path + ": ";
        }
        text += violation.message;
    }
    throw std::runtime_error(text);
}

/// The tool result for @p arguments that do not conform to @p schema, or nothing when they do.
inline std::optional<nlohmann::json> argument_rejection(const CompiledSchema& schema,
                                                        const nlohmann::json& arguments) {
    auto result = schema.validate(arguments);
    if (result.valid) {
        return std::nullopt;
    }
    std::string text = "Invalid arguments: ";
    for (std::size_t i = 0; i < result.violations.size(); ++i) {
        const auto& violation = result.violations[i];
        if (i > 0) {
            text += "; ";
        }
        if (!violation.instance_path.empty()) {
            text += violation.instance_path + ": ";
        }
        text += violation.message;
    }
    return nlohmann::json(make_tool_error_result(std::move(text)));
}

/// Compiles the schemas of the tool @p name, refusing the tool when either is not a JSON Schema the
/// SDK can use. Tool arguments are always objects, so the input schema must also declare
/// `"type": "object"`, as the 2025-11-25 and 2026-07-28 schemas require, and its `x-mcp-header`
/// annotations must meet the constraints 2026-07-28 sets on them.
inline CompiledToolSchemas compile_tool_schemas(const std::string& name,
                                                const nlohmann::json& input_schema,
                                                const nlohmann::json* output_schema) {
    const auto refuse = [&name](SchemaError::Category category, std::string_view field,
                                std::string_view problem) {
        return SchemaError(category, "Tool '" + detail::sanitize_for_diagnostics(name) + "' " +
                                         std::string(field) + ": " + std::string(problem));
    };
    const auto compile = [&](const nlohmann::json& schema, std::string_view field) {
        if (!schema.is_object()) {
            throw refuse(SchemaError::Category::eInvalidSchema, field, "must be a JSON object");
        }
        try {
            return CompiledSchema::compile(schema);
        } catch (const SchemaError& error) {
            throw refuse(error.category(), field, error.what());
        }
    };

    // Checked before the schema is compiled, which reports a pattern it cannot use only once it
    // has found no other problem; the same goes for the x-mcp-header annotations.
    if (!input_schema.is_object()) {
        throw refuse(SchemaError::Category::eInvalidSchema, "inputSchema", "must be a JSON object");
    }
    const auto type = input_schema.find("type");
    if (type == input_schema.end() || *type != "object") {
        throw refuse(SchemaError::Category::eInvalidSchema, "inputSchema",
                     R"(must declare "type": "object")");
    }
    std::optional<CompiledSchema> compiled;
    std::optional<SchemaError> compile_error;
    try {
        compiled = CompiledSchema::compile(input_schema);
    } catch (const SchemaError& error) {
        compile_error = refuse(error.category(), "inputSchema", error.what());
    }
    std::shared_ptr<const ToolParamHeaders> param_headers;
    if (compiled) {
        param_headers = tool_param_headers_cache().find(*compiled);
    }
    if (!param_headers) {
        try {
            auto headers = detail::collect_param_headers(input_schema);
            if (compiled) {
                param_headers = std::make_shared<const ToolParamHeaders>(
                    ToolParamHeaders{.schema = *compiled, .headers = std::move(headers)});
            }
        } catch (const std::invalid_argument& error) {
            throw refuse(SchemaError::Category::eInvalidSchema, "inputSchema", error.what());
        }
        if (param_headers) {
            tool_param_headers_cache().insert(param_headers);
        }
    }
    if (compile_error) {
        throw *compile_error;
    }
    auto input = std::move(*compiled);
    std::optional<CompiledSchema> output;
    if (output_schema != nullptr) {
        output = compile(*output_schema, "outputSchema");
    }
    return {
        .input = std::move(input),
        .output = std::move(output),
        .param_headers = std::move(param_headers),
    };
}

/// One state of a URI template's matching automaton.
struct UriMatchState {
    enum class Kind : std::uint8_t {
        eLiteral,  ///< Consumes `literal`.
        eExcept,   ///< Consumes one character that is not in `excluded`.
        eSplit,    ///< Consumes nothing and continues at both `next` and `alternative`.
        eMatch,    ///< The whole URI matched if the input ends here.
    };
    Kind kind{Kind::eMatch};
    char literal{0};
    std::string_view excluded;
    std::size_t next{0};
    std::size_t alternative{0};
};

struct UriTemplatePattern {
    /// The template's language written as a regular expression. Two templates with the same
    /// source match the same URIs.
    std::string source;
    std::vector<UriMatchState> matcher;
};

/// Does `uri` match the automaton `states` from start to end? Every state the input can be in is
/// followed at once, as in Thompson's construction, so the work is linear in the URI's length and
/// needs no recursion, whatever the template. A backtracking regular-expression engine can take
/// time exponential in the number of expressions on a URI that almost matches.
inline bool uri_template_matches(const std::vector<UriMatchState>& states, std::string_view uri) {
    constexpr auto unseen = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> seen(states.size(), unseen);
    std::vector<std::size_t> current;
    std::vector<std::size_t> following;
    std::vector<std::size_t> pending;
    // Adds `start` and every state reachable from it without consuming input; `step` marks the
    // states already added for this input position.
    const auto add = [&](std::vector<std::size_t>& set, std::size_t start, std::size_t step) {
        pending.push_back(start);
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (seen[index] == step) {
                continue;
            }
            seen[index] = step;
            if (states[index].kind == UriMatchState::Kind::eSplit) {
                pending.push_back(states[index].alternative);
                pending.push_back(states[index].next);
            } else {
                set.push_back(index);
            }
        }
    };

    add(current, 0, 0);
    for (std::size_t position = 0; position < uri.size(); ++position) {
        const char ch = uri[position];
        following.clear();
        for (const auto index : current) {
            const auto& state = states[index];
            const bool consumes =
                (state.kind == UriMatchState::Kind::eLiteral && state.literal == ch) ||
                (state.kind == UriMatchState::Kind::eExcept &&
                 state.excluded.find(ch) == std::string_view::npos);
            if (consumes) {
                add(following, state.next, position + 1);
            }
        }
        if (following.empty()) {
            return false;
        }
        current.swap(following);
    }
    return std::ranges::any_of(
        current, [&](std::size_t index) { return states[index].kind == UriMatchState::Kind::eMatch; });
}

inline bool is_uri_template_operator(char ch) {
    constexpr std::string_view operators = "+#./;?&";
    return operators.find(ch) != std::string_view::npos;
}

inline char uri_expression_operator(std::string_view expression) {
    return !expression.empty() && is_uri_template_operator(expression.front()) ? expression.front() : 0;
}

// Characters an expression's expansion can never contain. When the next expression's leading
// literal is one of them, it marks the boundary between the two unambiguously.
inline std::string_view uri_expression_excluded_characters(char expression_operator) {
    switch (expression_operator) {
        case '#':
        case '?':
        case '&':
            return "#";
        case '+':
        case '/':
        case ';':
            return "?#";
        default:
            return "/?#";
    }
}

inline void validate_uri_template_variables(std::string_view expression) {
    if (!expression.empty() && is_uri_template_operator(expression.front())) {
        expression.remove_prefix(1);
    }
    if (expression.empty()) {
        throw std::invalid_argument("Resource URI template contains an empty expression");
    }

    // A comma separates two variables, so one at either end of the list leaves an empty variable.
    for (;;) {
        auto separator = expression.find(',');
        auto variable = expression.substr(0, separator);
        if (variable.empty()) {
            throw std::invalid_argument("Resource URI template contains an empty variable");
        }

        auto modifier = variable.find_first_of(":*");
        auto name = variable.substr(0, modifier);
        if (name.empty() || !std::ranges::all_of(name, [](char ch) {
                auto uch = static_cast<unsigned char>(ch);
                return std::isalnum(uch) != 0 || ch == '_' || ch == '.' || ch == '%';
            })) {
            throw std::invalid_argument("Resource URI template contains an invalid variable name");
        }

        if (separator == std::string_view::npos) {
            break;
        }
        expression.remove_prefix(separator + 1);
    }
}

inline void append_regex_literal(std::string& pattern, char ch) {
    constexpr std::string_view metacharacters = R"(\.^$|()[]*+?{})";
    if (metacharacters.find(ch) != std::string_view::npos) {
        pattern.push_back('\\');
    }
    pattern.push_back(ch);
}

inline void append_literal_state(std::vector<UriMatchState>& states, char ch) {
    states.push_back({
        .kind = UriMatchState::Kind::eLiteral,
        .literal = ch,
        .excluded = {},
        .next = states.size() + 1,
        .alternative = 0,
    });
}

/// `[^excluded]+`
inline void append_run_states(std::vector<UriMatchState>& states, std::string_view excluded) {
    const auto first = states.size();
    states.push_back({
        .kind = UriMatchState::Kind::eExcept,
        .literal = 0,
        .excluded = excluded,
        .next = first + 1,
        .alternative = 0,
    });
    states.push_back({
        .kind = UriMatchState::Kind::eSplit,
        .literal = 0,
        .excluded = {},
        .next = first,
        .alternative = first + 2,
    });
}

/// `(?:<prefix>[^excluded]+)?` when `non_empty`, else `(?:<prefix>[^excluded]*)?`.
inline void append_prefixed_run_states(std::vector<UriMatchState>& states, char prefix,
                                       std::string_view excluded, bool non_empty) {
    const auto first = states.size();
    const auto end = first + 4;
    states.push_back({
        .kind = UriMatchState::Kind::eSplit,
        .literal = 0,
        .excluded = {},
        .next = first + 1,
        .alternative = end,
    });
    states.push_back({
        .kind = UriMatchState::Kind::eLiteral,
        .literal = prefix,
        .excluded = {},
        .next = first + 2,
        .alternative = 0,
    });
    if (non_empty) {
        states.push_back({
            .kind = UriMatchState::Kind::eExcept,
            .literal = 0,
            .excluded = excluded,
            .next = first + 3,
            .alternative = 0,
        });
        states.push_back({
            .kind = UriMatchState::Kind::eSplit,
            .literal = 0,
            .excluded = {},
            .next = first + 2,
            .alternative = end,
        });
    } else {
        states.push_back({
            .kind = UriMatchState::Kind::eSplit,
            .literal = 0,
            .excluded = {},
            .next = first + 3,
            .alternative = end,
        });
        states.push_back({
            .kind = UriMatchState::Kind::eExcept,
            .literal = 0,
            .excluded = excluded,
            .next = first + 2,
            .alternative = 0,
        });
    }
}

/// `(?:\.[^/?#]+)*`, the label expansion's `(?:\.[^/?#]+(?:\.[^/?#]+)*)?`.
inline void append_label_states(std::vector<UriMatchState>& states) {
    const auto first = states.size();
    states.push_back({
        .kind = UriMatchState::Kind::eSplit,
        .literal = 0,
        .excluded = {},
        .next = first + 1,
        .alternative = first + 4,
    });
    states.push_back({
        .kind = UriMatchState::Kind::eLiteral,
        .literal = '.',
        .excluded = {},
        .next = first + 2,
        .alternative = 0,
    });
    states.push_back({
        .kind = UriMatchState::Kind::eExcept,
        .literal = 0,
        .excluded = "/?#",
        .next = first + 3,
        .alternative = 0,
    });
    states.push_back({
        .kind = UriMatchState::Kind::eSplit,
        .literal = 0,
        .excluded = {},
        .next = first + 2,
        .alternative = first,
    });
}

inline void append_uri_expression_pattern(std::string& pattern, std::vector<UriMatchState>& states,
                                          std::string_view expression) {
    validate_uri_template_variables(expression);
    char expression_operator = is_uri_template_operator(expression.front()) ? expression.front() : 0;

    switch (expression_operator) {
        case '+':
            pattern += R"([^?#]+)";
            append_run_states(states, "?#");
            break;
        case '#':
            pattern += R"((?:#[^#]*)?)";
            append_prefixed_run_states(states, '#', "#", false);
            break;
        case '.':
            pattern += R"((?:\.[^/?#]+(?:\.[^/?#]+)*)?)";
            append_label_states(states);
            break;
        case '/':
            pattern += R"((?:/[^?#]+)?)";
            append_prefixed_run_states(states, '/', "?#", true);
            break;
        case ';':
            pattern += R"((?:;[^?#]*)?)";
            append_prefixed_run_states(states, ';', "?#", false);
            break;
        case '?':
            pattern += R"((?:\?[^#]*)?)";
            append_prefixed_run_states(states, '?', "#", false);
            break;
        case '&':
            pattern += R"((?:&[^#]*)?)";
            append_prefixed_run_states(states, '&', "#", false);
            break;
        default:
            pattern += R"([^/?#]+)";
            append_run_states(states, "/?#");
            break;
    }
}

inline UriTemplatePattern compile_uri_template(std::string_view uri_template) {
    if (uri_template.empty()) {
        throw std::invalid_argument("Resource URI template must not be empty");
    }

    std::string pattern = "^";
    std::vector<UriMatchState> states;
    // The operator of the previous token, when that token was an expression.
    std::optional<char> previous_operator;
    for (std::size_t index = 0; index < uri_template.size();) {
        if (uri_template[index] == '}') {
            throw std::invalid_argument("Resource URI template contains an unmatched '}'");
        }
        if (uri_template[index] != '{') {
            append_regex_literal(pattern, uri_template[index]);
            append_literal_state(states, uri_template[index]);
            previous_operator.reset();
            ++index;
            continue;
        }

        auto close = uri_template.find('}', index + 1);
        if (close == std::string_view::npos) {
            throw std::invalid_argument("Resource URI template contains an unmatched '{'");
        }
        auto expression = uri_template.substr(index + 1, close - index - 1);
        const char expression_operator = uri_expression_operator(expression);
        // Two expressions with nothing between them compile to adjacent runs. The boundary is
        // decidable only when the second expands with a leading literal (every operator but '+'
        // does) that the first can never contain, as in {+path}{?query} or {id}{#section}.
        // Otherwise any split of the input between the two would do, so the variables' values
        // are ambiguous: reject it.
        if (previous_operator) {
            const bool has_leading_literal = expression_operator != 0 && expression_operator != '+';
            if (!has_leading_literal ||
                uri_expression_excluded_characters(*previous_operator).find(expression_operator) ==
                    std::string_view::npos) {
                throw std::invalid_argument(
                    "Resource URI template must separate adjacent expressions with a literal");
            }
        }
        append_uri_expression_pattern(pattern, states, expression);
        previous_operator = expression_operator;
        index = close + 1;
    }
    pattern += '$';
    states.push_back({
        .kind = UriMatchState::Kind::eMatch,
        .literal = 0,
        .excluded = {},
        .next = 0,
        .alternative = 0,
    });

    return UriTemplatePattern{.source = std::move(pattern), .matcher = std::move(states)};
}

// Before 2026-07-28 structuredContent is an object; a 2026-07-28 response may hold any JSON value.
inline void validate_call_tool_result(const nlohmann::json& result, bool modern, bool decode = true) {
    if (!result.is_object() || !result.contains("content")) {
        throw std::invalid_argument("Raw tool result must be a CallToolResult object with content");
    }
    if (!modern && result.contains("structuredContent") &&
        !result.at("structuredContent").is_object()) {
        throw std::invalid_argument("CallToolResult structuredContent must be a JSON object");
    }
    if (result.contains("_meta") && !result.at("_meta").is_object()) {
        throw std::invalid_argument("CallToolResult _meta must be a JSON object");
    }

    if (decode) {
        static_cast<void>(result.get<CallToolResult>());
    }
}

// Whether a handler's value is already a serialized CallToolResult: an object made only of
// CallToolResult members that decodes as one. A bare {"content": []} is left out, because an empty
// list says nothing about which of the two the handler meant.
inline bool is_serialized_call_tool_result(const nlohmann::json& result, bool modern) {
    if (!result.is_object() || !result.contains("content") || !result.at("content").is_array()) {
        return false;
    }
    for (const auto& [key, value] : result.items()) {
        static_cast<void>(value);
        if (key != "content" && key != "structuredContent" && key != "isError" && key != "_meta") {
            return false;
        }
    }
    if (result.at("content").empty() && result.size() == 1) {
        return false;
    }
    try {
        validate_call_tool_result(result, modern);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

inline nlohmann::json normalize_structured_tool_result(nlohmann::json result, bool modern) {
    if (result.is_object()) {
        // A handler that built a CallToolResult is already speaking the protocol; wrapping it again
        // would bury its content blocks inside a text block.
        if (is_serialized_call_tool_result(result, modern)) {
            return result;
        }
        return nlohmann::json(make_tool_structured_result(std::move(result)));
    }
    if (result.is_string()) {
        return nlohmann::json(make_tool_text_result(result.get<std::string>()));
    }
    return nlohmann::json(make_tool_text_result(mcp::detail::dump_json(result)));
}

/// The `_meta` of a message that belongs to the subscriptions/listen request with id @p listen_id.
inline nlohmann::json subscription_meta(const nlohmann::json& listen_id) {
    nlohmann::json meta = nlohmann::json::object();
    meta[std::string(mcp::constants::meta::g_META_SUBSCRIPTION_ID)] = listen_id;
    return meta;
}

class InvalidParamsError final : public std::invalid_argument {
   public:
    using std::invalid_argument::invalid_argument;
};

template <typename Params>
Params deserialize_request_params(const nlohmann::json& message, std::string_view method) {
    try {
        return message.at("params").get<Params>();
    } catch (const nlohmann::json::exception& error) {
        throw InvalidParamsError("Invalid " + std::string(method) + " params: " + error.what());
    } catch (const std::invalid_argument& error) {
        throw InvalidParamsError("Invalid " + std::string(method) + " params: " + error.what());
    }
}

/// The members of a tools/call's params, read in place. The checks, and the exceptions they throw,
/// are those of decoding the params into a CallToolParams.
struct CallToolView {
    const std::string* name;
    const nlohmann::json* arguments;
    const nlohmann::json* meta;
};

inline CallToolView view_call_tool_params(const nlohmann::json& message) {
    static const nlohmann::json s_no_arguments = nlohmann::json::object();
    try {
        const auto& params = message.at("params");
        const auto& name = params.at("name");
        if (!name.is_string()) {
            // Throws the type error that decoding the name does.
            static_cast<void>(name.get<std::string>());
        }
        const nlohmann::json* arguments = &s_no_arguments;
        if (params.contains("arguments")) {
            if (!params.at("arguments").is_object()) {
                throw std::invalid_argument("CallToolParams arguments must be a JSON object");
            }
            arguments = &params.at("arguments");
        }
        const nlohmann::json* meta = nullptr;
        if (detail::has_json_value(params, "_meta")) {
            meta = &params.at("_meta");
        }
        return {.name = &name.get_ref<const std::string&>(), .arguments = arguments, .meta = meta};
    } catch (const nlohmann::json::exception& error) {
        throw InvalidParamsError("Invalid tools/call params: " + std::string(error.what()));
    } catch (const std::invalid_argument& error) {
        throw InvalidParamsError("Invalid tools/call params: " + std::string(error.what()));
    }
}

inline bool has_valid_request_id(const nlohmann::json& message) {
    return message.contains("id") && detail::is_request_id(message.at("id"));
}

// A peer that serializes an absent optional as an explicit null means the member is not there, and
// a null is never a usable error object. "result" gets no such treatment: JSON-RPC allows a null
// result as a legitimate empty value, so a present-but-null result is a result.
inline bool has_error_member(const nlohmann::json& message) {
    return message.contains("error") && !message.at("error").is_null();
}

/// Whether @p message, an object, has `"jsonrpc": "2.0"`.
inline bool has_jsonrpc_2(const nlohmann::json& message) {
    const auto version = message.find("jsonrpc");
    return version != message.end() && version->is_string() &&
           version->get_ref<const nlohmann::json::string_t&>() == "2.0";
}

// Runs on every request, twice on the way to its handler, so each member is looked up once.
inline const char* validate_request_envelope(const nlohmann::json& message) {
    if (!message.is_object()) {
        return "JSON-RPC request must be an object";
    }
    if (!has_jsonrpc_2(message)) {
        return "jsonrpc must be \"2.0\"";
    }
    const auto end = message.end();
    if (const auto id = message.find("id"); id == end || !detail::is_request_id(*id)) {
        return "JSON-RPC request id must be a string or integer";
    }
    if (const auto method = message.find("method"); method == end || !method->is_string()) {
        return "JSON-RPC request method must be a string";
    }
    if (const auto error = message.find("error");
        message.contains("result") || (error != end && !error->is_null())) {
        return "JSON-RPC request must not contain result or error";
    }
    if (const auto params = message.find("params"); params != end && !params->is_object()) {
        return "MCP request params must be an object";
    }
    return nullptr;
}

inline bool is_valid_notification_envelope(const nlohmann::json& message) {
    // A notification is never answered, so dropping one costs the peer a silent protocol stall
    // rather than an error. An explicit null for an absent optional is the default shape for
    // several mainstream JSON serializers, so it is read as "no params" rather than rejected.
    return message.is_object() && !message.contains("id") && message.contains("jsonrpc") &&
           message.at("jsonrpc").is_string() && message.at("jsonrpc") == "2.0" &&
           message.contains("method") && message.at("method").is_string() &&
           !message.contains("result") && !has_error_member(message) &&
           (!message.contains("params") || message.at("params").is_null() ||
            message.at("params").is_object());
}

inline bool is_valid_response_envelope(const nlohmann::json& message) {
    // An error that cannot be decoded still answers the request it names: dispatch_response
    // fails that request instead of leaving its handler waiting.
    return message.is_object() && has_valid_request_id(message) && !message.contains("method") &&
           message.contains("jsonrpc") && message.at("jsonrpc").is_string() &&
           message.at("jsonrpc") == "2.0" && message.contains("result") != has_error_member(message);
}

/// An error that answers no request it can name leaves out `id`: MCP's schema gives an error
/// response's id the type of a request id, which null is not.
inline std::string make_invalid_request_wire(const nlohmann::json& message, std::string_view reason) {
    nlohmann::json wire = {
        {"jsonrpc", "2.0"},
        {"error", {{"code", mcp::constants::jsonrpc::g_INVALID_REQUEST}, {"message", reason}}},
    };
    if (message.is_object() && has_valid_request_id(message)) {
        wire["id"] = message.at("id");
    }
    return mcp::detail::dump_json(wire);
}

inline std::string make_parse_error_wire() {
    return mcp::detail::dump_json(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"error", {{"code", mcp::constants::jsonrpc::g_PARSE_ERROR}, {"message", "Parse error"}}},
    });
}

inline bool policy_allows_legacy_version(const ServerProtocolPolicy& policy, std::string_view version) {
    if (!is_legacy_protocol_version(version)) {
        return false;
    }
    if (!policy.legacy_versions) {
        return true;
    }
    const auto& allowed = *policy.legacy_versions;
    return std::ranges::find(allowed, version) != allowed.end();
}

struct Server::PendingRequest {
    std::shared_ptr<boost::asio::steady_timer> timer;
    nlohmann::json result;
    std::optional<Error> error;
    bool completed{false};
    /// The session_epoch() it was sent in. Only that session may answer it.
    std::uint64_t epoch{0};
    /// Set for a request a handler is waiting on: cancelling that handler ends the wait.
    std::shared_ptr<std::atomic<bool>> caller_cancelled;
    /// The server stopped waiting before the client answered, so the client is told to stop.
    bool given_up{false};
};

struct Server::Session {
    std::shared_ptr<ITransport> transport;
    MemoryTransport* memory_transport = nullptr;
    /// The transport, when a StreamableHttpSessionManager hands this session its messages as JSON.
    detail::SessionPipe* session_pipe = nullptr;
    const HttpServerTransport* http_transport = nullptr;
    std::unique_ptr<boost::asio::any_io_executor> strand;
    std::shared_ptr<detail::SerializedTransportWriter> writer;
    std::map<std::string, PendingRequest> pending_requests;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> in_flight;
    /// Subscribed URI to the session_epoch() it was made in. A subscription from a session the
    /// client has since ended delivers nothing.
    std::map<std::string, std::uint64_t> subscriptions;
    /// Bytes of the URIs in `subscriptions`.
    std::size_t subscription_bytes{0};
    std::shared_ptr<boost::asio::steady_timer> drain_timer;
    std::atomic_size_t active_dispatches{0};
    /// The size of the messages being dispatched, on transports that hand over text.
    std::atomic_size_t active_dispatch_bytes{0};
    std::atomic_bool stopping{false};
    /// Open subscriptions/listen streams of this session.
    std::atomic_size_t modern_listeners{0};
};

class NullTransport : public ITransport {
   public:
    Task<std::string> read_message() override {
        throw std::runtime_error("stateless direct dispatch has no readable transport");
    }

    Task<void> write_message(std::string_view /*message*/) override { co_return; }

    void close() override {}
};

inline NullTransport& null_transport() {
    static NullTransport s_transport;
    return s_transport;
}

struct Server::Impl : std::enable_shared_from_this<Server::Impl> {
    enum class LifecycleState : std::uint8_t {
        eUninitialized,
        eAwaitingInitialized,
        eReady,
    };

    struct ToolRegistration {
        /// Called through middleware, task runs and the handler executor. For a sync handler it is
        /// sync_handler lifted into a coroutine.
        TypeErasedHandler handler;
        detail::ToolResultMode result_mode;
        CompiledToolSchemas schemas;
        ToolTaskMode task_mode{ToolTaskMode::eNever};
        ToolTaskPreparation task_preparation;
        /// The handler of a tool registered as synchronous, called directly when the call already
        /// runs on the strand the handler belongs on; empty for an async handler.
        SyncTypeErasedHandler sync_handler;
    };

    struct ResourceTemplateRegistration {
        ResourceTemplate metadata;
        std::string matcher_source;
        std::vector<UriMatchState> matcher;
        TypeErasedHandler handler;
    };

    /// Makes a new share, of the given size, of the in-flight limits a request counts against.
    using CountAgain = std::function<std::shared_ptr<void>(std::size_t)>;

    /// What a request's handler needs beyond its JSON: the per-request protocol fields of a
    /// 2026-07-28 request, its cancellation flag, and whether the request came from the session,
    /// whose transport then carries its notifications.
    struct RequestScope {
        std::shared_ptr<const RequestMetadata> meta;
        std::shared_ptr<std::atomic<bool>> cancelled;
        bool session_request{false};
        /// The `Mcp-Param-*` headers of the HTTP request, or null when it came another way.
        std::shared_ptr<const detail::ParamHeaderValues> param_headers;
        /// Where the notifications of a 2026-07-28 request go, when its transport streams them.
        MessageSender notifications;
        /// Lets the transport wake a request that waits for its cancellation.
        std::function<void(std::function<void()>)> watch_cancel;
        /// Who sent the request, or null when the transport did not say.
        std::shared_ptr<const std::string> principal;
        /// The handler's state when it serves a 2026-07-28 tools/call, prompts/get or
        /// resources/read, which may ask the client for input.
        std::shared_ptr<detail::MrtrEntry> mrtr;
        /// The request's share of the in-flight limits, for a request that belongs to no session.
        std::shared_ptr<void> in_flight;
        /// Makes a new share of the limits the request counts against (see
        /// detail::MrtrEntry::count_again).
        CountAgain count_again;
        /// The id, as JSON text, of a request of 2025-11-25 or earlier read from the session. The
        /// handler's notifications and requests are written as related to it, so a transport that
        /// gives each request a stream sends them on that request's stream.
        std::shared_ptr<const std::string> related_request;
        /// Set instead of `mrtr` when such a request is served inside the request itself, because
        /// the client declared nothing that lets it answer a request for input.
        std::shared_ptr<detail::InlineInput> inline_input;
        /// The request, when a handler that outlives it must keep it.
        std::shared_ptr<const nlohmann::json> message;
    };

    /// The progress token of a request served inside the request itself, or none. A request that
    /// was parked reports through its own state, which carries the token.
    static std::optional<ProgressToken> inline_progress_token(const RequestScope& scope) {
        if (scope.inline_input == nullptr || scope.meta == nullptr) {
            return std::nullopt;
        }
        const auto& meta = *scope.meta->meta;
        const auto token = meta.find("progressToken");
        if (token == meta.end()) {
            return std::nullopt;
        }
        try {
            return token->get<ProgressToken>();
        } catch (const nlohmann::json::exception&) {
            return std::nullopt;
        }
    }

    /// Why a subscriptions/listen stream ended.
    enum class ListenEnd : std::uint8_t {
        eOpen,
        /// The client cancelled it or its transport went away: nothing more is sent.
        eCancelled,
        /// The server closed it: the request is answered with its empty result.
        eClosed,
    };

    /// One open subscriptions/listen request: the notification types it opted in to and where its
    /// notifications go. Its timer and strand serve only to wake the request that waits for the end.
    struct Listener : std::enable_shared_from_this<Listener> {
        explicit Listener(boost::asio::strand<boost::asio::any_io_executor> strand_arg)
            : strand(std::move(strand_arg)), timer(strand) {
            timer.expires_at(std::chrono::steady_clock::time_point::max());
        }

        /// Ends the stream for @p reason unless it already ended, and wakes the request.
        void end(ListenEnd reason) {
            auto expected = ListenEnd::eOpen;
            if (state.compare_exchange_strong(expected, reason, std::memory_order_acq_rel)) {
                boost::asio::post(strand, [self = shared_from_this()] { self->timer.cancel(); });
            }
        }

        [[nodiscard]] bool open() const {
            return state.load(std::memory_order_acquire) == ListenEnd::eOpen;
        }

        /// The listen request's id as sent, which the stream's messages name.
        nlohmann::json id;
        std::string key;
        /// The session whose transport carries the stream, or null when the request came another
        /// way.
        std::shared_ptr<Session> session;
        bool tools_list_changed{false};
        bool prompts_list_changed{false};
        bool resources_list_changed{false};
        std::set<std::string> resource_uris;
        /// The tasks whose notifications/tasks the stream carries.
        std::set<std::string> task_ids;
        MessageSender sink;
        boost::asio::strand<boost::asio::any_io_executor> strand;
        boost::asio::steady_timer timer;
        /// Hands @p message to the sink unless the acknowledgment is still on its way: nothing goes
        /// out before it, so the message waits in `pending` and the request sends it afterwards.
        /// Returns whether the caller is to send it.
        bool deliver_or_defer(const std::shared_ptr<const std::string>& message) {
            std::scoped_lock lock(gate);
            if (acknowledged) {
                return true;
            }
            pending.push_back(message);
            return false;
        }

        std::mutex gate;
        bool acknowledged{false};                                 // guarded by gate
        std::vector<std::shared_ptr<const std::string>> pending;  // guarded by gate
        std::atomic<ListenEnd> state{ListenEnd::eOpen};
    };

    /// The change a notification announces, which decides the streams that receive it.
    enum class ListenTopic : std::uint8_t {
        eToolsListChanged,
        ePromptsListChanged,
        eResourcesListChanged,
        eResourceUpdated,
    };

    /// A handler waiting for the client's retry, filed under the requestState it was given.
    struct ParkedRequest {
        std::shared_ptr<detail::MrtrEntry> entry;
        std::string principal;
        std::string method;
        std::string digest;
        std::uint64_t sequence{0};
        std::shared_ptr<boost::asio::steady_timer> expiry;
        /// What the handler's copy of the request takes in memory.
        std::size_t bytes{0};
    };

    static constexpr LoggingLevel g_default_log_level = LoggingLevel::eDebug;

    /// Which HTTP session `session` is serving: its transport's count of ended sessions, or 0 for a
    /// transport without sessions. Work tagged with one value belongs to a session that is over
    /// once the count moves on.
    static std::uint64_t session_epoch(const Session& session) {
        return session.http_transport != nullptr
                   ? detail::terminated_session_count(*session.http_transport)
                   : 0;
    }

    static Task<void> skip_write() { co_return; }

    struct Registry;
    Impl() = default;
    /// A server for another session of the application whose servers share @p shared.
    explicit Impl(std::shared_ptr<Registry> shared) : registry(std::move(shared)) {}
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    /// What this server shares with the servers built for the other sessions it serves; see
    /// One caching hint per CacheTarget.
    using CacheHints = std::array<CacheHint, static_cast<std::size_t>(CacheTarget::eResourcesRead) + 1>;

    /// Registry. Declared before the references into it below, which bind to it.
    std::shared_ptr<Registry> registry = std::make_shared<Registry>();

    Implementation& server_info = registry->server_info;
    ServerCapabilities& capabilities = registry->capabilities;
    std::optional<std::string>& instructions = registry->instructions;
    CacheHints& cache_hints = registry->cache_hints;
    bool& report_server_info = registry->report_server_info;
    std::string& server_info_meta_text = registry->server_info_meta_text;
    std::once_flag& server_info_meta_once = registry->server_info_meta_once;
    const std::string& server_info_meta() const;
    /// The runs of this server in progress; each server has its own.
    std::shared_ptr<detail::RunControl> run_control = std::make_shared<detail::RunControl>();
    std::atomic<LifecycleState> lifecycle{LifecycleState::eUninitialized};
    // The transport's terminated_session_count() when the current session was initialized.
    std::atomic<std::uint64_t> terminated_sessions_at_initialize{0};
    std::atomic_bool shutdown_requested{false};
    // Set by ~Server. The implementation itself outlives the Server whenever work is still
    // in flight, so this is what tells that work its owner has gone.
    std::atomic_bool server_gone{false};

    mutable std::mutex session_mutex;
    std::shared_ptr<Session> session;
    std::atomic<int64_t> next_request_id{1};
    std::atomic<LoggingLevel> log_level{g_default_log_level};
    /// The capabilities the client declared in its initialize request, for the requests a legacy
    /// session sends it.
    mutable std::mutex client_capabilities_mutex;
    nlohmann::json client_capabilities = nlohmann::json::object();
    /// The protocol version the session's initialize request negotiated, guarded by
    /// client_capabilities_mutex. Empty before an initialize, so a server answering 2026-07-28
    /// requests, which have none, keeps it empty.
    std::string negotiated_version;
    [[nodiscard]] std::string negotiated_protocol_version() const {
        std::scoped_lock lock(client_capabilities_mutex);
        return negotiated_version;
    }
    /// Whether an initialize negotiated @p version or a later one. Versions are dates, so they
    /// compare as text.
    [[nodiscard]] bool negotiated_at_least(std::string_view version) const {
        std::scoped_lock lock(client_capabilities_mutex);
        return !negotiated_version.empty() && std::string_view{negotiated_version} >= version;
    }
    std::size_t& page_size = registry->page_size;
    std::atomic_size_t& max_in_flight_requests = registry->max_in_flight_requests;
    std::atomic_size_t& max_in_flight_bytes = registry->max_in_flight_bytes;
    /// Requests dispatched with dispatch_request_direct(), which belong to no session, and the
    /// memory they take parsed. Together they are held to the limits of one session.
    struct DirectCounters {
        std::atomic_size_t requests{0};
        std::atomic_size_t bytes{0};
    };
    std::shared_ptr<DirectCounters> direct_counters = std::make_shared<DirectCounters>();
    /// Counts a request that belongs to no session while anything holds it.
    struct CountedRequest {
        std::shared_ptr<DirectCounters> counters;
        std::size_t bytes;
        CountedRequest(std::shared_ptr<DirectCounters> shared, std::size_t size)
            : counters(std::move(shared)), bytes(size) {
            counters->requests.fetch_add(1, std::memory_order_relaxed);
            counters->bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        ~CountedRequest() {
            counters->bytes.fetch_sub(bytes, std::memory_order_relaxed);
            counters->requests.fetch_sub(1, std::memory_order_relaxed);
        }
        CountedRequest(const CountedRequest&) = delete;
        CountedRequest& operator=(const CountedRequest&) = delete;
    };
    /// Counts work against the in-flight limits of a session while anything holds it, as a
    /// dispatched message counts.
    struct CountedSessionWork {
        std::shared_ptr<Session> session;
        std::size_t bytes;
        CountedSessionWork(std::shared_ptr<Session> owner, std::size_t size)
            : session(std::move(owner)), bytes(size) {
            session->active_dispatches.fetch_add(1, std::memory_order_relaxed);
            session->active_dispatch_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        ~CountedSessionWork() {
            session->active_dispatch_bytes.fetch_sub(bytes, std::memory_order_relaxed);
            const auto remaining =
                session->active_dispatches.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining != 0 || !session->stopping.load(std::memory_order_acquire)) {
                return;
            }
            // The session's end waits on its strand for its work to drain.
            try {
                boost::asio::post(*session->strand, [owner = session] {
                    owner->drain_timer->expires_at(std::chrono::steady_clock::now());
                });
            } catch (...) {
                // Queueing allocates, and a destructor must not throw.
            }
        }
        CountedSessionWork(const CountedSessionWork&) = delete;
        CountedSessionWork& operator=(const CountedSessionWork&) = delete;
    };
    std::atomic<std::chrono::milliseconds::rep>& reverse_request_timeout_ms =
        registry->reverse_request_timeout_ms;

    CompletionHandler& completion_handler = registry->completion_handler;

    /// A registered tool as tools/list describes it. The tool's own schemas are unset: they are
    /// listed from their compiled form, which servers that register the same schema share.
    struct ListedTool {
        Tool tool;
        CompiledSchema input;
        std::optional<CompiledSchema> output;

        [[nodiscard]] nlohmann::json listed_json() const {
            nlohmann::json json_tool = tool;
            json_tool["inputSchema"] = input.schema();
            if (output) {
                json_tool["outputSchema"] = output->schema();
            }
            return json_tool;
        }
    };

    void add_tool_registration(Tool&& tool, std::string name, CompiledToolSchemas schemas,
                               TypeErasedHandler handler, detail::ToolResultMode result_mode,
                               SyncTypeErasedHandler sync_handler = {});
    /// Appends @p listed as tools/list writes it to @p text, adding the schema texts it was written
    /// from to @p held.
    static void append_listed_tool(std::string& text, const ListedTool& listed,
                                   std::vector<std::shared_ptr<const ListedSchemaText>>& held);

    std::vector<ListedTool>& tools = registry->tools;
    std::mutex& tools_text_mutex = registry->tools_text_mutex;
    std::shared_ptr<const std::string>& tools_text = registry->tools_text;
    bool& tools_text_built = registry->tools_text_built;
    std::vector<std::shared_ptr<const ListedSchemaText>>& tools_text_schemas =
        registry->tools_text_schemas;
    void forget_tools_text();
    [[nodiscard]] std::shared_ptr<const std::string> listed_tools_text();
    std::map<std::string, ToolRegistration, std::less<>>& tool_handlers = registry->tool_handlers;
    ToolValidationOptions& tool_validation = registry->tool_validation;
    std::optional<boost::asio::any_io_executor>& handler_executor = registry->handler_executor;
    std::atomic<std::uint64_t>& argument_rejections = registry->argument_rejections;
    std::atomic<std::uint64_t>& structured_content_rejections = registry->structured_content_rejections;
    NotificationRateLimit& notification_rate_limit = registry->notification_rate_limit;
    std::shared_ptr<std::atomic<std::uint64_t>>& dropped_notifications =
        registry->dropped_notifications;

    ServerProtocolPolicy& protocol_policy = registry->protocol_policy;
    std::atomic_bool& serving = registry->serving;
    std::atomic<std::uint64_t>& invalid_metadata_rejections = registry->invalid_metadata_rejections;
    std::atomic<std::uint64_t>& unsupported_version_rejections =
        registry->unsupported_version_rejections;
    std::atomic<std::uint64_t>& legacy_shaped_rejections = registry->legacy_shaped_rejections;

    mutable std::mutex mrtr_mutex;
    /// Immutable snapshot of the options; `sealing_keys` is always empty, the key material lives in
    /// the codec only.
    std::shared_ptr<const MrtrOptions> mrtr_options = std::make_shared<const MrtrOptions>();
    /// Set when mrtr_options->state_mode is sealed.
    std::shared_ptr<const detail::sealed_state::Codec> mrtr_codec;
    std::unordered_map<std::string, ParkedRequest> parked_requests;
    std::uint64_t next_park_sequence{0};
    std::shared_ptr<detail::MrtrCounters> mrtr_counters = std::make_shared<detail::MrtrCounters>();

    std::atomic_bool& tasks_enabled = registry->tasks_enabled;
    TaskOptions& task_options = registry->task_options;
    std::shared_ptr<TaskStore>& task_store = registry->task_store;
    /// The handlers of tasks that run in this process for this server's requests, by task id.
    mutable std::mutex task_runs_mutex;
    std::map<std::string, std::shared_ptr<detail::TaskRun>> task_runs;  // guarded by task_runs_mutex

    std::vector<Resource>& resources = registry->resources;
    std::map<std::string, TypeErasedHandler, std::less<>>& resource_handlers =
        registry->resource_handlers;
    std::map<std::string, TypeErasedHandler, std::less<>>& extension_methods =
        registry->extension_methods;
    std::vector<ResourceTemplateRegistration>& resource_templates = registry->resource_templates;
    std::vector<Prompt>& prompts = registry->prompts;
    std::map<std::string, TypeErasedHandler, std::less<>>& prompt_handlers = registry->prompt_handlers;
    std::vector<Middleware>& middlewares = registry->middlewares;
    SubscriptionHandler& subscribe_handler = registry->subscribe_handler;
    SubscriptionHandler& unsubscribe_handler = registry->unsubscribe_handler;

    mutable std::mutex listeners_mutex;
    std::vector<std::shared_ptr<Listener>> listeners;  // guarded by listeners_mutex

    /// What the servers of one application share when one Server serves several sessions (the
    /// servers run_http() builds for its legacy sessions share the one the application made): the
    /// identity, capabilities, registrations, settings and counters. A server made on its own has
    /// a registry of its own. The state of a session (its lifecycle, log level, requests, tasks
    /// and subscriptions) stays with each server.
    struct Registry {
        Implementation server_info;
        ServerCapabilities capabilities;
        std::optional<std::string> instructions;
        CacheHints cache_hints{};
        bool report_server_info{true};
        /// The `_meta` a 2026-07-28 result carries when it has none: the server's identity under
        /// mcp::constants::meta::g_META_SERVER_INFO, as JSON text. Written by the first such
        /// result, so a server that only serves earlier revisions never writes it; empty when the
        /// identity cannot be serialized.
        std::string server_info_meta_text;  // written once, under server_info_meta_once
        std::once_flag server_info_meta_once;
        /// Reports each deprecated feature the servers sharing this registry use, once for all of
        /// them.
        std::shared_ptr<detail::DeprecationReporter> deprecations =
            std::make_shared<detail::DeprecationReporter>();
        std::size_t page_size{0};
        std::atomic_size_t max_in_flight_requests{
            mcp::detail::constants::server::g_DEFAULT_MAX_IN_FLIGHT_REQUESTS};
        std::atomic_size_t max_in_flight_bytes{
            mcp::detail::constants::server::g_DEFAULT_MAX_IN_FLIGHT_BYTES};
        std::atomic<std::chrono::milliseconds::rep> reverse_request_timeout_ms{
            mcp::detail::constants::server::g_DEFAULT_REVERSE_REQUEST_TIMEOUT.count()};
        CompletionHandler completion_handler;
        std::vector<ListedTool> tools;
        /// The tools as JSON text, comma-separated, in registration order: what a tools/list that
        /// lists them all sends, without building the tools again. Built by the first such list
        /// after a registration, since a server built for one session may never be asked for its
        /// tools. Null when a tool could not be serialized, in which case the list is built (and
        /// fails) per request.
        std::mutex tools_text_mutex;
        std::shared_ptr<const std::string> tools_text;  // guarded by tools_text_mutex
        bool tools_text_built{false};                   // guarded by tools_text_mutex
        /// The schema texts tools_text was written from, held so that servers listing the same
        /// schemas find them; guarded by tools_text_mutex.
        std::vector<std::shared_ptr<const ListedSchemaText>> tools_text_schemas;
        std::map<std::string, ToolRegistration, std::less<>> tool_handlers;
        ToolValidationOptions tool_validation;
        /// Where tool handlers run, when that is not the executor the server runs on.
        std::optional<boost::asio::any_io_executor> handler_executor;
        std::atomic<std::uint64_t> argument_rejections{0};
        std::atomic<std::uint64_t> structured_content_rejections{0};
        /// Read unsynchronized once the server serves; see Server::set_notification_rate_limit().
        NotificationRateLimit notification_rate_limit;
        /// Shared with the budget of each request, which may outlive the server.
        std::shared_ptr<std::atomic<std::uint64_t>> dropped_notifications =
            std::make_shared<std::atomic<std::uint64_t>>(0);
        ServerProtocolPolicy protocol_policy;
        // Set by the first request and by run(); the policy is read unsynchronized after that.
        std::atomic_bool serving{false};
        std::atomic<std::uint64_t> invalid_metadata_rejections{0};
        std::atomic<std::uint64_t> unsupported_version_rejections{0};
        std::atomic<std::uint64_t> legacy_shaped_rejections{0};
        // The tasks extension; configured before the server serves its first request.
        std::atomic_bool tasks_enabled{false};
        TaskOptions task_options;
        std::shared_ptr<TaskStore> task_store;
        std::vector<Resource> resources;
        std::map<std::string, TypeErasedHandler, std::less<>> resource_handlers;
        /// Request methods of extensions, served to 2026-07-28 clients (add_extension_method).
        std::map<std::string, TypeErasedHandler, std::less<>> extension_methods;
        std::vector<ResourceTemplateRegistration> resource_templates;
        std::vector<Prompt> prompts;
        std::map<std::string, TypeErasedHandler, std::less<>> prompt_handlers;
        std::vector<Middleware> middlewares;
        SubscriptionHandler subscribe_handler;
        SubscriptionHandler unsubscribe_handler;
        /// The servers that share this registry, so that a notification about it reaches the
        /// sessions of all of them.
        std::mutex members_mutex;
        std::vector<std::weak_ptr<Impl>> members;  // guarded by members_mutex
    };

    /// A resource or resource template implies the resources capability; one the application set
    /// stays as it is.
    void imply_resources() {
        if (!capabilities.resources) {
            capabilities.resources = ServerCapabilities::ResourcesCapability{};
        }
    }

    /// The servers sharing this one's registry that are still alive, this one included.
    [[nodiscard]] std::vector<std::shared_ptr<Impl>> registry_members() {
        std::vector<std::shared_ptr<Impl>> alive;
        std::scoped_lock lock(registry->members_mutex);
        auto& members = registry->members;
        std::erase_if(members, [&alive](const std::weak_ptr<Impl>& member) {
            auto locked = member.lock();
            if (!locked) {
                return true;
            }
            alive.push_back(std::move(locked));
            return false;
        });
        return alive;
    }

    // The request path lives here rather than on Server so that work still in flight when a
    // Server is destroyed has something valid to run against. Holders keep the implementation
    // alive; Server is only the handle the application owns.
    [[nodiscard]] std::shared_ptr<Session> session_snapshot() const;

    /// Whether the client's initialize request declared `sampling.context`.
    [[nodiscard]] bool client_declares_sampling_context() const {
        std::scoped_lock lock(client_capabilities_mutex);
        const auto sampling = client_capabilities.find("sampling");
        return sampling != client_capabilities.end() && sampling->is_object() &&
               sampling->contains("context");
    }
    /// Whether the session's transport has ended a session since the current one was initialized.
    [[nodiscard]] bool transport_started_new_session() const;
    void reset_session(const std::shared_ptr<Session>& session);
    void abandon_session_work(const std::shared_ptr<Session>& session);
    /// Fails the server's own requests that are still waiting for an answer.
    static void abandon_pending_requests(Session& session);

    /// Returns once no request of @p session is running.
    static Task<void> wait_for_dispatches(Session& session);
    static Task<void> run(std::shared_ptr<Impl> impl, std::shared_ptr<ITransport> transport,
                          boost::asio::any_io_executor executor);
    /// Serves @p session until its transport ends: registers it, reads it, then ends it.
    Task<void> run_session(std::shared_ptr<Session> session);
    /// The read loop of @p session: reads each message and starts its dispatch, until the
    /// transport closes.
    Task<void> read_session(std::shared_ptr<Session> session);
    /// A message read while @p session is at its limit: a request is refused, and anything else,
    /// such as an answer to the server's own request, is dispatched in place.
    Task<void> handle_past_limit(Session& session, nlohmann::json json_msg);
    /// Ends @p session once its read loop has: stops its work, closes its transport and waits for
    /// its dispatches.
    Task<void> end_session(std::shared_ptr<Session> session);
    /// Whether a message of @p message_bytes must not start another dispatch in the background.
    bool session_is_full(const Session& session, std::size_t message_bytes) const;
    /// Writes @p answer, made by the read loop itself, without holding up reading while it can.
    Task<void> answer_from_read_loop(Session& session, std::shared_ptr<const std::string> answer);
    static bool is_identified_request(const nlohmann::json& json_msg);
    Task<void> dispatch(nlohmann::json json_msg);
    Task<void> notify_resource_updated(const std::string& uri) const;
    Task<void> dispatch_on_strand(nlohmann::json json_msg,
                                  std::shared_ptr<const std::string> principal = nullptr);
    /// dispatch_request_wire() for a request that belongs to no session, refused past the
    /// in-flight limits. The request counts until its work ends, even when its client has gone:
    /// a 2026-07-28 handler counts while it runs, not while it is parked, and again when it goes
    /// on after being given up.
    Task<std::string> dispatch_direct(nlohmann::json json_msg,
                                      std::shared_ptr<std::atomic<bool>> cancelled,
                                      std::shared_ptr<const detail::ParamHeaderValues> param_headers,
                                      MessageSender notifications,
                                      std::function<void(std::function<void()>)> watch_cancel,
                                      std::shared_ptr<const std::string> principal);
    /// dispatch_direct() for the call state of dispatch_request_direct().
    Task<std::string> dispatch_with_context(nlohmann::json json_msg, RequestContext request_context);
    /// What an HttpServerTransport calls to have this server answer a 2026-07-28 request. Holds the
    /// server weakly, so a transport that outlives it answers with an error.
    static Task<std::string> dispatch_for_http(std::weak_ptr<Impl> weak, nlohmann::json json_msg,
                                               RequestContext request_context);
    void dispatch_notification(const nlohmann::json& json_msg);
    void dispatch_response(const nlohmann::json& json_msg) const;
    /// Sends a request to the client. @p related_request, when set, is the id as JSON text of the
    /// client request whose handler asks (see RequestScope::related_request).
    Task<nlohmann::json> send_request(std::shared_ptr<std::atomic<bool>> caller_cancelled,
                                      const std::string& method,
                                      const std::optional<nlohmann::json>& params,
                                      std::shared_ptr<const std::string> related_request = nullptr);
    static Task<nlohmann::json> await_reverse_response(
        std::shared_ptr<Session> session, std::shared_ptr<const std::string> wire, std::int64_t id,
        std::chrono::milliseconds timeout, std::shared_ptr<std::atomic<bool>> cancelled,
        std::shared_ptr<const std::string> related_request);
    static Task<nlohmann::json> await_reverse_response_on_strand(
        std::shared_ptr<Session> session, std::shared_ptr<const std::string> wire, std::int64_t id,
        std::chrono::milliseconds timeout, std::shared_ptr<std::atomic<bool>> cancelled,
        std::shared_ptr<const std::string> related_request);
    /// Writes @p wire to the session, as related to @p related_request when that is set.
    static Task<void> write_to_session(const Session& session, std::shared_ptr<const std::string> wire,
                                       const std::shared_ptr<const std::string>& related_request);
    Task<void> send_notification(const std::string& method,
                                 const std::optional<nlohmann::json>& params) const;
    /// send_notification() as part of a notification to every session sharing the registry: a
    /// session that cannot take it, such as one closing meanwhile, misses it without stopping the
    /// notification of the others.
    Task<void> send_broadcast(const std::string& method) const;
    /// notify_resource_updated() in the same way.
    Task<void> broadcast_resource_updated(const std::string& uri) const;
    static Task<void> notify_resource_updated_on_strand(std::shared_ptr<Session> session,
                                                        std::shared_ptr<const std::string> uri);

    Task<void> dispatch_request(nlohmann::json json_msg, std::shared_ptr<const std::string> principal);
    /// @p checked_modern, when set, says that the caller found the envelope valid and whether the
    /// request names its protocol version in _meta.
    Task<std::string> dispatch_request_wire(
        nlohmann::json incoming, bool enforce_lifecycle,
        std::shared_ptr<std::atomic<bool>> cancelled = nullptr,
        std::shared_ptr<const detail::ParamHeaderValues> param_headers = nullptr,
        MessageSender notifications = {}, std::function<void(std::function<void()>)> watch_cancel = {},
        std::shared_ptr<const std::string> principal = nullptr,
        std::shared_ptr<void> in_flight = nullptr, CountAgain count_again = {},
        std::optional<bool> checked_modern = std::nullopt);
    Task<std::string> dispatch_modern_wire(const nlohmann::json& json_msg, std::string_view method,
                                           RequestScope scope);
    /// Where the notifications of a request go, given how it arrived. Sets @p transport and
    /// @p epoch to the session's transport and session when that is the route.
    static MessageSender notification_route(
        std::shared_ptr<std::atomic<bool>> cancelled,
        const std::shared_ptr<const RequestMetadata>& request_meta, bool session_request,
        MessageSender notifications, const std::shared_ptr<Session>& session, ITransport** transport,
        std::uint64_t* epoch, std::shared_ptr<const std::string> related_request = nullptr);
    Task<std::string> handle_initialize_wire(const nlohmann::json& json_msg, bool update_lifecycle);
    Task<std::string> handle_shutdown_wire(const nlohmann::json& json_msg);
    static Task<std::string> handle_ping_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_discover_wire(const nlohmann::json& json_msg,
                                           const RequestScope& scope) const;
    Task<std::string> handle_extension_wire(const nlohmann::json& json_msg, const RequestScope& scope,
                                            const TypeErasedHandler& handler);
    Task<std::string> handle_tools_call_wire(const nlohmann::json& json_msg, const RequestScope& scope);
    Task<std::string> handle_tools_list_wire(const nlohmann::json& json_msg, const RequestScope& scope);
    Task<std::string> handle_resources_list_wire(const nlohmann::json& json_msg,
                                                 const RequestScope& scope);
    Task<std::string> handle_resources_read_wire(const nlohmann::json& json_msg,
                                                 const RequestScope& scope);
    Task<std::string> handle_resource_templates_list_wire(const nlohmann::json& json_msg,
                                                          const RequestScope& scope);
    Task<std::string> handle_subscribe_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_unsubscribe_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_prompts_list_wire(const nlohmann::json& json_msg,
                                               const RequestScope& scope);
    Task<std::string> handle_prompts_get_wire(const nlohmann::json& json_msg,
                                              const RequestScope& scope);
    Task<std::string> handle_set_level_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_complete_wire(const nlohmann::json& json_msg, const RequestScope& scope);
    /// Runs the task preparation of @p tool, through the middleware when there is any, in the
    /// context of the request @p scope. Sets @p prepared to what the preparation returned; it
    /// stays empty when the preparation did not run to its end, in which case the returned
    /// result answers the call.
    Task<nlohmann::json> prepare_task_call(const ToolRegistration& tool, CallToolParams params,
                                           std::shared_ptr<std::atomic<bool>> cancelled,
                                           std::optional<ProgressToken> progress_token,
                                           const RequestScope& scope,
                                           std::optional<TaskPreparation>& prepared);
    /// Checks @p arguments against the input schema and calls the preparation of @p tool, and reports
    /// what it threw like call_tool_handler().
    Task<nlohmann::json> call_task_preparation(const ToolRegistration& tool, Context& ctx,
                                               const nlohmann::json& arguments,
                                               std::optional<TaskPreparation>& prepared);
    Task<nlohmann::json> invoke_tool_impl(CallToolParams params,
                                          std::shared_ptr<std::atomic<bool>> cancelled,
                                          std::optional<ProgressToken> progress_token,
                                          std::shared_ptr<const RequestMetadata> request_meta = nullptr,
                                          bool session_request = true, MessageSender notifications = {},
                                          std::shared_ptr<const std::string> principal = nullptr,
                                          std::shared_ptr<detail::MrtrEntry> mrtr = nullptr,
                                          std::shared_ptr<const std::string> related_request = nullptr,
                                          std::shared_ptr<detail::InlineInput> inline_input = nullptr,
                                          std::shared_ptr<detail::TaskRun> task_run = nullptr);
    /// invoke_tool_impl for a server without middleware, which needs neither the parameters
    /// object nor a handler of its own: the tool is called with @p arguments as they are.
    Task<nlohmann::json> invoke_tool_direct(const std::string& name, const nlohmann::json& arguments,
                                            std::shared_ptr<std::atomic<bool>> cancelled,
                                            std::optional<ProgressToken> progress_token,
                                            std::shared_ptr<const RequestMetadata> request_meta,
                                            bool session_request, MessageSender notifications,
                                            std::shared_ptr<const std::string> principal,
                                            std::shared_ptr<detail::MrtrEntry> mrtr,
                                            std::shared_ptr<const std::string> related_request,
                                            std::shared_ptr<detail::InlineInput> inline_input,
                                            std::shared_ptr<detail::TaskRun> task_run = nullptr);
    /// Calls @p handler on a strand of its own, over the handler executor when one is set, and
    /// otherwise over the executor the caller runs on unless that is a strand already. A handler
    /// may fan out with `co_await (a || b)` or `co_await (a && b)`, which Boost.Asio makes safe
    /// only when every branch runs one at a time. On the handler executor, @p ctx then reaches the
    /// client through the caller's executor (detail::ContextRouting).
    Task<nlohmann::json> call_on_handler_strand(const TypeErasedHandler& handler, Context& ctx,
                                                const nlohmann::json& arguments);
    /// Checks @p arguments against the tool's input schema and runs its handler. A tool that
    /// throws has failed at its own job, which the protocol reports inside the result as isError.
    Task<nlohmann::json> call_tool_handler(const ToolRegistration& tool, Context& ctx,
                                           const nlohmann::json& arguments);
    /// Validates or normalizes the result of tool @p name and checks it against its outputSchema.
    void finish_tool_result(const ToolRegistration& tool, const std::string& name,
                            nlohmann::json& handler_result, bool modern);
    /// `session_request` says whether the request was read from the session. A 2026-07-28 request
    /// that was not has no route to the client through the session's transport. @p notifications,
    /// when set, receives the notifications of a 2026-07-28 request in place of the session's
    /// transport. @p principal is who sent the request, or null when unknown. @p mrtr, when set,
    /// routes the handler's questions to the client and its notifications through the request
    /// that currently carries it. @p related_request is RequestScope::related_request. @p task_run,
    /// when set, makes the context that of a task's handler: its questions go to the task's
    /// inputRequests, its progress to the task's status message, and it sends no notifications.
    Context make_context(std::shared_ptr<std::atomic<bool>> cancelled = nullptr,
                         std::optional<ProgressToken> progress_token = std::nullopt,
                         std::shared_ptr<const RequestMetadata> request_meta = nullptr,
                         bool session_request = true, MessageSender notifications = {},
                         std::shared_ptr<const std::string> principal = nullptr,
                         const std::shared_ptr<detail::MrtrEntry>& mrtr = nullptr,
                         const std::shared_ptr<const std::string>& related_request = nullptr,
                         const std::shared_ptr<detail::InlineInput>& inline_input = nullptr,
                         const std::shared_ptr<detail::TaskRun>& task_run = nullptr);
    TypeErasedHandler build_middleware_chain(TypeErasedHandler final_handler);
    /// Calls @p final_handler through the middleware, which without any is a plain call. The
    /// handler must stay where it is until the call ends: one in a node of a std::map does.
    Task<nlohmann::json> call_through_middleware(const TypeErasedHandler& final_handler, Context& ctx,
                                                 const nlohmann::json& params);
    // [gcc11-sso: wire-builders] DO NOT convert to Task<T>.
    static std::string make_result_wire(const RequestId& id, const nlohmann::json& result);
    /// The response to @p id whose result is @p result_text, JSON text already serialized.
    static std::string make_result_text_wire(const RequestId& id, std::string_view result_text);
    /// The response to a request: for a 2026-07-28 request, @p result with the members that
    /// revision adds to every result (resultType, and the server's identity when reported) and,
    /// for a cacheable one, the hints of @p target. Members the result already has are kept.
    /// A result of an earlier revision is sent as it is.
    std::string make_result_wire(const RequestId& id, const RequestScope& scope, nlohmann::json result,
                                 std::optional<CacheTarget> target = std::nullopt) const;
    std::string make_result_wire(const RequestId& id,
                                 const std::shared_ptr<const RequestMetadata>& request_meta,
                                 nlohmann::json result, std::optional<CacheTarget> target) const;
    static std::string make_error_wire(const RequestId& id, int code, std::string message);
    static std::string make_error_wire(const RequestId& id, int code, std::string message,
                                       nlohmann::json data);
    /// The versions this server serves, oldest first: the legacy versions initialize may
    /// negotiate, then the versions served per request.
    [[nodiscard]] std::vector<std::string> supported_versions() const;
    /// -32601 for an initialize sent to a server that serves no legacy version, naming the
    /// versions it serves in the message and in `data.supported`.
    [[nodiscard]] std::string initialize_not_served_wire(const RequestId& id) const;
    [[nodiscard]] std::string unsupported_version_wire(const RequestId& id,
                                                       std::string requested) const;
    std::optional<PaginationSlice> paginate(std::size_t total, const nlohmann::json& json_msg) const;

    /// Serves a 2026-07-28 tools/call, prompts/get or resources/read, whose handler may ask the
    /// client for input: the request is then answered input_required, and a retry carrying its
    /// requestState resumes the handler.
    Task<std::string> dispatch_mrtr(const nlohmann::json& json_msg, std::string_view method,
                                    RequestScope scope);
    Task<std::string> serve_mrtr_method(const nlohmann::json& json_msg, const RequestScope& scope);
    /// What a retry's `Mcp-Param-*` headers disagree with, for the method that mirrors arguments.
    std::optional<std::string> retry_header_mismatch(const nlohmann::json& json_msg,
                                                     std::string_view method,
                                                     const RequestScope& scope) const;
    /// Serves a request of that kind for a client that declared no capability for answering a
    /// request for input: the handler runs in the request, and a question it asks is refused as a
    /// parked handler's would be.
    Task<std::string> serve_without_parking(const nlohmann::json& json_msg, RequestScope scope);
    static Task<void> run_mrtr_handler(std::shared_ptr<Impl> impl,
                                       std::shared_ptr<detail::MrtrEntry> entry,
                                       std::shared_ptr<const nlohmann::json> message,
                                       RequestScope scope);
    /// Files @p entry under a new requestState, giving up older handlers past the limits.
    std::string park(const std::shared_ptr<detail::MrtrEntry>& entry,
                     const std::shared_ptr<const std::string>& principal, std::string_view method,
                     std::string digest, std::size_t bytes);
    /// The state of a sealed retry, or nothing when the retry is refused. Counts the outcome.
    std::optional<detail::MrtrSealedState> open_sealed_state(
        const detail::sealed_state::Codec& codec, const MrtrOptions& options, const std::string& token,
        const std::shared_ptr<const std::string>& principal, std::string_view method,
        const std::string& digest);
    /// Takes the handler filed under @p token for a retry, or returns null when the retry may not
    /// resume it.
    std::shared_ptr<detail::MrtrEntry> take_parked(const std::string& token,
                                                   const std::shared_ptr<const std::string>& principal,
                                                   std::string_view method, const std::string& digest);
    void evict_parked(const std::string& token, MrtrEvictionReason reason);
    void finish_eviction(const ParkedRequest& victim, MrtrEvictionReason reason,
                         const MrtrOptions& options);
    void abandon_all_parked();

    /// Serves tasks/get, tasks/update and tasks/cancel.
    Task<std::string> handle_task_wire(const nlohmann::json& json_msg, std::string_view method,
                                       const RequestScope& scope);
    /// The answer to a task request; sets @p changed to the task's new state when the request
    /// changed it.
    std::string serve_task_request(const nlohmann::json& json_msg, std::string_view method,
                                   const RequestScope& scope,
                                   std::shared_ptr<const TaskRecord>& changed);
    /// Creates the task for a tools/call of a client that declared the tasks extension, starts its
    /// handler apart from the request, and returns the answer carrying the task's handle.
    /// The task's handler is called with @p arguments.
    std::string start_task(const nlohmann::json& json_msg, const RequestScope& scope,
                           const CallToolView& call, const nlohmann::json& arguments,
                           const boost::asio::any_io_executor& executor);
    /// Whether @p owner or the server as a whole already runs as many task handlers as the limits
    /// allow. The caller holds task_runs_mutex.
    bool task_limit_reached_locked(const std::string& owner) const;
    /// Runs the handler of a task and records its outcome.
    static Task<void> run_task_worker(std::shared_ptr<Impl> impl, std::shared_ptr<detail::TaskRun> run,
                                      CallToolParams params,
                                      std::shared_ptr<const RequestMetadata> request_meta,
                                      std::shared_ptr<const std::string> principal,
                                      std::optional<std::chrono::milliseconds> ttl);
    /// The notifications/tasks message that tells @p listener the state of @p record.
    static std::shared_ptr<const std::string> task_notification_wire(const TaskRecord& record,
                                                                     const Listener& listener);
    /// Sends the state of a task to the subscriptions/listen streams that asked for it.
    static Task<void> notify_task(std::shared_ptr<Impl> impl, std::shared_ptr<const TaskRecord> record);
    static Task<void> notify_task_if_alive(std::weak_ptr<Impl> weak,
                                           std::shared_ptr<const TaskRecord> record);
    /// Cancels the handlers of the tasks that run in this process.
    void abandon_tasks();
    [[nodiscard]] static std::string missing_tasks_extension_wire(const RequestId& id);

    Task<std::string> handle_listen_wire(const nlohmann::json& json_msg, const RequestScope& scope);
    /// Sends the notification for @p topic on every open subscriptions/listen stream that opted in
    /// to it; @p uri names the resource of a resources/updated.
    Task<void> notify_listeners(ListenTopic topic, std::shared_ptr<const std::string> uri);
    /// Ends the streams of @p reason's kind: all of them, or those of @p session, or the one of
    /// @p session whose request id is @p key.
    void end_listeners(ListenEnd reason, const std::shared_ptr<Session>& session = nullptr,
                       const std::string* key = nullptr);
    void remove_listener(const std::shared_ptr<Listener>& listener);
    /// Whether the session gets the plain list-changed notifications of 2025-11-25 and earlier. A
    /// session that only serves 2026-07-28 requests gets what its streams opted in to instead.
    [[nodiscard]] bool legacy_broadcast_wanted() const;
    static Task<void> wait_for_listener_end(std::shared_ptr<Listener> listener);
};

}  // namespace mcp
