#include <algorithm>

#include "server_impl.hpp"

namespace methods = mcp::constants::methods;

namespace mcp {

Server::Server(const Implementation& server_info, const ServerCapabilities& capabilities)
    : Server(std::make_shared<Impl>()) {
    impl_->server_info = server_info;
    impl_->capabilities = capabilities;
}

Server::Server(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {
    std::scoped_lock lock(impl_->registry->members_mutex);
    impl_->registry->members.push_back(impl_);
}

Server::Impl::~Impl() {
    // Leaves the registry's members now: make_shared put this implementation in one allocation with
    // its control block, which the member's weak pointer would otherwise keep until the next
    // registry_members() call, one per ended session of run_http().
    std::scoped_lock lock(registry->members_mutex);
    auto& members = registry->members;
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [](const std::weak_ptr<Impl>& member) { return member.expired(); }),
                  members.end());
}

std::unique_ptr<Server> Server::session_server() const {
    // The constructor is private, which std::make_unique cannot reach.
    return std::unique_ptr<Server>(new Server(std::make_shared<Impl>(impl_->registry)));
}

std::shared_ptr<detail::RunControl> Server::run_control() const { return impl_->run_control; }

ServerProtocolPolicy Server::protocol_policy() const { return impl_->protocol_policy; }

void Server::stop() { impl_->run_control->stop_all(); }

std::uint16_t Server::bound_port() const { return impl_->run_control->port(); }

SyncServer Server::sync() { return SyncServer(*this); }

Server::~Server() {
    if (impl_) {
        impl_->server_gone.store(true, std::memory_order_release);
        impl_->end_listeners(Impl::ListenEnd::eCancelled);
        // Nothing can resume these handlers now; they unwind.
        impl_->abandon_all_parked();
        impl_->abandon_tasks();
    }
    reset_session();
}

void Server::Impl::forget_tools_text() {
    std::scoped_lock lock(tools_text_mutex);
    tools_text.reset();
    tools_text_built = false;
    tools_text_schemas.clear();
}

namespace {

/// The text of @p schema as tools/list writes it, shared with every server that lists the same
/// compiled schema. The text is added to @p held, which keeps it found while the caller lists it.
const std::string& listed_schema_text(const CompiledSchema& schema,
                                      std::vector<std::shared_ptr<const ListedSchemaText>>& held) {
    auto text = listed_schema_text_cache().find(schema);
    if (!text) {
        text = std::make_shared<const ListedSchemaText>(
            ListedSchemaText{.schema = schema, .text = mcp::detail::dump_json(schema.schema())});
        listed_schema_text_cache().insert(text);
    }
    held.push_back(text);
    return held.back()->text;
}

}  // namespace

void Server::Impl::append_listed_tool(std::string& text, const ListedTool& listed,
                                      std::vector<std::shared_ptr<const ListedSchemaText>>& held) {
    // The tool is written with a placeholder string in place of each schema, and the shared schema
    // texts are spliced in where the placeholders are. A placeholder found more than once means
    // another member holds the same text, and the tool is written whole instead.
    constexpr std::string_view input_mark = R"("\u0001mcp-input-schema")";
    constexpr std::string_view output_mark = R"("\u0001mcp-output-schema")";
    nlohmann::json shell = listed.tool;
    shell["inputSchema"] = "\x01mcp-input-schema";
    if (listed.output) {
        shell["outputSchema"] = "\x01mcp-output-schema";
    }
    const auto shell_text = mcp::detail::dump_json(shell);
    const auto once = [&shell_text](std::string_view mark) {
        const auto at = shell_text.find(mark);
        return at != std::string::npos && shell_text.rfind(mark) == at ? at : std::string::npos;
    };
    struct Splice {
        std::size_t at;
        std::string_view mark;
        const CompiledSchema* schema;
    };
    std::vector<Splice> splices{{.at = once(input_mark), .mark = input_mark, .schema = &listed.input}};
    if (listed.output) {
        splices.push_back({.at = once(output_mark), .mark = output_mark, .schema = &*listed.output});
    }
    if (std::any_of(splices.begin(), splices.end(),
                    [](const Splice& splice) { return splice.at == std::string::npos; })) {
        text += mcp::detail::dump_json(listed.listed_json());
        return;
    }
    std::sort(splices.begin(), splices.end(),
              [](const Splice& a, const Splice& b) { return a.at < b.at; });
    std::size_t copied = 0;
    for (const auto& splice : splices) {
        text.append(shell_text, copied, splice.at - copied);
        text += listed_schema_text(*splice.schema, held);
        copied = splice.at + splice.mark.size();
    }
    text.append(shell_text, copied, std::string::npos);
}

std::shared_ptr<const std::string> Server::Impl::listed_tools_text() {
    std::scoped_lock lock(tools_text_mutex);
    if (!tools_text_built) {
        tools_text_built = true;
        try {
            std::string text;
            std::vector<std::shared_ptr<const ListedSchemaText>> held;
            for (const auto& tool : tools) {
                if (!text.empty()) {
                    text += ',';
                }
                append_listed_tool(text, tool, held);
            }
            tools_text = std::make_shared<const std::string>(std::move(text));
            tools_text_schemas = std::move(held);
        } catch (const nlohmann::json::exception&) {
            tools_text.reset();
        }
    }
    return tools_text;
}

void Server::Impl::add_tool_registration(Tool&& tool, std::string name, CompiledToolSchemas schemas,
                                         TypeErasedHandler handler, detail::ToolResultMode result_mode,
                                         SyncTypeErasedHandler sync_handler) {
    forget_tools_text();
    if (!capabilities.tools) {
        capabilities.tools = ServerCapabilities::ToolsCapability{};
    }
    tool.inputSchema = nullptr;
    tool.outputSchema.reset();
    tools.push_back(
        ListedTool{.tool = std::move(tool), .input = schemas.input, .output = schemas.output});
    ToolRegistration registration{
        .handler = std::move(handler), .result_mode = result_mode, .schemas = std::move(schemas)};
    registration.sync_handler = std::move(sync_handler);
    tool_handlers.emplace(std::move(name), std::move(registration));
}

namespace {

/// A sync handler as a TypeErasedHandler, for the paths that await handlers.
TypeErasedHandler lift_sync_handler(SyncTypeErasedHandler handler) {
    if (!handler) {
        return {};
    }
    return [handler = std::move(handler)](Context& ctx,
                                          const nlohmann::json& params) -> Task<nlohmann::json> {
        co_return handler(ctx, params);
    };
}

/// Refuses a name the tools page asks servers not to give a tool: empty, longer than 128
/// characters, or holding a character other than an ASCII letter, a digit, `_`, `-` or `.`.
void check_tool_name(std::string_view name) {
    const bool allowed = !name.empty() &&
                         name.size() <= mcp::detail::constants::server::g_MAX_TOOL_NAME_LENGTH &&
                         std::ranges::all_of(name, [](char ch) {
                             return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
                         });
    if (!allowed) {
        throw std::invalid_argument("Tool name '" + detail::sanitize_for_diagnostics(name) +
                                    "' must be 1 to 128 characters from A-Z, a-z, 0-9, '_', '-' "
                                    "and '.'");
    }
}

/// Refuses a tool registration under @p name when the name breaks the naming rules or is @p taken.
void check_new_tool(const std::string& name, bool taken) {
    check_tool_name(name);
    if (taken) {
        throw std::invalid_argument("Duplicate tool registration: " + name);
    }
}

}  // namespace

void Server::register_tool(const Tool& tool, const std::string& name, TypeErasedHandler handler,
                           detail::ToolResultMode result_mode) {
    check_new_tool(name, impl_->tool_handlers.contains(name));
    auto schemas = compile_tool_schemas(tool.name, tool.inputSchema,
                                        tool.outputSchema ? &*tool.outputSchema : nullptr);
    impl_->add_tool_registration(Tool(tool), name, std::move(schemas), std::move(handler), result_mode);
}

void Server::register_tool(Tool&& tool, const nlohmann::json& input_schema,
                           const nlohmann::json* output_schema, TypeErasedHandler handler,
                           detail::ToolResultMode result_mode) {
    check_new_tool(tool.name, impl_->tool_handlers.contains(tool.name));
    auto schemas = compile_tool_schemas(tool.name, input_schema, output_schema);
    auto name = tool.name;
    impl_->add_tool_registration(std::move(tool), std::move(name), std::move(schemas),
                                 std::move(handler), result_mode);
}

void Server::register_sync_tool(const Tool& tool, SyncTypeErasedHandler handler,
                                detail::ToolResultMode result_mode) {
    check_new_tool(tool.name, impl_->tool_handlers.contains(tool.name));
    auto schemas = compile_tool_schemas(tool.name, tool.inputSchema,
                                        tool.outputSchema ? &*tool.outputSchema : nullptr);
    auto lifted = lift_sync_handler(handler);
    impl_->add_tool_registration(Tool(tool), tool.name, std::move(schemas), std::move(lifted),
                                 result_mode, std::move(handler));
}

void Server::register_sync_tool(Tool&& tool, const nlohmann::json& input_schema,
                                const nlohmann::json* output_schema, SyncTypeErasedHandler handler,
                                detail::ToolResultMode result_mode) {
    check_new_tool(tool.name, impl_->tool_handlers.contains(tool.name));
    auto schemas = compile_tool_schemas(tool.name, input_schema, output_schema);
    auto name = tool.name;
    auto lifted = lift_sync_handler(handler);
    impl_->add_tool_registration(std::move(tool), std::move(name), std::move(schemas),
                                 std::move(lifted), result_mode, std::move(handler));
}

void Server::register_sync_resource(const Resource& resource, SyncTypeErasedHandler handler) {
    register_resource(resource, lift_sync_handler(std::move(handler)));
}

void Server::register_sync_resource_template(const ResourceTemplate& tmpl,
                                             SyncTypeErasedHandler handler) {
    register_resource_template(tmpl, lift_sync_handler(std::move(handler)));
}

void Server::register_sync_prompt(const Prompt& prompt, SyncTypeErasedHandler handler) {
    register_prompt(prompt, lift_sync_handler(std::move(handler)));
}

void Server::register_resource(const Resource& resource, TypeErasedHandler handler) {
    auto uri = resource.uri;
    if (impl_->resource_handlers.contains(uri)) {
        throw std::invalid_argument("Duplicate resource registration: " + uri);
    }
    impl_->resources.push_back(resource);
    impl_->resource_handlers.emplace(std::move(uri), std::move(handler));
    impl_->imply_resources();
}

void Server::register_prompt(const Prompt& prompt, TypeErasedHandler handler) {
    auto name = prompt.name;
    if (impl_->prompt_handlers.contains(name)) {
        throw std::invalid_argument("Duplicate prompt registration: " + name);
    }
    impl_->prompts.push_back(prompt);
    impl_->prompt_handlers.emplace(std::move(name), std::move(handler));
    if (!impl_->capabilities.prompts) {
        impl_->capabilities.prompts = ServerCapabilities::PromptsCapability{};
    }
}

namespace {

/// @p resource, with its MIME type text/plain when unset.
Resource text_resource(const Resource& resource) {
    Resource described = resource;
    if (!described.mimeType) {
        described.mimeType = "text/plain";
    }
    return described;
}

/// The resources/read result of a text resource.
nlohmann::json text_contents(const Resource& resource, std::string text) {
    ReadResourceResult result;
    result.contents.emplace_back(TextResourceContents{
        .uri = resource.uri,
        .text = std::move(text),
        .mimeType = resource.mimeType,
        .meta = {},
    });
    return result;
}

/// The arguments of the prompts/get request whose params are @p params, for a prompt that
/// declares @p declared.
/// @throws InvalidParamsError When an argument the prompt requires is missing.
Server::PromptArguments prompt_arguments(const std::vector<PromptArgument>& declared,
                                         const nlohmann::json& params) {
    Server::PromptArguments arguments;
    if (const auto given = params.find("arguments"); given != params.end() && given->is_object()) {
        arguments = given->get<Server::PromptArguments>();
    }
    for (const auto& argument : declared) {
        if (argument.required.value_or(false) && !arguments.contains(argument.name)) {
            throw InvalidParamsError("Missing required argument: " + argument.name);
        }
    }
    return arguments;
}

/// The prompts/get result of a text prompt: one message from the user.
nlohmann::json text_prompt_result(std::string text, const std::optional<std::string>& description) {
    GetPromptResult result;
    result.messages.push_back(
        PromptMessage{.role = Role::eUser, .content = TextContent{.text = std::move(text)}});
    result.description = description;
    return result;
}

}  // namespace

void Server::add_text_resource(const std::string& uri, const std::string& name,
                               std::function<std::string()> read, const std::string& mime_type) {
    Resource resource;
    resource.uri = uri;
    resource.name = name;
    resource.mimeType = mime_type;
    add_text_resource(resource, std::move(read));
}

void Server::add_text_resource(const Resource& resource, std::function<std::string()> read) {
    auto described = text_resource(resource);
    register_sync_resource(described, [described, read = std::move(read)](
                                          Context& /*ctx*/, const nlohmann::json& /*params*/) {
        return text_contents(described, read());
    });
}

void Server::register_text_resource(const Resource& resource, std::function<Task<std::string>()> read) {
    auto described = text_resource(resource);
    register_resource(described,
                      [described, read = std::move(read)](
                          Context& /*ctx*/, const nlohmann::json& /*params*/) -> Task<nlohmann::json> {
                          auto text = co_await read();
                          co_return text_contents(described, std::move(text));
                      });
}

void Server::add_text_prompt(const std::string& name, const std::string& description,
                             std::vector<PromptArgument> arguments,
                             std::function<std::string(const PromptArguments&)> render) {
    Prompt prompt;
    prompt.name = name;
    prompt.description = description;
    prompt.arguments = std::move(arguments);
    add_text_prompt(prompt, std::move(render));
}

void Server::add_text_prompt(const Prompt& prompt,
                             std::function<std::string(const PromptArguments&)> render) {
    register_sync_prompt(prompt, [declared = prompt.arguments.value_or(std::vector<PromptArgument>{}),
                                  description = prompt.description, render = std::move(render)](
                                     Context& /*ctx*/, const nlohmann::json& params) {
        return text_prompt_result(render(prompt_arguments(declared, params)), description);
    });
}

void Server::register_text_prompt(const Prompt& prompt,
                                  std::function<Task<std::string>(const PromptArguments&)> render) {
    register_prompt(prompt,
                    [declared = prompt.arguments.value_or(std::vector<PromptArgument>{}),
                     description = prompt.description, render = std::move(render)](
                        Context& /*ctx*/, const nlohmann::json& params) -> Task<nlohmann::json> {
                        // [gcc11-sso] The arguments live behind a pointer, so no map of strings is kept
                        // in the frame across the co_await.
                        const auto arguments =
                            std::make_shared<const PromptArguments>(prompt_arguments(declared, params));
                        auto text = co_await render(*arguments);
                        co_return text_prompt_result(std::move(text), description);
                    });
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
        .metadata = tmpl,
        .matcher_source = std::move(pattern.source),
        .matcher = std::move(pattern.matcher),
        .handler = std::move(handler),
    });
    impl_->imply_resources();
}

void Server::use(Middleware mw) { impl_->middlewares.push_back(std::move(mw)); }

void Server::set_completion_provider(CompletionHandler handler) {
    if (handler && !impl_->capabilities.completions) {
        impl_->capabilities.completions = nlohmann::json::object();
    }
    impl_->completion_handler = std::move(handler);
}

void Server::set_handler_executor(const Executor& executor) {
    impl_->handler_executor = executor.get();
}

void Server::set_tool_validation(const ToolValidationOptions& options) {
    impl_->tool_validation = options;
}

void Server::set_notification_rate_limit(const NotificationRateLimit& limit) {
    detail::check_rate_limit(limit);
    impl_->notification_rate_limit = limit;
}

std::uint64_t Server::dropped_notifications() const {
    return impl_->dropped_notifications->load(std::memory_order_relaxed);
}

ToolValidationStats Server::tool_validation_stats() const {
    return {
        .argument_rejections = impl_->argument_rejections.load(std::memory_order_relaxed),
        .structured_content_rejections =
            impl_->structured_content_rejections.load(std::memory_order_relaxed),
    };
}

void Server::set_page_size(std::size_t size) { impl_->page_size = size; }

void Server::set_max_in_flight_bytes(std::size_t bytes) {
    if (bytes == 0) {
        throw std::invalid_argument("The in-flight memory limit must be at least 1 byte");
    }
    impl_->max_in_flight_bytes.store(bytes, std::memory_order_relaxed);
}

void Server::set_max_in_flight_requests(std::size_t limit) {
    if (limit == 0) {
        throw std::invalid_argument("The in-flight request limit must be at least 1");
    }
    impl_->max_in_flight_requests.store(limit, std::memory_order_relaxed);
}

void Server::set_reverse_request_timeout(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("The reverse request timeout must be positive");
    }
    impl_->reverse_request_timeout_ms.store(timeout.count(), std::memory_order_relaxed);
}

void Server::set_instructions(std::string instructions) {
    impl_->instructions = std::move(instructions);
}

void Server::declare_extension(const std::string& id, nlohmann::json settings) {
    if (id.empty()) {
        throw std::invalid_argument("An extension identifier must not be empty");
    }
    if (!settings.is_object()) {
        throw std::invalid_argument("Extension settings must be a JSON object");
    }
    mcp::declare_extension(impl_->capabilities, id, std::move(settings));
}

ServerCapabilities& Server::capabilities() { return impl_->capabilities; }

const ServerCapabilities& Server::capabilities() const { return impl_->capabilities; }

Implementation Server::named(std::string_view name, std::string_view version) {
    Implementation info;
    info.name = std::string(name);
    info.version = std::string(version);
    return info;
}

void Server::add_extension_method(const std::string& method, TypeErasedHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument("An extension method needs a name and a handler");
    }
    if (detail::is_modern_method(method) || impl_->extension_methods.contains(method)) {
        throw std::invalid_argument("Method is already served: " + method);
    }
    impl_->extension_methods.emplace(method, std::move(handler));
}

void Server::set_cache_hints(CacheTarget target, CacheHint hint) {
    if (hint.ttlMs < 0) {
        throw std::invalid_argument("Cache hint ttlMs must be >= 0");
    }
    impl_->cache_hints.at(static_cast<std::size_t>(target)) = hint;
}

void Server::set_report_server_info(bool report) { impl_->report_server_info = report; }

void Server::set_protocol_policy(ServerProtocolPolicy policy) {
    policy.validate();
    if (impl_->serving.load(std::memory_order_acquire)) {
        if (policy == impl_->protocol_policy) {
            return;
        }
        throw std::logic_error("The protocol policy cannot change once the server serves requests");
    }
    impl_->protocol_policy = std::move(policy);
}

ProtocolRejectionStats Server::protocol_rejection_stats() const {
    return {
        .invalid_metadata = impl_->invalid_metadata_rejections.load(std::memory_order_relaxed),
        .unsupported_version = impl_->unsupported_version_rejections.load(std::memory_order_relaxed),
        .legacy_shaped = impl_->legacy_shaped_rejections.load(std::memory_order_relaxed),
    };
}

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

// These three await rather than returning the task, and that is load-bearing. The argument is a
// temporary bound to a reference parameter of a lazy coroutine: under co_await it lives to the end
// of the full expression, which includes the await, but a plain `return` destroys it before the
// coroutine body ever runs and leaves the reference dangling. Both forms compile.
// Each goes to the sessions of every server sharing this one's registry, such as the legacy
// sessions run_http() serves.
Task<void> Server::notify_tools_list_changed() {
    for (const auto& member : impl_->registry_members()) {
        co_await member->notify_listeners(Impl::ListenTopic::eToolsListChanged, nullptr);
        if (member->legacy_broadcast_wanted()) {
            co_await member->send_broadcast(std::string(methods::g_NOTIFICATIONS_TOOLS_LIST_CHANGED));
        }
    }
}

Task<void> Server::notify_resources_list_changed() {
    for (const auto& member : impl_->registry_members()) {
        co_await member->notify_listeners(Impl::ListenTopic::eResourcesListChanged, nullptr);
        if (member->legacy_broadcast_wanted()) {
            co_await member->send_broadcast(
                std::string(methods::g_NOTIFICATIONS_RESOURCES_LIST_CHANGED));
        }
    }
}

Task<void> Server::notify_prompts_list_changed() {
    for (const auto& member : impl_->registry_members()) {
        co_await member->notify_listeners(Impl::ListenTopic::ePromptsListChanged, nullptr);
        if (member->legacy_broadcast_wanted()) {
            co_await member->send_broadcast(std::string(methods::g_NOTIFICATIONS_PROMPTS_LIST_CHANGED));
        }
    }
}

Task<void> Server::notify_resource_updated(const std::string& uri) {
    for (const auto& member : impl_->registry_members()) {
        co_await member->notify_listeners(Impl::ListenTopic::eResourceUpdated,
                                          std::make_shared<const std::string>(uri));
        co_await member->broadcast_resource_updated(uri);
    }
}

void Server::close_subscriptions() {
    for (const auto& member : impl_->registry_members()) {
        member->end_listeners(Impl::ListenEnd::eClosed);
    }
}

std::size_t Server::subscription_count() const {
    std::size_t count = 0;
    for (const auto& member : impl_->registry_members()) {
        std::scoped_lock lock(member->listeners_mutex);
        count += member->listeners.size();
    }
    return count;
}

bool Server::Impl::transport_started_new_session() const {
    auto current = session_snapshot();
    return current && current->http_transport != nullptr &&
           detail::terminated_session_count(*current->http_transport) !=
               terminated_sessions_at_initialize.load(std::memory_order_relaxed);
}

std::shared_ptr<Server::Session> Server::Impl::session_snapshot() const {
    std::scoped_lock lock(session_mutex);
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
    end_listeners(ListenEnd::eCancelled, session);
    abandon_pending_requests(*session);
}

void Server::Impl::abandon_pending_requests(Session& session) {
    for (auto& [id, pending] : session.pending_requests) {
        static_cast<void>(id);
        if (!pending.completed) {
            pending.error = Error{
                .code = mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                .message = "Server session closed",
                .data = std::nullopt,
            };
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
    // `stopping` is atomic, the session pointer is mutex-guarded, and ITransport::close() wakes a
    // blocked reader. The request maps belong to the session strand, so abandoning their entries is
    // posted there and never waited on: waiting would deadlock on the session strand itself, and
    // would never return when the io_context is stopped or was never run.
    session->stopping.store(true, std::memory_order_release);
    if (session->strand) {
        // Guarded for the same reason as the close below: queueing the hand-off allocates, and an
        // exception escaping ~Server would terminate the process.
        try {
            boost::asio::post(*session->strand, [impl = shared_from_this(), session] {
                impl->abandon_session_work(session);
            });
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
        std::scoped_lock lock(session_mutex);
        if (this->session == session) {
            this->session.reset();
        }
    }
}

}  // namespace mcp
