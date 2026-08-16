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

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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
    for (std::size_t index = 0; index < uri_template.size();) {
        if (uri_template[index] == '}') {
            throw std::invalid_argument("Resource URI template contains an unmatched '}'");
        }
        if (uri_template[index] != '{') {
            append_regex_literal(pattern, uri_template[index]);
            ++index;
            continue;
        }

        auto close = uri_template.find('}', index + 1);
        if (close == std::string_view::npos) {
            throw std::invalid_argument("Resource URI template contains an unmatched '{'");
        }
        auto expression = uri_template.substr(index + 1, close - index - 1);
        append_uri_expression_pattern(pattern, expression);
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

nlohmann::json normalize_structured_tool_result(nlohmann::json result) {
    if (result.is_object()) {
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
    if (message.contains("result") || message.contains("error")) {
        return "JSON-RPC request must not contain result or error";
    }
    if (message.contains("params") && !message.at("params").is_object()) {
        return "MCP request params must be an object";
    }
    return nullptr;
}

bool is_valid_notification_envelope(const nlohmann::json& message) {
    return message.is_object() && !message.contains("id") && message.contains("jsonrpc") &&
           message.at("jsonrpc").is_string() && message.at("jsonrpc") == "2.0" &&
           message.contains("method") && message.at("method").is_string() &&
           !message.contains("result") && !message.contains("error") &&
           (!message.contains("params") || message.at("params").is_object());
}

bool is_valid_response_envelope(const nlohmann::json& message) {
    if (!message.is_object() || !has_valid_request_id(message) || message.contains("method") ||
        !message.contains("jsonrpc") || !message.at("jsonrpc").is_string() ||
        message.at("jsonrpc") != "2.0" || (message.contains("result") == message.contains("error"))) {
        return false;
    }

    if (!message.contains("error")) {
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

struct Server::Impl {
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
    std::atomic<LifecycleState> lifecycle{LifecycleState::eUninitialized};
    std::atomic_bool shutdown_requested{false};

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
};

Server::Server(const Implementation& server_info, const ServerCapabilities& capabilities)
    : impl_(std::make_unique<Impl>()) {
    impl_->server_info = server_info;
    impl_->capabilities = capabilities;
}

Server::~Server() { reset_session(); }

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
    auto session = session_snapshot();
    if (!session || !session->transport || !session->strand ||
        session->stopping.load(std::memory_order_acquire)) {
        throw std::runtime_error("reverse RPC is unavailable in stateless direct dispatch");
    }

    // [gcc11-sso: int64-id] DO NOT change id to std::string.
    const int64_t id = impl_->next_request_id.fetch_add(1, std::memory_order_relaxed);
    JSONRPCRequest request;
    request.id = RequestId{std::to_string(id)};
    request.method = method;
    request.params = params;
    auto wire = std::make_shared<const std::string>(nlohmann::json(std::move(request)).dump());
    return await_reverse_response(std::move(session), std::move(wire), id);
}

Task<nlohmann::json> Server::await_reverse_response(std::shared_ptr<Session> session,
                                                    std::shared_ptr<const std::string> wire,
                                                    int64_t id) {
    auto strand = *session->strand;
    // A caller's awaitable keeps its original executor across an awaited post. Launch the
    // complete correlation lifecycle on the session strand so map and timer state stay confined.
    return boost::asio::co_spawn(
        strand, await_reverse_response_on_strand(std::move(session), std::move(wire), id),
        boost::asio::use_awaitable);
}

Task<nlohmann::json> Server::await_reverse_response_on_strand(std::shared_ptr<Session> session,
                                                              std::shared_ptr<const std::string> wire,
                                                              int64_t id) {
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
        throw std::runtime_error("JSON-RPC error " + std::to_string(error->code) + ": " +
                                 error->message);
    }

    co_return json_result;
}

Task<nlohmann::json> Server::invoke_tool(const std::string& tool_name, const nlohmann::json& args) {
    co_return co_await invoke_tool_impl(CallToolParams{tool_name, args, std::nullopt}, nullptr,
                                        std::nullopt);
}

Task<void> Server::run(std::shared_ptr<ITransport> transport, boost::asio::any_io_executor executor) {
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
    co_await boost::asio::co_spawn(strand, run_session(std::move(session)), boost::asio::use_awaitable);
}

Task<void> Server::run_session(std::shared_ptr<Session> session) {
    {
        std::lock_guard lock(impl_->session_mutex);
        if (impl_->session) {
            throw std::runtime_error("Server already has an active session");
        }
        impl_->session = session;
    }

    impl_->lifecycle.store(Impl::LifecycleState::eUninitialized, std::memory_order_relaxed);
    impl_->shutdown_requested.store(false, std::memory_order_relaxed);

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
                [this, session, json_msg = std::move(json_msg)]() mutable -> Task<void> {
                    try {
                        co_await dispatch_on_strand(std::move(json_msg));
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

    session->stopping.store(true, std::memory_order_release);
    for (auto& [id, cancelled] : session->in_flight) {
        static_cast<void>(id);
        cancelled->store(true, std::memory_order_relaxed);
    }
    for (auto& [id, pending] : session->pending_requests) {
        if (!pending.completed) {
            pending.error = Error{g_CONNECTION_CLOSED, "Server session closed", std::nullopt};
            pending.completed = true;
        }
        static_cast<void>(id);
        pending.timer->cancel();
    }
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

    reset_session(session);
}

Task<void> Server::dispatch(nlohmann::json json_msg) {
    auto session = session_snapshot();
    if (session && session->strand) {
        auto strand = *session->strand;
        co_await boost::asio::co_spawn(strand, dispatch_on_strand(std::move(json_msg)),
                                       boost::asio::use_awaitable);
        co_return;
    }

    co_await dispatch_on_strand(std::move(json_msg));
}

Task<void> Server::dispatch_on_strand(nlohmann::json json_msg) {
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
        (json_msg.contains("result") || json_msg.contains("error"))) {
        dispatch_response(json_msg);
        co_return;
    }

    // Anything that is neither a notification nor a response is request-shaped. Route invalid
    // values through request validation so the peer receives -32600 instead of a silent drop.
    co_await dispatch_request(std::move(json_msg));
}

Task<std::string> Server::dispatch_request_direct(nlohmann::json json_msg) {
    co_return co_await dispatch_request_wire(std::move(json_msg), false);
}

Context Server::make_context(std::shared_ptr<std::atomic<bool>> cancelled,
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
            [this](std::string method, std::optional<nlohmann::json> params) -> Task<nlohmann::json> {
                co_return co_await send_request(std::move(method), std::move(params));
            },
            std::move(cancelled),
            std::move(progress_token),
            &impl_->log_level,
            std::move(message_sender)};
}

// Invalid parameter decoding is reported as -32602. Exceptions raised after decoding (including
// handler and middleware failures) are reported as -32603.
Task<void> Server::dispatch_request(nlohmann::json json_msg) {
    auto session = session_snapshot();
    if (!session || !session->writer) {
        throw std::runtime_error("server dispatch requires an active session");
    }
    co_await session->writer->write_message(co_await dispatch_request_wire(std::move(json_msg), true));
}

Task<std::string> Server::dispatch_request_wire(nlohmann::json json_msg, bool enforce_lifecycle) {
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
            if (enforce_lifecycle && impl_->lifecycle.load(std::memory_order_relaxed) !=
                                         Impl::LifecycleState::eUninitialized) {
                co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_REQUEST,
                                          "Server has already been initialized");
            }
            co_return co_await handle_initialize_wire(json_msg, enforce_lifecycle);
        }

        if (method == "ping") {
            co_return co_await handle_ping_wire(json_msg);
        }

        if (enforce_lifecycle &&
            impl_->lifecycle.load(std::memory_order_relaxed) != Impl::LifecycleState::eReady) {
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
            error_payload = error.what();
        }
    }

    if (!error_payload.is_null()) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), error_code,
                                  error_payload.get<std::string>());
    }

    co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INTERNAL_ERROR,
                              "Request produced no response");
}

void Server::dispatch_notification(const nlohmann::json& json_msg) {
    if (!is_valid_notification_envelope(json_msg)) {
        return;
    }
    auto method = json_msg.at("method").get<std::string>();

    if (method == "notifications/initialized") {
        auto expected = Impl::LifecycleState::eAwaitingInitialized;
        impl_->lifecycle.compare_exchange_strong(expected, Impl::LifecycleState::eReady,
                                                 std::memory_order_relaxed);
        return;
    }

    if (method == "notifications/cancelled") {
        auto session = session_snapshot();
        if (impl_->lifecycle.load(std::memory_order_relaxed) != Impl::LifecycleState::eReady ||
            !session) {
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

void Server::dispatch_response(const nlohmann::json& json_msg) {
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

    if (json_msg.contains("error")) {
        it->second.error = json_msg.at("error").get<Error>();
    } else if (json_msg.contains("result")) {
        it->second.result = json_msg.at("result");
    }

    it->second.completed = true;
    it->second.timer->cancel();
}

Task<void> Server::handle_initialize(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("server initialization requires an active session");
    }
    co_await session->writer->write_message(co_await handle_initialize_wire(json_msg, true));
}

Task<std::string> Server::handle_initialize_wire(const nlohmann::json& json_msg,
                                                 bool update_lifecycle) {
    auto initialize_request = deserialize_request_params<InitializeRequest>(json_msg, "initialize");

    InitializeResult init_result;
    init_result.protocolVersion =
        std::string(negotiate_protocol_version(initialize_request.protocolVersion));
    init_result.capabilities = impl_->capabilities;
    init_result.serverInfo = impl_->server_info;

    if (update_lifecycle) {
        impl_->lifecycle.store(Impl::LifecycleState::eAwaitingInitialized, std::memory_order_relaxed);
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(init_result)));
}

Task<void> Server::handle_shutdown(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("server shutdown requires an active session");
    }
    co_await session->writer->write_message(co_await handle_shutdown_wire(json_msg));
}

Task<std::string> Server::handle_shutdown_wire(const nlohmann::json& json_msg) {
    impl_->shutdown_requested.store(true, std::memory_order_relaxed);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<void> Server::handle_ping(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("server ping requires an active session");
    }
    co_await session->writer->write_message(co_await handle_ping_wire(json_msg));
}

Task<std::string> Server::handle_ping_wire(const nlohmann::json& json_msg) {
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<nlohmann::json> Server::invoke_tool_impl(CallToolParams params,
                                              std::shared_ptr<std::atomic<bool>> cancelled,
                                              std::optional<ProgressToken> progress_token) {
    auto iter = impl_->tool_handlers.find(params.name);
    if (iter == impl_->tool_handlers.end()) {
        throw std::runtime_error("Unknown tool: " + params.name);
    }

    auto ctx = make_context(std::move(cancelled), std::move(progress_token));
    nlohmann::json handler_result;

    try {
        if (impl_->middlewares.empty()) {
            handler_result = co_await iter->second.handler(ctx, params.arguments);
        } else {
            TypeErasedHandler wrapped_handler =
                [original_handler = iter->second.handler](
                    Context& inner_ctx, const nlohmann::json& full_params) -> Task<nlohmann::json> {
                auto call_params = full_params.get<CallToolParams>();
                co_return co_await original_handler(inner_ctx, call_params.arguments);
            };
            auto handler = build_middleware_chain(std::move(wrapped_handler));
            nlohmann::json params_json = params;
            handler_result = co_await handler(ctx, params_json);
        }
    } catch (const std::exception& error) {
        co_return nlohmann::json(make_tool_error_result(error.what()));
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

Task<void> Server::handle_tools_call(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("tool dispatch requires an active session");
    }
    co_await session->writer->write_message(co_await handle_tools_call_wire(json_msg));
}

Task<std::string> Server::handle_tools_call_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<CallToolParams>(json_msg, "tools/call");
    if (!impl_->tool_handlers.contains(params.name)) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                  "Unknown tool: " + params.name);
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

Task<void> Server::handle_tools_list(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("tool listing requires an active session");
    }
    co_await session->writer->write_message(co_await handle_tools_list_wire(json_msg));
}

Task<std::string> Server::handle_tools_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(impl_->tools.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListToolsResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.tools.assign(impl_->tools.begin() + static_cast<std::ptrdiff_t>(begin),
                             impl_->tools.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<void> Server::handle_resources_list(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("resource listing requires an active session");
    }
    co_await session->writer->write_message(co_await handle_resources_list_wire(json_msg));
}

Task<std::string> Server::handle_resources_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(impl_->resources.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListResourcesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resources.assign(impl_->resources.begin() + static_cast<std::ptrdiff_t>(begin),
                                 impl_->resources.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<void> Server::handle_resources_read(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("resource reading requires an active session");
    }
    co_await session->writer->write_message(co_await handle_resources_read_wire(json_msg));
}

Task<std::string> Server::handle_resources_read_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<ReadResourceRequestParams>(json_msg, "resources/read");
    auto iter = impl_->resource_handlers.find(params.uri);
    if (iter != impl_->resource_handlers.end()) {
        auto handler = build_middleware_chain(iter->second);

        nlohmann::json params_json = params;
        auto ctx = make_context();
        nlohmann::json handler_result = co_await handler(ctx, params_json);
        co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
    }

    const Impl::ResourceTemplateRegistration* match = nullptr;
    for (const auto& resource_template : impl_->resource_templates) {
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

Task<void> Server::handle_resource_templates_list(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("resource template listing requires an active session");
    }
    co_await session->writer->write_message(co_await handle_resource_templates_list_wire(json_msg));
}

Task<std::string> Server::handle_resource_templates_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(impl_->resource_templates.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListResourceTemplatesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resourceTemplates.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        list_result.resourceTemplates.push_back(impl_->resource_templates[index].metadata);
    }
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<void> Server::handle_subscribe(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("resource subscription requires an active session");
    }
    co_await session->writer->write_message(co_await handle_subscribe_wire(json_msg));
}

Task<std::string> Server::handle_subscribe_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<ResourceSubscribeParams>(json_msg, "resources/subscribe");
    auto session = session_snapshot();
    if (session) {
        session->subscriptions[params.uri] = true;
    }
    if (impl_->subscribe_handler) {
        impl_->subscribe_handler(params.uri);
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<void> Server::handle_unsubscribe(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("resource unsubscription requires an active session");
    }
    co_await session->writer->write_message(co_await handle_unsubscribe_wire(json_msg));
}

Task<std::string> Server::handle_unsubscribe_wire(const nlohmann::json& json_msg) {
    auto params =
        deserialize_request_params<ResourceUnsubscribeParams>(json_msg, "resources/unsubscribe");
    auto session = session_snapshot();
    if (session) {
        session->subscriptions.erase(params.uri);
    }
    if (impl_->unsubscribe_handler) {
        impl_->unsubscribe_handler(params.uri);
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<void> Server::handle_prompts_list(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("prompt listing requires an active session");
    }
    co_await session->writer->write_message(co_await handle_prompts_list_wire(json_msg));
}

Task<std::string> Server::handle_prompts_list_wire(const nlohmann::json& json_msg) {
    auto page = paginate(impl_->prompts.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListPromptsResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.prompts.assign(impl_->prompts.begin() + static_cast<std::ptrdiff_t>(begin),
                               impl_->prompts.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(list_result)));
}

Task<void> Server::handle_prompts_get(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("prompt retrieval requires an active session");
    }
    co_await session->writer->write_message(co_await handle_prompts_get_wire(json_msg));
}

Task<std::string> Server::handle_prompts_get_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<GetPromptRequestParams>(json_msg, "prompts/get");
    auto iter = impl_->prompt_handlers.find(params.name);
    if (iter == impl_->prompt_handlers.end()) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                  "Unknown prompt: " + params.name);
    }

    auto handler = build_middleware_chain(iter->second);

    nlohmann::json params_json = std::move(params);
    auto ctx = make_context();
    nlohmann::json handler_result = co_await handler(ctx, params_json);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
}

Task<void> Server::handle_set_level(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("logging configuration requires an active session");
    }
    co_await session->writer->write_message(co_await handle_set_level_wire(json_msg));
}

Task<std::string> Server::handle_set_level_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<SetLevelRequestParams>(json_msg, "logging/setLevel");
    impl_->log_level.store(params.level, std::memory_order_relaxed);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<void> Server::handle_complete(const nlohmann::json& json_msg) {
    auto session = session_snapshot();
    if (!session) {
        throw std::runtime_error("completion requires an active session");
    }
    co_await session->writer->write_message(co_await handle_complete_wire(json_msg));
}

Task<std::string> Server::handle_complete_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<CompleteParams>(json_msg, "completion/complete");

    if (!impl_->completion_handler) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                  "No completion handler registered");
    }

    auto complete_result = co_await impl_->completion_handler(params);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(),
                               nlohmann::json(std::move(complete_result)));
}

std::string Server::make_result_wire(const RequestId& id, nlohmann::json result) {
    JSONRPCResultResponse response;
    response.id = id;
    response.result = std::move(result);
    return nlohmann::json(std::move(response)).dump();
}

std::string Server::make_error_wire(const RequestId& id, int code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    JSONRPCErrorResponse response;
    response.id = id;
    response.error = std::move(error);
    return nlohmann::json(std::move(response)).dump();
}

Task<void> Server::send_notification(const std::string& method,
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

Task<void> Server::notify_tools_list_changed() {
    co_await send_notification("notifications/tools/list_changed", std::nullopt);
}

Task<void> Server::notify_resources_list_changed() {
    co_await send_notification("notifications/resources/list_changed", std::nullopt);
}

Task<void> Server::notify_prompts_list_changed() {
    co_await send_notification("notifications/prompts/list_changed", std::nullopt);
}

Task<void> Server::notify_resource_updated(const std::string& uri) {
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

Task<void> Server::notify_resource_updated_on_strand(std::shared_ptr<Session> session,
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

TypeErasedHandler Server::build_middleware_chain(TypeErasedHandler final_handler) {
    auto handler = std::move(final_handler);
    for (const auto& mw : std::ranges::reverse_view(impl_->middlewares)) {
        handler = [mw, next = std::move(handler)](
                      Context& ctx, const nlohmann::json& params) -> Task<nlohmann::json> {
            co_return co_await mw(ctx, params, next);
        };
    }
    return handler;
}

std::optional<Server::PaginationSlice> Server::paginate(std::size_t total,
                                                        const nlohmann::json& json_msg) {
    if (impl_->page_size == 0 || total == 0) {
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

    auto end = std::min(offset + impl_->page_size, total);
    std::optional<std::string> next_cursor;
    if (end < total) {
        next_cursor = std::to_string(end);
    }

    return PaginationSlice{offset, end, std::move(next_cursor)};
}

bool Server::has_tool_output_schema(const std::string& name) const {
    for (const auto& tool : impl_->tools) {
        if (tool.name == name) {
            return tool.outputSchema.has_value();
        }
    }
    return false;
}

std::shared_ptr<Server::Session> Server::session_snapshot() const {
    std::lock_guard lock(impl_->session_mutex);
    return impl_->session;
}

void Server::reset_session() {
    if (impl_) {
        reset_session(session_snapshot());
    }
}

void Server::reset_session(const std::shared_ptr<Session>& session) {
    if (!impl_ || !session) {
        return;
    }

    session->stopping.store(true, std::memory_order_release);
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
    if (session->transport) {
        session->transport->close();
    }

    {
        std::lock_guard lock(impl_->session_mutex);
        if (impl_->session == session) {
            impl_->session.reset();
        }
    }
}

}  // namespace mcp
