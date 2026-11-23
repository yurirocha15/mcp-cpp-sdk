#include "server_impl.hpp"

namespace methods = mcp::constants::methods;

namespace mcp {

void Server::enable_tasks(TaskOptions options) {
    if (impl_->serving.load(std::memory_order_acquire)) {
        throw std::logic_error("Tasks cannot be enabled once the server serves requests");
    }
    if (options.poll_interval < std::chrono::milliseconds::zero() ||
        (options.ttl && *options.ttl <= std::chrono::milliseconds::zero())) {
        throw std::invalid_argument(
            "The task time to live must be longer than zero and the polling interval not negative");
    }
    if (!options.store) {
        InMemoryTaskStoreOptions store_options;
        store_options.max_tasks_per_owner = options.max_tasks_per_principal;
        store_options.max_tasks = options.max_tasks;
        options.store = std::make_shared<InMemoryTaskStore>(std::move(store_options));
    }
    impl_->task_store = options.store;
    impl_->task_options = std::move(options);
    impl_->tasks_enabled.store(true, std::memory_order_release);
}

void Server::set_tool_task_preparation(const std::string& name, ToolTaskPreparation preparation) {
    if (impl_->serving.load(std::memory_order_acquire)) {
        throw std::logic_error(
            "A tool's task preparation cannot change once the server serves requests");
    }
    const auto tool = impl_->tool_handlers.find(name);
    if (tool == impl_->tool_handlers.end()) {
        throw std::invalid_argument("Unknown tool: " + name);
    }
    tool->second.task_preparation = std::move(preparation);
}

void Server::set_tool_task_mode(const std::string& name, ToolTaskMode mode) {
    if (impl_->serving.load(std::memory_order_acquire)) {
        throw std::logic_error("A tool's task mode cannot change once the server serves requests");
    }
    const auto tool = impl_->tool_handlers.find(name);
    if (tool == impl_->tool_handlers.end()) {
        throw std::invalid_argument("Unknown tool: " + name);
    }
    tool->second.task_mode = mode;
    if (mode != ToolTaskMode::eNever && !impl_->handler_executor) {
        const std::string warning =
            "Tool \"" + name +
            "\" runs as a task without a handler executor: a handler that blocks stalls the "
            "executor the server runs on. See Server::set_handler_executor().";
        if (impl_->task_options.warning_handler) {
            impl_->task_options.warning_handler(warning);
        } else {
            std::cerr << warning << '\n';
        }
    }
}

std::string Server::Impl::missing_tasks_extension_wire(const RequestId& id) {
    nlohmann::json required = {
        {"extensions",
         {{std::string(mcp::constants::tasks::g_EXTENSION_ID), nlohmann::json::object()}}}};
    return make_error_wire(id, mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY,
                           "Missing required client capability",
                           {{"requiredCapabilities", std::move(required)}});
}

Task<std::string> Server::Impl::handle_task_wire(const nlohmann::json& json_msg,
                                                 std::string_view method, const RequestScope& scope) {
    std::shared_ptr<const TaskRecord> changed;
    // [gcc11-sso: string-in-frame] The answer is held as json until no suspension is left.
    nlohmann::json answer = serve_task_request(json_msg, method, scope, changed);
    if (changed) {
        auto self = shared_from_this();
        co_await notify_task(std::move(self), std::move(changed));
    }
    co_return std::move(answer.get_ref<nlohmann::json::string_t&>());
}

std::string Server::Impl::serve_task_request(const nlohmann::json& json_msg, std::string_view method,
                                             const RequestScope& scope,
                                             std::shared_ptr<const TaskRecord>& changed) {
    const auto& id = json_msg.at("id");
    if (!declares_extension(scope.meta.get(), mcp::constants::tasks::g_EXTENSION_ID)) {
        return missing_tasks_extension_wire(id.get<RequestId>());
    }
    const auto params_member = json_msg.find("params");
    const nlohmann::json* params =
        params_member != json_msg.end() && params_member->is_object() ? &*params_member : nullptr;
    const nlohmann::json* task_id = nullptr;
    if (params != nullptr) {
        const auto found = params->find("taskId");
        task_id = found != params->end() && found->is_string() ? &*found : nullptr;
    }
    if (task_id == nullptr) {
        throw InvalidParamsError("Invalid " + std::string(method) + " params: taskId must be a string");
    }
    const std::string_view id_text = task_id->get_ref<const std::string&>();
    const std::string_view owner =
        scope.principal ? std::string_view(*scope.principal) : std::string_view();
    const auto record = task_store->get(owner, id_text);
    if (!record) {
        return make_error_wire(id.get<RequestId>(), mcp::constants::jsonrpc::g_INVALID_PARAMS,
                               "Failed to retrieve task: Task not found");
    }
    if (method == methods::g_TASKS_GET) {
        return make_result_wire(id.get<RequestId>(), scope, nlohmann::json(record->to_detailed()));
    }

    std::shared_ptr<detail::TaskRun> run;
    {
        std::lock_guard lock(task_runs_mutex);
        const auto found = task_runs.find(record->id);
        if (found != task_runs.end()) {
            run = found->second;
        }
    }
    std::optional<TaskRecord> stored;
    if (method == methods::g_TASKS_UPDATE) {
        const auto responses = params->find("inputResponses");
        if (responses == params->end() || !responses->is_object()) {
            throw InvalidParamsError("Invalid tasks/update params: inputResponses must be an object");
        }
        stored = task_store->submit_input_responses(id_text, *responses);
        if (stored && run) {
            run->signal();
        }
    } else {
        // The task is cancelled before its handler is told to stop, so that a handler that
        // unwinds at once finds the task finished and records nothing over the cancellation.
        stored = task_store->cancel(id_text, "The client cancelled the task");
        if (run) {
            run->abandon();
        }
    }
    if (stored) {
        changed = std::make_shared<const TaskRecord>(std::move(*stored));
    }
    return make_result_wire(id.get<RequestId>(), scope, nlohmann::json::object());
}

bool Server::Impl::task_limit_reached_locked(const std::string& owner) const {
    const auto owned = static_cast<std::size_t>(
        std::count_if(task_runs.begin(), task_runs.end(),
                      [&owner](const auto& entry) { return entry.second->owner == owner; }));
    return owned >= task_options.max_tasks_per_principal || task_runs.size() >= task_options.max_tasks;
}

std::string Server::Impl::start_task(const nlohmann::json& json_msg, const RequestScope& scope,
                                     const CallToolView& call, const nlohmann::json& arguments,
                                     const boost::asio::any_io_executor& executor) {
    const std::string owner = scope.principal ? *scope.principal : std::string();
    // The limits count handlers that have not returned, not stored tasks: a cancelled or expired
    // task keeps its slot until its handler ends. The check, the creation and the registration
    // happen under one lock, so concurrent requests cannot pass the limit together.
    std::unique_lock runs_lock(task_runs_mutex);
    if (task_limit_reached_locked(owner)) {
        return make_error_wire(json_msg.at("id").get<RequestId>(),
                               mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                               "Too many tasks for this principal");
    }
    auto record = task_store->create(owner, task_options.ttl, task_options.poll_interval);
    if (!record) {
        return make_error_wire(json_msg.at("id").get<RequestId>(),
                               mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                               "Too many tasks for this principal");
    }

    nlohmann::json client_capabilities = nlohmann::json::object();
    {
        const auto& meta = *scope.meta->meta;
        const auto declared = meta.find(std::string(mcp::constants::meta::g_META_CLIENT_CAPABILITIES));
        if (declared != meta.end() && declared->is_object()) {
            client_capabilities = *declared;
        }
    }
    // The handler runs on the handler executor when one is set, and the task's waits on the same
    // strand.
    auto run = std::make_shared<detail::TaskRun>(
        boost::asio::make_strand(handler_executor ? *handler_executor : executor), task_store,
        record->id, owner, std::move(client_capabilities));
    // The task runs hold the server's implementation only while a handler needs it.
    run->publish = [weak = weak_from_this()](std::shared_ptr<const TaskRecord> changed) {
        return Impl::notify_task_if_alive(weak, std::move(changed));
    };
    task_runs.emplace(record->id, run);
    runs_lock.unlock();

    CallToolParams params{
        *call.name, arguments,
        call.meta != nullptr ? std::optional<nlohmann::json>(*call.meta) : std::nullopt};
    // The answer is complete before the handler can start, so it always describes a task that has
    // not run.
    auto answer = make_result_wire(json_msg.at("id").get<RequestId>(), scope,
                                   nlohmann::json(CreateTaskResult{record->to_detailed().task}));
    boost::asio::co_spawn(run->strand,
                          run_task_worker(shared_from_this(), run, std::move(params), scope.meta,
                                          scope.principal, record->ttl),
                          boost::asio::detached);
    return answer;
}

Task<void> Server::Impl::run_task_worker(std::shared_ptr<Impl> impl,
                                         std::shared_ptr<detail::TaskRun> run, CallToolParams params,
                                         std::shared_ptr<const RequestMetadata> request_meta,
                                         std::shared_ptr<const std::string> principal,
                                         std::optional<std::chrono::milliseconds> ttl) {
    // [gcc11-sso: string-in-frame] The outcome is held as json until no suspension is left.
    nlohmann::json result;
    nlohmann::json error;
    bool abandoned = false;
    const MessageSender no_notifications;
    if (ttl) {
        boost::asio::co_spawn(run->strand, detail::TaskRun::abandon_after(run, *ttl),
                              boost::asio::detached);
    }
    try {
        result = co_await impl->invoke_tool_impl(std::move(params), run->cancelled, std::nullopt,
                                                 std::move(request_meta), false, no_notifications,
                                                 std::move(principal), nullptr, nullptr, nullptr, run);
    } catch (const detail::TaskAbandoned&) {
        // The task was cancelled, expired or its server went away: the store holds its outcome.
        abandoned = true;
    } catch (const InvalidParamsError& failure) {
        error = {{"code", mcp::constants::jsonrpc::g_INVALID_PARAMS}, {"message", failure.what()}};
    } catch (const nlohmann::json::exception&) {
        error = {{"code", mcp::constants::jsonrpc::g_INTERNAL_ERROR},
                 {"message", "The result could not be encoded as JSON"}};
    } catch (const std::exception& failure) {
        error = {{"code", mcp::constants::jsonrpc::g_INTERNAL_ERROR},
                 {"message", detail::sanitize_for_diagnostics(failure.what())}};
    } catch (...) {
        error = {{"code", mcp::constants::jsonrpc::g_INTERNAL_ERROR}, {"message", "Internal error"}};
    }
    run->stop_expiry();
    {
        std::lock_guard lock(impl->task_runs_mutex);
        impl->task_runs.erase(run->id);
    }
    std::shared_ptr<const TaskRecord> finished;
    if (!abandoned) {
        std::optional<TaskRecord> stored;
        if (error.is_null()) {
            stored = run->store->complete(run->id, std::move(result));
        } else {
            std::string message = "The request failed: " + error.at("message").get<std::string>();
            stored = run->store->fail(run->id, std::move(error), std::move(message));
        }
        if (stored) {
            finished = std::make_shared<const TaskRecord>(std::move(*stored));
        }
    }
    if (finished) {
        co_await notify_task(std::move(impl), std::move(finished));
    }
}

Task<void> Server::Impl::notify_task_if_alive(std::weak_ptr<Impl> weak,
                                              std::shared_ptr<const TaskRecord> record) {
    auto impl = weak.lock();
    if (impl) {
        co_await notify_task(std::move(impl), std::move(record));
    }
}

Task<void> Server::Impl::notify_task(std::shared_ptr<Impl> impl,
                                     std::shared_ptr<const TaskRecord> record) {
    std::vector<std::shared_ptr<Listener>> targets;
    {
        std::lock_guard lock(impl->listeners_mutex);
        for (const auto& listener : impl->listeners) {
            if (listener->open() && listener->task_ids.contains(record->id)) {
                targets.push_back(listener);
            }
        }
    }
    for (const auto& listener : targets) {
        const auto wire = task_notification_wire(*record, *listener);
        if (!listener->deliver_or_defer(wire)) {
            continue;
        }
        try {
            co_await listener->sink(*wire);
        } catch (const std::exception&) {
            // A stream whose transport cannot take the message ends through that transport.
        }
    }
}

std::shared_ptr<const std::string> Server::Impl::task_notification_wire(const TaskRecord& record,
                                                                        const Listener& listener) {
    nlohmann::json params = record.to_detailed();
    params["_meta"] = subscription_meta(listener.id);
    nlohmann::json message = nlohmann::json::object();
    message["jsonrpc"] = "2.0";
    message["method"] = methods::g_NOTIFICATIONS_TASKS;
    message["params"] = std::move(params);
    return std::make_shared<const std::string>(mcp::detail::dump_json(message));
}

void Server::Impl::abandon_tasks() {
    std::map<std::string, std::shared_ptr<detail::TaskRun>> running;
    {
        std::lock_guard lock(task_runs_mutex);
        running.swap(task_runs);
    }
    for (const auto& [id, run] : running) {
        run->abandon();
    }
}

}  // namespace mcp
