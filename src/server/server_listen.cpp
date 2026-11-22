#include "server_impl.hpp"

namespace methods = mcp::constants::methods;

namespace mcp {

// Reached only as a 2026-07-28 request whose `_meta` dispatch_modern_wire has already checked. The
// request stays open: its notifications go to the sink of the transport that carried it, and it is
// answered only when the server closes the stream. A client ends it by cancelling the request.
Task<std::string> Server::Impl::handle_listen_wire(const nlohmann::json& json_msg,
                                                   const RequestScope& scope) {
    auto session = session_snapshot();
    MessageSender sink;
    std::shared_ptr<Session> owner;
    if (scope.session_request && session) {
        if (session->http_transport != nullptr) {
            // A request an HttpServerTransport queued before the server was bound has no stream to
            // keep open: the session's own stream is the legacy session's.
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                                      "Method not found: subscriptions/listen");
        }
        owner = session;
        sink = [session](std::string_view message) { return session->writer->write_message(message); };
    } else if (scope.notifications) {
        sink = scope.notifications;
    } else {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_REQUEST,
                                  "subscriptions/listen needs a transport that streams notifications");
    }

    const auto notifications = json_msg.at("params").find("notifications");
    if (notifications == json_msg.at("params").end() || !notifications->is_object()) {
        throw InvalidParamsError(
            "Invalid subscriptions/listen params: notifications must be an object");
    }
    const auto requested_task_ids = notifications->find("taskIds");
    const std::string_view task_owner =
        scope.principal ? std::string_view(*scope.principal) : std::string_view();
    if (requested_task_ids != notifications->end()) {
        constexpr std::size_t g_max_task_ids = 1024;
        if (!requested_task_ids->is_array() || requested_task_ids->size() > g_max_task_ids ||
            !std::all_of(requested_task_ids->begin(), requested_task_ids->end(),
                         [](const nlohmann::json& id) { return id.is_string(); })) {
            throw InvalidParamsError(
                "Invalid subscriptions/listen params: notifications.taskIds must be an array of at "
                "most 1024 strings");
        }
        if (tasks_enabled.load(std::memory_order_acquire) && !requested_task_ids->empty() &&
            !declares_extension(scope.meta.get(), mcp::constants::tasks::g_EXTENSION_ID)) {
            co_return missing_tasks_extension_wire(json_msg.at("id").get<RequestId>());
        }
    }
    const auto flag = [&notifications](const char* name) {
        const auto it = notifications->find(name);
        if (it == notifications->end()) {
            return false;
        }
        if (!it->is_boolean()) {
            throw InvalidParamsError("Invalid subscriptions/listen params: notifications." +
                                     std::string(name) + " must be a boolean");
        }
        return it->get<bool>();
    };

    auto executor = co_await boost::asio::this_coro::executor;
    auto listener = std::make_shared<Listener>(boost::asio::make_strand(executor));
    listener->id = json_msg.at("id");
    listener->key = json_msg.at("id").get<RequestId>().correlation_key();
    listener->session = owner;
    listener->sink = std::move(sink);

    // Only the types this server advertises it emits are honored; the acknowledgment reports them.
    nlohmann::json honored = nlohmann::json::object();
    const bool tools_requested = flag("toolsListChanged");
    const bool prompts_requested = flag("promptsListChanged");
    const bool resources_requested = flag("resourcesListChanged");
    listener->tools_list_changed =
        tools_requested && capabilities.tools && capabilities.tools->listChanged.value_or(false);
    listener->prompts_list_changed =
        prompts_requested && capabilities.prompts && capabilities.prompts->listChanged.value_or(false);
    listener->resources_list_changed = resources_requested && capabilities.resources &&
                                       capabilities.resources->listChanged.value_or(false);
    if (listener->tools_list_changed) {
        honored["toolsListChanged"] = true;
    }
    if (listener->prompts_list_changed) {
        honored["promptsListChanged"] = true;
    }
    if (listener->resources_list_changed) {
        honored["resourcesListChanged"] = true;
    }
    if (const auto uris = notifications->find("resourceSubscriptions"); uris != notifications->end()) {
        if (!uris->is_array() ||
            !std::all_of(uris->begin(), uris->end(),
                         [](const nlohmann::json& uri) { return uri.is_string(); })) {
            throw InvalidParamsError(
                "Invalid subscriptions/listen params: notifications.resourceSubscriptions must be "
                "an array of strings");
        }
        if (!uris->empty() && capabilities.resources &&
            capabilities.resources->subscribe.value_or(false)) {
            honored["resourceSubscriptions"] = *uris;
            for (const auto& uri : *uris) {
                listener->resource_uris.insert(uri.get<std::string>());
            }
        }
    }

    if (requested_task_ids != notifications->end() && tasks_enabled.load(std::memory_order_acquire)) {
        // Only the tasks the caller created are honored, as only they may be fetched.
        nlohmann::json honored_ids = nlohmann::json::array();
        for (const auto& id : *requested_task_ids) {
            if (task_store->get(task_owner, id.get_ref<const std::string&>())) {
                honored_ids.push_back(id);
                listener->task_ids.insert(id.get<std::string>());
            }
        }
        if (!honored_ids.empty()) {
            honored["taskIds"] = std::move(honored_ids);
        }
    }

    // [gcc11-sso: scope-before-await] The strings that outlive a suspension are owned by shared state.
    std::shared_ptr<const std::string> acknowledgment;
    {
        nlohmann::json params = nlohmann::json::object();
        params["_meta"] = subscription_meta(listener->id);
        params["notifications"] = std::move(honored);
        nlohmann::json message = nlohmann::json::object();
        message["jsonrpc"] = "2.0";
        message["method"] = methods::g_NOTIFICATIONS_SUBSCRIPTIONS_ACKNOWLEDGED;
        message["params"] = std::move(params);
        acknowledgment = std::make_shared<const std::string>(mcp::detail::dump_json(message));
    }

    {
        std::lock_guard lock(listeners_mutex);
        listeners.push_back(listener);
    }
    // The state of each listed task as it is now follows the acknowledgment, so that a task that
    // finished before the stream was registered is not missed; what changes later is sent as it
    // happens. The stream holds these messages back until the acknowledgment is written.
    for (const auto& task_id : listener->task_ids) {
        if (const auto current = task_store->get(task_owner, task_id)) {
            listener->deliver_or_defer(task_notification_wire(*current, *listener));
        }
    }
    if (owner) {
        owner->modern_listeners.fetch_add(1, std::memory_order_relaxed);
    }
    struct Removal {
        std::shared_ptr<Impl> impl;
        std::shared_ptr<Listener> listener;
        ~Removal() { impl->remove_listener(listener); }
    };
    const Removal removal{shared_from_this(), listener};

    // A transport that sets the cancellation flag wakes the wait through this; the flag is read
    // after registering, so a cancellation that came first is not missed.
    if (scope.watch_cancel) {
        scope.watch_cancel([listener] { listener->end(ListenEnd::eCancelled); });
    }
    if (scope.cancelled && scope.cancelled->load(std::memory_order_relaxed)) {
        listener->end(ListenEnd::eCancelled);
    }

    co_await listener->sink(*acknowledgment);
    // What changed while the acknowledgment was being written follows it, in order; the stream
    // takes messages directly only once nothing is left waiting.
    for (bool drained = false; !drained;) {
        std::vector<std::shared_ptr<const std::string>> batch;
        {
            std::lock_guard lock(listener->gate);
            batch.swap(listener->pending);
            drained = batch.empty();
            listener->acknowledged = drained;
        }
        for (const auto& message : batch) {
            co_await listener->sink(*message);
        }
    }

    co_await boost::asio::co_spawn(listener->strand, wait_for_listener_end(listener),
                                   boost::asio::use_awaitable);

    // The stream is closed, or the client has gone and nobody reads the answer.
    nlohmann::json result = nlohmann::json::object();
    result["_meta"] = subscription_meta(listener->id);
    auto closing = std::make_shared<const std::string>(
        make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(result), std::nullopt));
    if (!owner || listener->state.load(std::memory_order_acquire) != ListenEnd::eClosed) {
        co_return *closing;
    }
    // The server ended the stream on a transport it shares with the rest of the session, where
    // there is no stream to close: after the closing result it sends notifications/cancelled for
    // the listen request, the one cancellation a server sends. Both go out here, in that order,
    // so the session writes nothing more for the request.
    std::shared_ptr<const std::string> cancellation;
    {
        nlohmann::json params = nlohmann::json::object();
        params["requestId"] = listener->id;
        params["reason"] = "The server closed the subscription";
        params["_meta"] = subscription_meta(listener->id);
        nlohmann::json message = nlohmann::json::object();
        message["jsonrpc"] = "2.0";
        message["method"] = methods::g_NOTIFICATIONS_CANCELLED;
        message["params"] = std::move(params);
        cancellation = std::make_shared<const std::string>(mcp::detail::dump_json(message));
    }
    co_await listener->sink(*closing);
    co_await listener->sink(*cancellation);
    co_return std::string{};
}

Task<void> Server::Impl::wait_for_listener_end(std::shared_ptr<Listener> listener) {
    // Runs on the listener's strand, like the posted cancel that wakes it: ending the stream sets
    // the state first, so a wake is either seen here or cancels the wait that starts below.
    while (listener->open()) {
        listener->timer.expires_at(std::chrono::steady_clock::time_point::max());
        try {
            co_await listener->timer.async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            if (error.code() != boost::asio::error::operation_aborted) {
                throw;
            }
        }
    }
}

void Server::Impl::remove_listener(const std::shared_ptr<Listener>& listener) {
    {
        std::lock_guard lock(listeners_mutex);
        std::erase(listeners, listener);
    }
    if (listener->session) {
        listener->session->modern_listeners.fetch_sub(1, std::memory_order_relaxed);
    }
}

void Server::Impl::end_listeners(ListenEnd reason, const std::shared_ptr<Session>& session,
                                 const std::string* key) {
    std::vector<std::shared_ptr<Listener>> ending;
    {
        std::lock_guard lock(listeners_mutex);
        for (const auto& listener : listeners) {
            if ((session == nullptr || listener->session == session) &&
                (key == nullptr || listener->key == *key)) {
                ending.push_back(listener);
            }
        }
    }
    for (const auto& listener : ending) {
        listener->end(reason);
    }
}

bool Server::Impl::legacy_broadcast_wanted() const {
    auto current = session_snapshot();
    return !current || current->modern_listeners.load(std::memory_order_relaxed) == 0 ||
           lifecycle.load(std::memory_order_relaxed) != LifecycleState::eUninitialized;
}

Task<void> Server::Impl::notify_listeners(ListenTopic topic, std::shared_ptr<const std::string> uri) {
    std::vector<std::shared_ptr<Listener>> targets;
    {
        std::lock_guard lock(listeners_mutex);
        for (const auto& listener : listeners) {
            bool wanted = false;
            switch (topic) {
                case ListenTopic::eToolsListChanged:
                    wanted = listener->tools_list_changed;
                    break;
                case ListenTopic::ePromptsListChanged:
                    wanted = listener->prompts_list_changed;
                    break;
                case ListenTopic::eResourcesListChanged:
                    wanted = listener->resources_list_changed;
                    break;
                case ListenTopic::eResourceUpdated:
                    wanted = listener->resource_uris.contains(*uri);
                    break;
            }
            if (wanted && listener->open()) {
                targets.push_back(listener);
            }
        }
    }

    std::string_view method;
    switch (topic) {
        case ListenTopic::eToolsListChanged:
            method = methods::g_NOTIFICATIONS_TOOLS_LIST_CHANGED;
            break;
        case ListenTopic::ePromptsListChanged:
            method = methods::g_NOTIFICATIONS_PROMPTS_LIST_CHANGED;
            break;
        case ListenTopic::eResourcesListChanged:
            method = methods::g_NOTIFICATIONS_RESOURCES_LIST_CHANGED;
            break;
        case ListenTopic::eResourceUpdated:
            method = methods::g_NOTIFICATIONS_RESOURCES_UPDATED;
            break;
    }

    for (const auto& listener : targets) {
        // [gcc11-sso: scope-before-await]
        std::shared_ptr<const std::string> wire;
        {
            nlohmann::json params = nlohmann::json::object();
            if (uri && topic == ListenTopic::eResourceUpdated) {
                params["uri"] = *uri;
            }
            params["_meta"] = subscription_meta(listener->id);
            nlohmann::json message = nlohmann::json::object();
            message["jsonrpc"] = "2.0";
            message["method"] = std::string(method);
            message["params"] = std::move(params);
            wire = std::make_shared<const std::string>(mcp::detail::dump_json(message));
        }
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

}  // namespace mcp
