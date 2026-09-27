#include "../detail/diagnostic_text.hpp"

#include <mcp/detail/serialized_transport_writer.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/memory.hpp>

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Patterns used here:
//   [int64-id]           Keep request IDs as int64_t across suspensions; rebuild string after.
//   [string_view]        Use std::string_view (trivially copyable) for coroutine method params.
//   [scope-before-await] Build SSO-risky objects in {}, serialise to wire string, then co_await.
//   [wire-builders]      make_result_wire/make_error_wire are synchronous helpers;
//                        do NOT convert them to Task<T> coroutines.
//
// These conventions are the guard against that bug, and they are the only guard. GCC 11 is also
// sensitive to the shape of the coroutine frames themselves, so an unrelated refactor can move a
// failure in or out of existence without touching anything the conventions describe. A green
// ubuntu-22.04 therefore means the bug is not currently being tripped, not that it is fixed.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

struct UriTemplatePattern {
    std::string source;
    std::regex matcher;
};

// The input is bounded because the matcher cannot be trusted with it, not because 512 is a policy
// anyone chose for resource names. Every std::regex implementation spends stack in proportion to
// the subject's length, by an amount each decides for itself, and the smallest default thread
// stack this SDK runs on — 512 KB for a non-main thread on macOS — has to survive the longest URI
// accepted here. Real resource identifiers sit far below this. A full-length filesystem path does
// not, but no bound a backtracking matcher could be given would reach it either: matching an
// RFC 6570 level-2 template ({var}, {+var}) needs no backtracking at all, so a linear, stack-free
// segment matcher would retire this limit entirely. That replacement is deferred, not rejected.
constexpr std::size_t g_MAX_TEMPLATE_MATCH_URI_LENGTH = 512;

bool is_uri_template_operator(char ch) {
    constexpr std::string_view operators = "+#./;?&";
    return operators.find(ch) != std::string_view::npos;
}

void validate_uri_template_variables(std::string_view expression) {
    if (!expression.empty() && is_uri_template_operator(expression.front())) {
        expression.remove_prefix(1);
    }
    if (expression.empty()) {
        throw std::invalid_argument("Resource URI template contains an empty expression");
    }

    while (!expression.empty()) {
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

void append_regex_literal(std::string& pattern, char ch) {
    constexpr std::string_view metacharacters = R"(\.^$|()[]*+?{})";
    if (metacharacters.find(ch) != std::string_view::npos) {
        pattern.push_back('\\');
    }
    pattern.push_back(ch);
}

void append_uri_expression_pattern(std::string& pattern, std::string_view expression) {
    validate_uri_template_variables(expression);
    char expression_operator = is_uri_template_operator(expression.front()) ? expression.front() : 0;

    switch (expression_operator) {
        case '+':
            pattern += R"([^?#]+)";
            break;
        case '#':
            pattern += R"((?:#[^#]*)?)";
            break;
        case '.':
            pattern += R"((?:\.[^/?#]+(?:\.[^/?#]+)*)?)";
            break;
        case '/':
            pattern += R"((?:/[^?#]+)?)";
            break;
        case ';':
            pattern += R"((?:;[^?#]*)?)";
            break;
        case '?':
            pattern += R"((?:\?[^#]*)?)";
            break;
        case '&':
            pattern += R"((?:&[^#]*)?)";
            break;
        default:
            pattern += R"([^/?#]+)";
            break;
    }
}

UriTemplatePattern compile_uri_template(std::string_view uri_template) {
    if (uri_template.empty()) {
        throw std::invalid_argument("Resource URI template must not be empty");
    }

    std::string pattern = "^";
    bool previous_token_was_expression = false;
    for (std::size_t index = 0; index < uri_template.size();) {
        if (uri_template[index] == '}') {
            throw std::invalid_argument("Resource URI template contains an unmatched '}'");
        }
        if (uri_template[index] != '{') {
            append_regex_literal(pattern, uri_template[index]);
            previous_token_was_expression = false;
            ++index;
            continue;
        }

        auto close = uri_template.find('}', index + 1);
        if (close == std::string_view::npos) {
            throw std::invalid_argument("Resource URI template contains an unmatched '{'");
        }
        // Two expressions with nothing between them compile to adjacent unbounded runs, which the
        // regex engine can only resolve by backtracking over every way of splitting the input. The
        // boundary between such variables is undecidable anyway, so reject the template outright.
        if (previous_token_was_expression) {
            throw std::invalid_argument(
                "Resource URI template must separate adjacent expressions with a literal");
        }
        auto expression = uri_template.substr(index + 1, close - index - 1);
        append_uri_expression_pattern(pattern, expression);
        previous_token_was_expression = true;
        index = close + 1;
    }
    pattern += '$';

    return UriTemplatePattern{pattern, std::regex(pattern, std::regex::ECMAScript)};
}

void validate_call_tool_result(const nlohmann::json& result) {
    if (!result.is_object() || !result.contains("content")) {
        throw std::invalid_argument("Raw tool result must be a CallToolResult object with content");
    }
    if (result.contains("structuredContent") && !result.at("structuredContent").is_object()) {
        throw std::invalid_argument("CallToolResult structuredContent must be a JSON object");
    }
    if (result.contains("_meta") && !result.at("_meta").is_object()) {
        throw std::invalid_argument("CallToolResult _meta must be a JSON object");
    }

    static_cast<void>(result.get<CallToolResult>());
}

bool is_serialized_call_tool_result(const nlohmann::json& result) {
    try {
        validate_call_tool_result(result);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

nlohmann::json normalize_structured_tool_result(nlohmann::json result) {
    if (result.is_object()) {
        // A handler that built a CallToolResult is already speaking the protocol; wrapping it again
        // would bury its content blocks inside a text block. Domain data that merely carries a
        // "content" key fails the full CallToolResult check and is still wrapped.
        if (is_serialized_call_tool_result(result)) {
            return result;
        }
        return nlohmann::json(make_tool_structured_result(std::move(result)));
    }
    if (result.is_string()) {
        return nlohmann::json(make_tool_text_result(result.get<std::string>()));
    }
    return nlohmann::json(make_tool_text_result(result.dump()));
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

bool has_valid_request_id(const nlohmann::json& message) {
    return message.contains("id") &&
           (message.at("id").is_string() || message.at("id").is_number_integer());
}

// A peer that serializes an absent optional as an explicit null means the member is not there, and
// a null is never a usable error object. "result" gets no such treatment: JSON-RPC allows a null
// result as a legitimate empty value, so a present-but-null result is a result.
bool has_error_member(const nlohmann::json& message) {
    return message.contains("error") && !message.at("error").is_null();
}

const char* validate_request_envelope(const nlohmann::json& message) {
    if (!message.is_object()) {
        return "JSON-RPC request must be an object";
    }
    if (!message.contains("jsonrpc") || !message.at("jsonrpc").is_string() ||
        message.at("jsonrpc") != "2.0") {
        return "jsonrpc must be \"2.0\"";
    }
    if (!has_valid_request_id(message)) {
        return "JSON-RPC request id must be a string or integer";
    }
    if (!message.contains("method") || !message.at("method").is_string()) {
        return "JSON-RPC request method must be a string";
    }
    if (message.contains("result") || has_error_member(message)) {
        return "JSON-RPC request must not contain result or error";
    }
    if (message.contains("params") && !message.at("params").is_object()) {
        return "MCP request params must be an object";
    }
    return nullptr;
}

bool is_valid_notification_envelope(const nlohmann::json& message) {
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

bool is_valid_response_envelope(const nlohmann::json& message) {
    if (!message.is_object() || !has_valid_request_id(message) || message.contains("method") ||
        !message.contains("jsonrpc") || !message.at("jsonrpc").is_string() ||
        message.at("jsonrpc") != "2.0" || (message.contains("result") == has_error_member(message))) {
        return false;
    }

    if (!has_error_member(message)) {
        return true;
    }

    try {
        static_cast<void>(message.at("error").get<Error>());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string make_invalid_request_wire(const nlohmann::json& message, std::string_view reason) {
    nlohmann::json id = nullptr;
    if (message.is_object() && has_valid_request_id(message)) {
        id = message.at("id");
    }
    return nlohmann::json{{"jsonrpc", "2.0"},
                          {"id", std::move(id)},
                          {"error", {{"code", g_INVALID_REQUEST}, {"message", reason}}}}
        .dump();
}

std::string make_parse_error_wire() {
    return nlohmann::json{{"jsonrpc", "2.0"},
                          {"id", nullptr},
                          {"error", {{"code", g_PARSE_ERROR}, {"message", "Parse error"}}}}
        .dump();
}

}  // namespace

struct Server::PendingRequest {
    std::shared_ptr<boost::asio::steady_timer> timer;
    nlohmann::json result;
    std::optional<Error> error;
    bool completed{false};
};

struct Server::Session {
    std::shared_ptr<ITransport> transport;
    MemoryTransport* memory_transport = nullptr;
    std::unique_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand;
    std::shared_ptr<detail::SerializedTransportWriter> writer;
    std::map<std::string, PendingRequest> pending_requests;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> in_flight;
    std::map<std::string, bool> subscriptions;
    std::shared_ptr<boost::asio::steady_timer> drain_timer;
    std::atomic_size_t active_dispatches{0};
    std::atomic_bool stopping{false};
};

class NullTransport : public ITransport {
   public:
    Task<std::string> read_message() override {
        throw std::runtime_error("stateless direct dispatch has no readable transport");
    }

    Task<void> write_message(std::string_view) override { co_return; }

    void close() override {}
};

NullTransport& null_transport() {
    static NullTransport transport;
    return transport;
}

struct Server::Impl : std::enable_shared_from_this<Server::Impl> {
    enum class LifecycleState : std::uint8_t {
        eUninitialized,
        eAwaitingInitialized,
        eReady,
    };

    struct ToolRegistration {
        TypeErasedHandler handler;
        detail::ToolResultMode result_mode;
    };

    struct ResourceTemplateRegistration {
        ResourceTemplate metadata;
        std::string matcher_source;
        std::regex matcher;
        TypeErasedHandler handler;
    };

    Implementation server_info;
    ServerCapabilities capabilities;
    std::optional<std::string> instructions;
    std::optional<std::int64_t> discover_ttl_ms;
    std::optional<CacheScope> discover_cache_scope;
    std::atomic<LifecycleState> lifecycle{LifecycleState::eUninitialized};
    std::atomic_bool shutdown_requested{false};
    // Set by ~Server. The implementation itself outlives the Server whenever work is still
    // in flight, so this is what tells that work its owner has gone.
    std::atomic_bool server_gone{false};

    mutable std::mutex session_mutex;
    std::shared_ptr<Session> session;
    std::atomic<int64_t> next_request_id{1};
    std::atomic<LoggingLevel> log_level{LoggingLevel::eDebug};
    std::size_t page_size{0};

    CompletionHandler completion_handler;

    std::vector<Tool> tools;
    std::map<std::string, ToolRegistration, std::less<>> tool_handlers;

    std::vector<Resource> resources;
    std::map<std::string, TypeErasedHandler, std::less<>> resource_handlers;

    std::vector<ResourceTemplateRegistration> resource_templates;

    std::vector<Prompt> prompts;
    std::map<std::string, TypeErasedHandler, std::less<>> prompt_handlers;

    std::vector<Middleware> middlewares;

    SubscriptionHandler subscribe_handler;
    SubscriptionHandler unsubscribe_handler;

    // The request path lives here rather than on Server so that work still in flight when a
    // Server is destroyed has something valid to run against. Holders keep the implementation
    // alive; Server is only the handle the application owns.
    [[nodiscard]] std::shared_ptr<Session> session_snapshot() const;
    void reset_session(const std::shared_ptr<Session>& session);
    static void abandon_session_work(const std::shared_ptr<Session>& session);

    static Task<void> run(std::shared_ptr<Impl> impl, std::shared_ptr<ITransport> transport,
                          boost::asio::any_io_executor executor);
    Task<void> run_session(std::shared_ptr<Session> session);
    Task<void> dispatch(nlohmann::json json_msg);
    Task<void> notify_resource_updated(const std::string& uri);
    Task<void> dispatch_on_strand(nlohmann::json json_msg);
    Task<void> dispatch_request(nlohmann::json json_msg);
    Task<std::string> dispatch_request_wire(nlohmann::json json_msg, bool enforce_lifecycle);
    void dispatch_notification(const nlohmann::json& json_msg);
    void dispatch_response(const nlohmann::json& json_msg);

    Task<std::string> handle_initialize_wire(const nlohmann::json& json_msg, bool update_lifecycle);
    Task<std::string> handle_shutdown_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_ping_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_discover_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_tools_call_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_tools_list_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_resources_list_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_resources_read_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_resource_templates_list_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_subscribe_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_unsubscribe_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_prompts_list_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_prompts_get_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_set_level_wire(const nlohmann::json& json_msg);
    Task<std::string> handle_complete_wire(const nlohmann::json& json_msg);

    Task<nlohmann::json> invoke_tool_impl(CallToolParams params,
                                          std::shared_ptr<std::atomic<bool>> cancelled,
                                          std::optional<ProgressToken> progress_token);
    Context make_context(std::shared_ptr<std::atomic<bool>> cancelled = nullptr,
                         std::optional<ProgressToken> progress_token = std::nullopt);
    TypeErasedHandler build_middleware_chain(TypeErasedHandler final_handler);

    Task<nlohmann::json> send_request(const std::string& method,
                                      const std::optional<nlohmann::json>& params);
    static Task<nlohmann::json> await_reverse_response(std::shared_ptr<Session> session,
                                                       std::shared_ptr<const std::string> wire,
                                                       std::int64_t id);
    static Task<nlohmann::json> await_reverse_response_on_strand(
        std::shared_ptr<Session> session, std::shared_ptr<const std::string> wire, std::int64_t id);
    Task<void> send_notification(const std::string& method,
                                 const std::optional<nlohmann::json>& params);
    Task<void> notify_resource_updated_on_strand(std::shared_ptr<Session> session,
                                                 std::shared_ptr<const std::string> uri);

    // [gcc11-sso: wire-builders] DO NOT convert to Task<T>.
    static std::string make_result_wire(const RequestId& id, nlohmann::json result);
    static std::string make_error_wire(const RequestId& id, int code, std::string message);
    std::optional<PaginationSlice> paginate(std::size_t total, const nlohmann::json& json_msg);
    [[nodiscard]] bool has_tool_output_schema(const std::string& name) const;
};

Server::Server(const Implementation& server_info, const ServerCapabilities& capabilities)
    : impl_(std::make_shared<Impl>()) {
    impl_->server_info = server_info;
    impl_->capabilities = capabilities;
}

Server::~Server() {
    if (impl_) {
        impl_->server_gone.store(true, std::memory_order_release);
    }
    reset_session();
}

void Server::register_tool(const Tool& tool, const std::string& name, TypeErasedHandler handler,
                           detail::ToolResultMode result_mode) {
    if (impl_->tool_handlers.contains(name)) {
        throw std::invalid_argument("Duplicate tool registration: " + name);
    }
    impl_->tools.push_back(tool);
    impl_->tool_handlers.emplace(name, Impl::ToolRegistration{std::move(handler), result_mode});
}

void Server::register_resource(const Resource& resource, TypeErasedHandler handler) {
    auto uri = resource.uri;
    if (impl_->resource_handlers.contains(uri)) {
        throw std::invalid_argument("Duplicate resource registration: " + uri);
    }
    impl_->resources.push_back(resource);
    impl_->resource_handlers.emplace(std::move(uri), std::move(handler));
}

void Server::register_prompt(const Prompt& prompt, TypeErasedHandler handler) {
    auto name = prompt.name;
    if (impl_->prompt_handlers.contains(name)) {
        throw std::invalid_argument("Duplicate prompt registration: " + name);
    }
    impl_->prompts.push_back(prompt);
    impl_->prompt_handlers.emplace(std::move(name), std::move(handler));
}

void Server::add_resource_template(const ResourceTemplate& tmpl) {
    register_resource_template(tmpl, {});
}

void Server::register_resource_template(const ResourceTemplate& tmpl, TypeErasedHandler handler) {
    auto pattern = compile_uri_template(tmpl.uriTemplate);
    for (const auto& registered : impl_->resource_templates) {
        if (registered.metadata.uriTemplate == tmpl.uriTemplate) {
            throw std::invalid_argument("Duplicate resource URI template: " + tmpl.uriTemplate);
        }
        if (registered.matcher_source == pattern.source) {
            throw std::invalid_argument("Ambiguous resource URI template: " + tmpl.uriTemplate);
        }
    }

    impl_->resource_templates.push_back(Impl::ResourceTemplateRegistration{
        tmpl, std::move(pattern.source), std::move(pattern.matcher), std::move(handler)});
}

void Server::use(Middleware mw) { impl_->middlewares.push_back(std::move(mw)); }

void Server::set_completion_provider(CompletionHandler handler) {
    impl_->completion_handler = std::move(handler);
}

void Server::set_page_size(std::size_t size) { impl_->page_size = size; }

void Server::set_instructions(std::string instructions) {
    impl_->instructions = std::move(instructions);
}

void Server::set_discover_ttl_ms(std::int64_t ttl_ms) {
    if (ttl_ms < 0) {
        throw std::invalid_argument("Discover ttlMs must be >= 0");
    }
    impl_->discover_ttl_ms = ttl_ms;
}

void Server::set_discover_cache_scope(CacheScope scope) { impl_->discover_cache_scope = scope; }

void Server::on_subscribe(const SubscriptionHandler& handler) { impl_->subscribe_handler = handler; }

void Server::on_unsubscribe(const SubscriptionHandler& handler) {
    impl_->unsubscribe_handler = handler;
}

bool Server::is_initialized() const {
    return impl_->lifecycle.load(std::memory_order_relaxed) != Impl::LifecycleState::eUninitialized;
}

bool Server::is_shutdown_requested() const {
    return impl_->shutdown_requested.load(std::memory_order_relaxed);
}

LoggingLevel Server::get_log_level() const { return impl_->log_level.load(std::memory_order_relaxed); }

Task<nlohmann::json> Server::send_request(const std::string& method,
                                          const std::optional<nlohmann::json>& params) {
    return impl_->send_request(method, params);
}

Task<nlohmann::json> Server::Impl::send_request(const std::string& method,
                                                const std::optional<nlohmann::json>& params) {
    // Distinct from the two failures below: a handler still running after its Server was
    // destroyed has to be able to tell that apart from a session that is merely closing and
    // from stateless dispatch, because the right response differs in each case.
    if (server_gone.load(std::memory_order_acquire)) {
        throw std::runtime_error("reverse RPC is unavailable: the Server has been destroyed");
    }
    auto session = session_snapshot();
    if (!session || !session->transport || !session->strand ||
        session->stopping.load(std::memory_order_acquire)) {
        throw std::runtime_error("reverse RPC is unavailable in stateless direct dispatch");
    }

    // [gcc11-sso: int64-id] DO NOT change id to std::string.
    const int64_t id = next_request_id.fetch_add(1, std::memory_order_relaxed);
    JSONRPCRequest request;
    request.id = RequestId{std::to_string(id)};
    request.method = method;
    request.params = params;
    auto wire = std::make_shared<const std::string>(nlohmann::json(std::move(request)).dump());
    return await_reverse_response(std::move(session), std::move(wire), id);
}

Task<nlohmann::json> Server::Impl::await_reverse_response(std::shared_ptr<Session> session,
                                                          std::shared_ptr<const std::string> wire,
                                                          int64_t id) {
    auto strand = *session->strand;
    // A caller's awaitable keeps its original executor across an awaited post. Launch the
    // complete correlation lifecycle on the session strand so map and timer state stay confined.
    return boost::asio::co_spawn(
        strand, await_reverse_response_on_strand(std::move(session), std::move(wire), id),
        boost::asio::use_awaitable);
}

Task<nlohmann::json> Server::Impl::await_reverse_response_on_strand(
    std::shared_ptr<Session> session, std::shared_ptr<const std::string> wire, int64_t id) {
    if (session->stopping.load(std::memory_order_acquire)) {
        throw std::runtime_error("server session is closing");
    }

    auto timer = std::make_shared<boost::asio::steady_timer>(*session->strand);
    timer->expires_at(std::chrono::steady_clock::time_point::max());
    {
        const auto id_key = RequestId{std::to_string(id)}.correlation_key();
        const auto [pending_it, inserted] = session->pending_requests.try_emplace(
            id_key, PendingRequest{timer, {}, std::nullopt, false});
        static_cast<void>(pending_it);
        if (!inserted) {
            throw std::runtime_error("duplicate pending request id: " + id_key);
        }
    }

    try {
        co_await session->writer->write_message(wire);
    } catch (...) {
        session->pending_requests.erase(RequestId{std::to_string(id)}.correlation_key());
        throw;
    }

    bool completed = false;
    {
        const auto id_key = RequestId{std::to_string(id)}.correlation_key();
        const auto pending_it = session->pending_requests.find(id_key);
        completed = pending_it != session->pending_requests.end() && pending_it->second.completed;
    }
    if (!completed) {
        try {
            co_await timer->async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (err.code() != boost::asio::error::operation_aborted) {
                session->pending_requests.erase(RequestId{std::to_string(id)}.correlation_key());
                throw;
            }
        }
    }

    // [gcc11-sso: int64-id] Rebuild the string only after all suspensions.
    const auto id_key = RequestId{std::to_string(id)}.correlation_key();
    auto it = session->pending_requests.find(id_key);
    if (it == session->pending_requests.end()) {
        throw std::runtime_error("pending request not found for id: " + std::to_string(id));
    }

    auto json_result = std::move(it->second.result);
    auto error = std::move(it->second.error);
    session->pending_requests.erase(it);

    if (error) {
        // Same defect as the peer-controlled text in the client's McpError, opposite direction:
        // this is the error a CLIENT returned for a server-initiated request (sampling,
        // elicitation, roots/list), stored verbatim by dispatch_response(). `message` is the
        // client's text and this diagnostic is what the server operator logs, with no
        // authorization step in the way, so it is flattened and bounded before it goes in.
        throw std::runtime_error("JSON-RPC error " + std::to_string(error->code) + ": " +
                                 detail::sanitize_for_diagnostics(error->message));
    }

    co_return json_result;
}

Task<nlohmann::json> Server::invoke_tool(const std::string& tool_name, const nlohmann::json& args) {
    return impl_->invoke_tool_impl(CallToolParams{tool_name, args, std::nullopt}, nullptr,
                                   std::nullopt);
}

Task<void> Server::run(std::shared_ptr<ITransport> transport, boost::asio::any_io_executor executor) {
    // Deliberately not a coroutine: reading impl_ here, on the caller's thread, hands the session
    // a reference that outlives this Server rather than one that is read again later.
    return Impl::run(impl_, std::move(transport), std::move(executor));
}

Task<void> Server::Impl::run(std::shared_ptr<Impl> impl, std::shared_ptr<ITransport> transport,
                             boost::asio::any_io_executor executor) {
    if (!transport) {
        throw std::invalid_argument("Server transport must not be null");
    }

    auto session = std::make_shared<Session>();
    session->transport = std::move(transport);
    session->memory_transport = dynamic_cast<MemoryTransport*>(session->transport.get());
    session->strand = std::make_unique<boost::asio::strand<boost::asio::any_io_executor>>(
        boost::asio::make_strand(executor));
    session->writer =
        std::make_shared<detail::SerializedTransportWriter>(session->transport, *session->strand);
    session->drain_timer = std::make_shared<boost::asio::steady_timer>(*session->strand);
    session->drain_timer->expires_at(std::chrono::steady_clock::time_point::max());
    auto strand = *session->strand;
    // The read loop and teardown both own session state, so run both on the session strand.
    co_await boost::asio::co_spawn(strand, impl->run_session(std::move(session)),
                                   boost::asio::use_awaitable);
}

Task<void> Server::Impl::run_session(std::shared_ptr<Session> new_session) {
    auto session = std::move(new_session);
    {
        std::lock_guard lock(session_mutex);
        if (this->session) {
            throw std::runtime_error("Server already has an active session");
        }
        this->session = session;
    }

    lifecycle.store(Impl::LifecycleState::eUninitialized, std::memory_order_relaxed);
    shutdown_requested.store(false, std::memory_order_relaxed);

    try {
        for (;;) {
            nlohmann::json json_msg;
            bool parse_failed = false;
            try {
                if (session->memory_transport) {
                    json_msg = co_await session->memory_transport->read_json();
                } else {
                    auto raw = co_await session->transport->read_message();
                    json_msg = nlohmann::json::parse(raw);
                }
            } catch (const nlohmann::json::parse_error&) {
                parse_failed = true;
            }
            if (parse_failed) {
                co_await session->writer->write_message(make_parse_error_wire());
                continue;
            }
            session->active_dispatches.fetch_add(1, std::memory_order_relaxed);
            boost::asio::co_spawn(
                *session->strand,
                [impl = shared_from_this(), session,
                 json_msg = std::move(json_msg)]() mutable -> Task<void> {
                    try {
                        co_await impl->dispatch_on_strand(std::move(json_msg));
                    } catch (...) {
                        // A failed request must not terminate the detached dispatcher.
                    }

                    const auto remaining =
                        session->active_dispatches.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (remaining == 0 && session->stopping.load(std::memory_order_acquire)) {
                        session->drain_timer->expires_at(std::chrono::steady_clock::now());
                    }
                    co_return;
                },
                boost::asio::detached);
        }
    } catch (const std::exception&) {
        // Closing a transport is the normal way to stop a session.
    }

    // Teardown below can fail — a transport may throw from close, and the drain wait rethrows any
    // error that is not cancellation. Whichever way it ends, the session must be unregistered, or
    // every later run() is refused for the lifetime of this Server.
    try {
        session->stopping.store(true, std::memory_order_release);
        abandon_session_work(session);
        session->transport->close();

        while (session->active_dispatches.load(std::memory_order_acquire) != 0) {
            session->drain_timer->expires_at(std::chrono::steady_clock::time_point::max());
            if (session->active_dispatches.load(std::memory_order_acquire) == 0) {
                break;
            }
            try {
                co_await session->drain_timer->async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    } catch (...) {
        reset_session(session);
        throw;
    }

    reset_session(session);
}

Task<void> Server::dispatch(nlohmann::json json_msg) { return impl_->dispatch(std::move(json_msg)); }

Task<void> Server::Impl::dispatch(nlohmann::json json_msg) {
    auto session = session_snapshot();
    if (session && session->strand) {
        auto strand = *session->strand;
        co_await boost::asio::co_spawn(strand, dispatch_on_strand(std::move(json_msg)),
                                       boost::asio::use_awaitable);
        co_return;
    }

    co_await dispatch_on_strand(std::move(json_msg));
}

Task<void> Server::Impl::dispatch_on_strand(nlohmann::json json_msg) {
    if (is_valid_notification_envelope(json_msg)) {
        dispatch_notification(json_msg);
        co_return;
    }

    // A method without an id is notification-shaped. Notifications never receive responses,
    // including when their envelope is malformed.
    if (json_msg.is_object() && json_msg.contains("method") && !json_msg.contains("id")) {
        co_return;
    }

    if (json_msg.is_object() && !json_msg.contains("method") &&
        (json_msg.contains("result") || has_error_member(json_msg))) {
        dispatch_response(json_msg);
        co_return;
    }

    // Anything that is neither a notification nor a response is request-shaped. Route invalid
    // values through request validation so the peer receives -32600 instead of a silent drop.
    co_await dispatch_request(std::move(json_msg));
}

Task<std::string> Server::dispatch_request_direct(nlohmann::json json_msg) {
    return impl_->dispatch_request_wire(std::move(json_msg), false);
}

Context Server::Impl::make_context(std::shared_ptr<std::atomic<bool>> cancelled,
                                   std::optional<ProgressToken> progress_token) {
    ITransport* transport = &null_transport();
    MessageSender message_sender;
    auto session = session_snapshot();
    if (session && session->transport) {
        transport = session->transport.get();
        auto writer = session->writer;
        message_sender = [writer = std::move(writer)](std::string_view message) {
            return writer->write_message(message);
        };
    }

    return {*transport,
            [impl = shared_from_this()](std::string method,
                                        std::optional<nlohmann::json> params) -> Task<nlohmann::json> {
                co_return co_await impl->send_request(std::move(method), std::move(params));
            },
            std::move(cancelled),
            std::move(progress_token),
            &log_level,
            std::move(message_sender)};
}

// Invalid parameter decoding is reported as -32602. Exceptions raised after decoding are reported
// as -32603, except from a tool handler: those become a tool result carrying isError.
Task<void> Server::Impl::dispatch_request(nlohmann::json json_msg) {
    auto session = session_snapshot();
    if (!session || !session->writer) {
        throw std::runtime_error("server dispatch requires an active session");
    }
    co_await session->writer->write_message(co_await dispatch_request_wire(std::move(json_msg), true));
}

Task<std::string> Server::Impl::dispatch_request_wire(nlohmann::json json_msg, bool enforce_lifecycle) {
    {
        const char* validation_error = validate_request_envelope(json_msg);
        if (validation_error != nullptr) {
            co_return make_invalid_request_wire(json_msg, validation_error);
        }
    }

    // [gcc11-sso: string_view] DO NOT change to std::string.
    std::string_view method = json_msg.at("method").get_ref<const nlohmann::json::string_t&>();

    // [gcc11-sso: scope-before-await] DO NOT use optional<string> for error state.
    nlohmann::json error_payload;
    int error_code = g_INTERNAL_ERROR;

    try {
        if (method == "initialize") {
            if (enforce_lifecycle &&
                lifecycle.load(std::memory_order_relaxed) != Impl::LifecycleState::eUninitialized) {
                co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_REQUEST,
                                          "Server has already been initialized");
            }
            co_return co_await handle_initialize_wire(json_msg, enforce_lifecycle);
        }

        if (method == "ping") {
            co_return co_await handle_ping_wire(json_msg);
        }

        if (method == "server/discover") {
            co_return co_await handle_discover_wire(json_msg);
        }

        if (enforce_lifecycle &&
            lifecycle.load(std::memory_order_relaxed) != Impl::LifecycleState::eReady) {
            co_return make_error_wire(
                json_msg.at("id").get<RequestId>(), g_INVALID_REQUEST,
                "Server is not ready; initialize and send notifications/initialized first");
        }

        if (method == "shutdown") {
            co_return co_await handle_shutdown_wire(json_msg);
        } else if (method == "tools/call") {
            co_return co_await handle_tools_call_wire(json_msg);
        } else if (method == "tools/list") {
            co_return co_await handle_tools_list_wire(json_msg);
        } else if (method == "resources/list") {
            co_return co_await handle_resources_list_wire(json_msg);
        } else if (method == "resources/read") {
            co_return co_await handle_resources_read_wire(json_msg);
        } else if (method == "resources/templates/list") {
            co_return co_await handle_resource_templates_list_wire(json_msg);
        } else if (method == "resources/subscribe") {
            co_return co_await handle_subscribe_wire(json_msg);
        } else if (method == "resources/unsubscribe") {
            co_return co_await handle_unsubscribe_wire(json_msg);
        } else if (method == "prompts/list") {
            co_return co_await handle_prompts_list_wire(json_msg);
        } else if (method == "prompts/get") {
            co_return co_await handle_prompts_get_wire(json_msg);
        } else if (method == "logging/setLevel") {
            co_return co_await handle_set_level_wire(json_msg);
        } else if (method == "completion/complete") {
            co_return co_await handle_complete_wire(json_msg);
        } else {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                      "Method not found: " + std::string(method));
        }
    } catch (const InvalidParamsError& error) {
        error_code = g_INVALID_PARAMS;
        error_payload = error.what();
    } catch (const std::exception& error) {
        if (json_msg.contains("id")) {
            // Handler text can carry build paths or third-party library detail, and reaches the
            // peer verbatim from here.
            error_payload = detail::sanitize_for_diagnostics(error.what());
        }
    }

    if (!error_payload.is_null()) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), error_code,
                                  error_payload.get<std::string>());
    }

    co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INTERNAL_ERROR,
                              "Request produced no response");
}

void Server::Impl::dispatch_notification(const nlohmann::json& json_msg) {
    if (!is_valid_notification_envelope(json_msg)) {
        return;
    }
    auto method = json_msg.at("method").get<std::string>();

    if (method == "notifications/initialized") {
        auto expected = Impl::LifecycleState::eAwaitingInitialized;
        lifecycle.compare_exchange_strong(expected, Impl::LifecycleState::eReady,
                                          std::memory_order_relaxed);
        return;
    }

    if (method == "notifications/cancelled") {
        auto session = session_snapshot();
        if (lifecycle.load(std::memory_order_relaxed) != Impl::LifecycleState::eReady || !session) {
            return;
        }
        if (!json_msg.contains("params")) {
            return;
        }
        CancelledNotificationParams params;
        try {
            params = json_msg.at("params").get<CancelledNotificationParams>();
        } catch (const std::exception&) {
            return;
        }
        auto id_str = params.requestId.correlation_key();

        auto it = session->in_flight.find(id_str);
        if (it != session->in_flight.end()) {
            it->second->store(true, std::memory_order_relaxed);
        }
    }
}

void Server::Impl::dispatch_response(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!is_valid_response_envelope(json_msg) || !session) {
        return;
    }

    auto id = json_msg.at("id").get<RequestId>();
    auto id_str = id.correlation_key();

    auto it = session->pending_requests.find(id_str);
    if (it == session->pending_requests.end()) {
        return;
    }

    if (has_error_member(json_msg)) {
        it->second.error = json_msg.at("error").get<Error>();
    } else if (json_msg.contains("result")) {
        it->second.result = json_msg.at("result");
    }

    it->second.completed = true;
    it->second.timer->cancel();
}

Task<std::string> Server::Impl::handle_initialize_wire(const nlohmann::json& json_msg,
                                                       bool update_lifecycle) {
    auto initialize_request = deserialize_request_params<InitializeRequest>(json_msg, "initialize");

    InitializeResult init_result;
    init_result.protocolVersion =
        std::string(negotiate_protocol_version(initialize_request.protocolVersion));
    init_result.capabilities = capabilities;
    init_result.serverInfo = server_info;
    init_result.instructions = instructions;

    if (update_lifecycle) {
        lifecycle.store(Impl::LifecycleState::eAwaitingInitialized, std::memory_order_relaxed);
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(init_result)));
}

Task<std::string> Server::Impl::handle_shutdown_wire(const nlohmann::json& json_msg) {
    shutdown_requested.store(true, std::memory_order_relaxed);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<std::string> Server::Impl::handle_ping_wire(const nlohmann::json& json_msg) {
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

// Params carry only `_meta`, whose contents (protocolVersion, clientInfo, clientCapabilities)
// are accepted but not yet interpreted. This request is a pre-gate method
// like initialize/ping: reachable with no prior state and idempotent, so it neither reads
// json_msg's params nor mutates lifecycle.
Task<std::string> Server::Impl::handle_discover_wire(const nlohmann::json& json_msg) {
    DiscoverResult discover_result;
    // resultType uses the DiscoverResult struct default ("complete"); a future revision introduces
    // a shared result-envelope helper for this field that other cacheable results will also use.
    discover_result.supportedVersions.assign(g_DISCOVERABLE_PROTOCOL_VERSIONS.begin(),
                                             g_DISCOVERABLE_PROTOCOL_VERSIONS.end());
    discover_result.capabilities = capabilities;
    discover_result.serverInfo = server_info;
    discover_result.instructions = instructions;
    // server/utilities/caching.md: servers MUST include caching hints on "complete" results,
    // server/discover listed first, and MUST provide a ttlMs >= 0. ttlMs defaults to 0 ("do not
    // cache" / immediately stale per the spec's freshness rule); an absent ttlMs "should only
    // occur in older server versions", which this SDK is not, so it is always emitted. The spec
    // does not state a default cacheScope, so this SDK defaults to the conservative choice,
    // "private" (do not assume the result is safe to share across authorization contexts),
    // until a caller explicitly opts into "public" via set_discover_cache_scope.
    discover_result.ttlMs = discover_ttl_ms.value_or(0);
    discover_result.cacheScope = discover_cache_scope.value_or(CacheScope::ePrivate);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json(discover_result));
}

Task<nlohmann::json> Server::Impl::invoke_tool_impl(CallToolParams params,
                                                    std::shared_ptr<std::atomic<bool>> cancelled,
                                                    std::optional<ProgressToken> progress_token) {
    auto iter = tool_handlers.find(params.name);
    if (iter == tool_handlers.end()) {
        // The name is chosen by whoever called: over JSON-RPC that is the client, and through the
        // public `invoke_tool` entry point it is whatever text the embedding passed in. Flatten and
        // bound it, or a name carrying CR/LF forges a line in the operator's log.
        throw std::runtime_error("Unknown tool: " + detail::sanitize_for_diagnostics(params.name));
    }

    auto ctx = make_context(std::move(cancelled), std::move(progress_token));
    nlohmann::json handler_result;

    // A tool that throws has failed at its own job, which the protocol reports inside the result as
    // isError. Middleware is not the tool: it decides whether the call may proceed at all, so it
    // runs outside this catch and a middleware failure surfaces as a JSON-RPC error instead.
    TypeErasedHandler guarded_handler = [original_handler = iter->second.handler](
                                            Context& inner_ctx,
                                            const nlohmann::json& arguments) -> Task<nlohmann::json> {
        try {
            co_return co_await original_handler(inner_ctx, arguments);
        } catch (const std::exception& error) {
            co_return nlohmann::json(
                make_tool_error_result(detail::sanitize_for_diagnostics(error.what())));
        }
    };

    if (middlewares.empty()) {
        handler_result = co_await guarded_handler(ctx, params.arguments);
    } else {
        TypeErasedHandler wrapped_handler =
            [guarded_handler](Context& inner_ctx,
                              const nlohmann::json& full_params) -> Task<nlohmann::json> {
            auto call_params = full_params.get<CallToolParams>();
            co_return co_await guarded_handler(inner_ctx, call_params.arguments);
        };
        auto handler = build_middleware_chain(std::move(wrapped_handler));
        nlohmann::json params_json = params;
        handler_result = co_await handler(ctx, params_json);
    }

    if (iter->second.result_mode == detail::ToolResultMode::eValidated) {
        validate_call_tool_result(handler_result);
    } else {
        handler_result = normalize_structured_tool_result(std::move(handler_result));
    }

    const bool is_error_result = handler_result.value("isError", false);
    if (has_tool_output_schema(params.name) && !is_error_result &&
        !handler_result.contains("structuredContent")) {
        throw std::invalid_argument("Tool with outputSchema must return structuredContent: " +
                                    params.name);
    }

    co_return handler_result;
}

Task<std::string> Server::Impl::handle_tools_call_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<CallToolParams>(json_msg, "tools/call");
    if (!tool_handlers.contains(params.name)) {
        // This, not the throw in invoke_tool_impl, is the site a remote client actually reaches:
        // the RPC path refuses before dispatch. The name is entirely the client's, so flatten and
        // bound it before it reaches the operator's log.
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                  "Unknown tool: " + detail::sanitize_for_diagnostics(params.name));
    }

    // [gcc11-sso: int64-id] request_id_str from json_msg (heap-safe); used only in in_flight map.
    auto request_id_str = json_msg.at("id").get<RequestId>().correlation_key();

    std::optional<ProgressToken> progress_token;
    try {
        if (params.meta) {
            auto& meta = *params.meta;
            if (meta.contains("progressToken")) {
                progress_token = meta.at("progressToken").get<ProgressToken>();
            }
        }
    } catch (const nlohmann::json::exception& error) {
        throw InvalidParamsError("Invalid tools/call params: " + std::string(error.what()));
    } catch (const std::invalid_argument& error) {
        throw InvalidParamsError("Invalid tools/call params: " + std::string(error.what()));
    }

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto session = session_snapshot();
    if (session) {
        session->in_flight[request_id_str] = cancelled;
    }

    try {
        nlohmann::json handler_result =
            co_await invoke_tool_impl(std::move(params), cancelled, std::move(progress_token));
        if (session) {
            session->in_flight.erase(request_id_str);
        }
        co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
    } catch (...) {
        if (session) {
            session->in_flight.erase(request_id_str);
        }
        throw;
    }
}

Task<std::string> Server::Impl::handle_tools_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(tools.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListToolsResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.tools.assign(tools.begin() + static_cast<std::ptrdiff_t>(begin),
                             tools.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<std::string> Server::Impl::handle_resources_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(resources.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListResourcesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resources.assign(resources.begin() + static_cast<std::ptrdiff_t>(begin),
                                 resources.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<std::string> Server::Impl::handle_resources_read_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<ReadResourceRequestParams>(json_msg, "resources/read");
    auto iter = resource_handlers.find(params.uri);
    if (iter != resource_handlers.end()) {
        auto handler = build_middleware_chain(iter->second);

        nlohmann::json params_json = params;
        auto ctx = make_context();
        nlohmann::json handler_result = co_await handler(ctx, params_json);
        co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
    }

    // Exact resources are found by lookup and so may be any length; only template matching is
    // length-sensitive. This reports the limit rather than "Unknown resource", so that a caller
    // whose URI is merely too long can tell that from one that names nothing.
    if (params.uri.size() > g_MAX_TEMPLATE_MATCH_URI_LENGTH) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Resource URI exceeds the " +
                                      std::to_string(g_MAX_TEMPLATE_MATCH_URI_LENGTH) +
                                      " character limit for template matching");
    }

    const Impl::ResourceTemplateRegistration* match = nullptr;
    for (const auto& resource_template : resource_templates) {
        if (!resource_template.handler) {
            continue;
        }
        if (!std::regex_match(params.uri, resource_template.matcher)) {
            continue;
        }
        if (match != nullptr) {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                      "Ambiguous resource template match: " + params.uri);
        }
        match = &resource_template;
    }

    if (match == nullptr || !match->handler) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Unknown resource: " + params.uri);
    }

    auto handler = build_middleware_chain(match->handler);

    nlohmann::json params_json = params;
    auto ctx = make_context();
    nlohmann::json handler_result = co_await handler(ctx, params_json);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
}

Task<std::string> Server::Impl::handle_resource_templates_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(resource_templates.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListResourceTemplatesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resourceTemplates.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        list_result.resourceTemplates.push_back(resource_templates[index].metadata);
    }
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<std::string> Server::Impl::handle_subscribe_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<ResourceSubscribeParams>(json_msg, "resources/subscribe");
    auto session = session_snapshot();
    if (session) {
        session->subscriptions[params.uri] = true;
    }
    if (subscribe_handler) {
        subscribe_handler(params.uri);
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<std::string> Server::Impl::handle_unsubscribe_wire(const nlohmann::json& json_msg) {
    auto params =
        deserialize_request_params<ResourceUnsubscribeParams>(json_msg, "resources/unsubscribe");
    auto session = session_snapshot();
    if (session) {
        session->subscriptions.erase(params.uri);
    }
    if (unsubscribe_handler) {
        unsubscribe_handler(params.uri);
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<std::string> Server::Impl::handle_prompts_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(prompts.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListPromptsResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.prompts.assign(prompts.begin() + static_cast<std::ptrdiff_t>(begin),
                               prompts.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<std::string> Server::Impl::handle_prompts_get_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<GetPromptRequestParams>(json_msg, "prompts/get");
    auto iter = prompt_handlers.find(params.name);
    if (iter == prompt_handlers.end()) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                  "Unknown prompt: " + params.name);
    }

    auto handler = build_middleware_chain(iter->second);

    nlohmann::json params_json = std::move(params);
    auto ctx = make_context();
    nlohmann::json handler_result = co_await handler(ctx, params_json);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
}

Task<std::string> Server::Impl::handle_set_level_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<SetLevelRequestParams>(json_msg, "logging/setLevel");
    log_level.store(params.level, std::memory_order_relaxed);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<std::string> Server::Impl::handle_complete_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<CompleteParams>(json_msg, "completion/complete");

    if (!completion_handler) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                  "No completion handler registered");
    }

    auto complete_result = co_await completion_handler(params);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(complete_result)));
}

std::string Server::Impl::make_result_wire(const RequestId& id, nlohmann::json result) {
    JSONRPCResultResponse response;
    response.id = id;
    response.result = std::move(result);
    return nlohmann::json(std::move(response)).dump();
}

std::string Server::Impl::make_error_wire(const RequestId& id, int code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    JSONRPCErrorResponse response;
    response.id = id;
    response.error = std::move(error);
    return nlohmann::json(std::move(response)).dump();
}

Task<void> Server::Impl::send_notification(const std::string& method,
                                           const std::optional<nlohmann::json>& params) {
    auto session = session_snapshot();
    if (!session || session->stopping.load(std::memory_order_acquire)) {
        co_return;
    }
    // [gcc11-sso: scope-before-await]
    std::string wire;
    {
        JSONRPCNotification notification;
        notification.method = method;
        notification.params = params;
        wire = nlohmann::json(std::move(notification)).dump();
    }
    co_await session->writer->write_message(wire);
}

// These three await rather than returning the task, and that is load-bearing. The argument is a
// temporary bound to a reference parameter of a lazy coroutine: under co_await it lives to the end
// of the full expression, which includes the await, but a plain `return` destroys it before the
// coroutine body ever runs and leaves the reference dangling. Both forms compile.
Task<void> Server::notify_tools_list_changed() {
    co_await impl_->send_notification("notifications/tools/list_changed", std::nullopt);
}

Task<void> Server::notify_resources_list_changed() {
    co_await impl_->send_notification("notifications/resources/list_changed", std::nullopt);
}

Task<void> Server::notify_prompts_list_changed() {
    co_await impl_->send_notification("notifications/prompts/list_changed", std::nullopt);
}

Task<void> Server::notify_resource_updated(const std::string& uri) {
    co_await impl_->notify_resource_updated(uri);
}

Task<void> Server::Impl::notify_resource_updated(const std::string& uri) {
    auto session = session_snapshot();
    if (!session || session->stopping.load(std::memory_order_acquire)) {
        co_return;
    }

    auto owned_uri = std::make_shared<const std::string>(uri);
    auto strand = *session->strand;
    co_await boost::asio::co_spawn(
        strand, notify_resource_updated_on_strand(std::move(session), std::move(owned_uri)),
        boost::asio::use_awaitable);
}

Task<void> Server::Impl::notify_resource_updated_on_strand(std::shared_ptr<Session> session,
                                                           std::shared_ptr<const std::string> uri) {
    if (session->stopping.load(std::memory_order_acquire)) {
        co_return;
    }

    auto it = session->subscriptions.find(*uri);
    if (it == session->subscriptions.end() || !it->second) {
        co_return;
    }

    ResourceUpdatedNotificationParams params;
    params.uri = *uri;

    JSONRPCNotification notification;
    notification.method = "notifications/resources/updated";
    nlohmann::json p = std::move(params);
    notification.params = std::move(p);

    nlohmann::json json_msg = std::move(notification);
    co_await session->writer->write_message(json_msg.dump());
}

TypeErasedHandler Server::Impl::build_middleware_chain(TypeErasedHandler final_handler) {
    auto handler = std::move(final_handler);
    for (const auto& mw : std::ranges::reverse_view(middlewares)) {
        handler = [mw, next = std::move(handler)](
                      Context& ctx, const nlohmann::json& params) -> Task<nlohmann::json> {
            co_return co_await mw(ctx, params, next);
        };
    }
    return handler;
}

std::optional<Server::PaginationSlice> Server::Impl::paginate(std::size_t total,
                                                              const nlohmann::json& json_msg) {
    if (page_size == 0 || total == 0) {
        return PaginationSlice{0, total, std::nullopt};
    }

    std::size_t offset = 0;
    if (json_msg.contains("params") && json_msg.at("params").contains("cursor")) {
        try {
            auto cursor_str = json_msg.at("params").at("cursor").get<std::string>();
            offset = std::stoull(cursor_str);
        } catch (const nlohmann::json::exception&) {
            return std::nullopt;
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        } catch (const std::out_of_range&) {
            return std::nullopt;
        }
    }

    if (offset >= total) {
        offset = 0;
    }

    auto end = std::min(offset + page_size, total);
    std::optional<std::string> next_cursor;
    if (end < total) {
        next_cursor = std::to_string(end);
    }

    return PaginationSlice{offset, end, std::move(next_cursor)};
}

bool Server::Impl::has_tool_output_schema(const std::string& name) const {
    for (const auto& tool : tools) {
        if (tool.name == name) {
            return tool.outputSchema.has_value();
        }
    }
    return false;
}

std::shared_ptr<Server::Session> Server::Impl::session_snapshot() const {
    std::lock_guard lock(session_mutex);
    return session;
}

void Server::reset_session() {
    if (impl_) {
        impl_->reset_session(impl_->session_snapshot());
    }
}

// Fails every outstanding request on a session and wakes whoever is waiting on it. Must run
// on the session strand: the request maps are plain std::maps that only the strand mutates,
// and the correlation timers belong to that strand.
void Server::Impl::abandon_session_work(const std::shared_ptr<Session>& session) {
    for (auto& [id, cancelled] : session->in_flight) {
        static_cast<void>(id);
        cancelled->store(true, std::memory_order_relaxed);
    }
    for (auto& [id, pending] : session->pending_requests) {
        static_cast<void>(id);
        if (!pending.completed) {
            pending.error = Error{g_CONNECTION_CLOSED, "Server session closed", std::nullopt};
            pending.completed = true;
        }
        pending.timer->cancel();
    }
}

void Server::Impl::reset_session(const std::shared_ptr<Session>& session) {
    if (!session) {
        return;
    }

    // Runs on whatever thread destroys the Server, so it does only what is safe from any thread:
    // `stopping` is atomic, the session pointer is mutex-guarded, and ITransport::close() is what
    // wakes a blocked reader. The request maps are plain std::maps that only the session strand
    // may touch, so abandoning their entries is handed to that strand instead.
    //
    // The hand-off is a post that is never waited on. Waiting would deadlock when the destructor
    // runs on the session strand itself, and would never return at all when the io_context is
    // stopped or was never run -- which is the ordinary way a Server reaches its destructor. On a
    // stopped context the posted work simply never runs, which is the same outcome as before:
    // cancelling a timer whose executor has nothing driving it delivers nothing either.
    session->stopping.store(true, std::memory_order_release);
    if (session->strand) {
        // Guarded for the same reason as the close below: queueing the hand-off allocates, and an
        // exception escaping ~Server would terminate the process.
        try {
            boost::asio::post(*session->strand, [session] { Impl::abandon_session_work(session); });
        } catch (const std::exception&) {
        }
    }
    if (session->transport) {
        // Unregistering the session below is what frees the Server for reuse, and it must happen
        // even for a transport that cannot close cleanly. This also runs from ~Server, where an
        // escaping exception would terminate the process.
        try {
            session->transport->close();
        } catch (const std::exception&) {
        }
    }

    {
        std::lock_guard lock(session_mutex);
        if (this->session == session) {
            this->session.reset();
        }
    }
}

}  // namespace mcp
