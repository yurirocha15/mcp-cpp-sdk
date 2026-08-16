#include <mcp/client/client.hpp>
#include <mcp/detail/serialized_transport_writer.hpp>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

std::optional<nlohmann::json> make_paginated_params(const std::optional<std::string>& cursor) {
    if (!cursor) {
        return std::nullopt;
    }

    PaginatedRequestParams params;
    params.cursor = cursor;
    return nlohmann::json(std::move(params));
}

}  // namespace

McpError::McpError(Error error)
    : std::runtime_error("JSON-RPC error " + std::to_string(error.code) + ": " + error.message),
      error_(std::move(error)) {}

McpError::McpError(int code, std::string message, std::optional<nlohmann::json> data)
    : McpError(Error{code, std::move(message), std::move(data)}) {}

struct Client::Impl {
    struct PendingRequest {
        PendingRequest(const boost::asio::any_io_executor& executor, std::chrono::milliseconds timeout)
            : timer(executor), deadline(std::chrono::steady_clock::now() + timeout) {
            timer.expires_at(deadline);
        }

        boost::asio::steady_timer timer;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<std::atomic<bool>> cancel_write_before_start =
            std::make_shared<std::atomic<bool>>(false);
        nlohmann::json result;
        std::optional<Error> error;
        std::exception_ptr write_error;
        bool completed{false};
    };

    Impl(std::shared_ptr<ITransport> client_transport, const boost::asio::any_io_executor& executor,
         ClientOptions client_options)
        : transport(std::move(client_transport)),
          strand(boost::asio::make_strand(executor)),
          options(std::move(client_options)),
          writer(transport, strand) {}

    static Task<nlohmann::json> request(std::shared_ptr<Impl> state, std::string_view method,
                                        const std::optional<nlohmann::json>& params,
                                        const RequestOptions& request_options) {
        const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
        if (timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("Request timeout must be positive");
        }

        const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);
        JSONRPCRequest request;
        request.id = RequestId{std::to_string(id)};
        request.method = std::string(method);
        request.params = params;
        auto wire = std::make_shared<const std::string>(nlohmann::json(std::move(request)).dump());
        auto strand = state->strand;
        // Awaiting post(strand) would not rebind a caller-owned coroutine's continuation.
        // Spawn the complete pending-request lifecycle on the client strand instead.
        return boost::asio::co_spawn(strand,
                                     send_request_wire(std::move(state), std::move(wire), id, timeout),
                                     boost::asio::use_awaitable);
    }

    static Task<void> notification(std::shared_ptr<Impl> state, std::string_view method,
                                   const std::optional<nlohmann::json>& params) {
        JSONRPCNotification notification;
        notification.method = std::string(method);
        notification.params = params;
        auto wire = std::make_shared<const std::string>(nlohmann::json(std::move(notification)).dump());
        auto strand = state->strand;
        // Keep the closed-state check and write initiation on the client strand.
        return boost::asio::co_spawn(strand, send_notification_wire(std::move(state), std::move(wire)),
                                     boost::asio::use_awaitable);
    }

    static Task<InitializeResult> finish_connect(std::shared_ptr<Impl> state,
                                                 Task<nlohmann::json> initialize_request) {
        try {
            auto result_json = co_await std::move(initialize_request);
            auto initialize_result =
                std::make_shared<InitializeResult>(result_json.get<InitializeResult>());
            if (state->options.strict_protocol_validation &&
                !is_supported_protocol_version(initialize_result->protocolVersion)) {
                request_close(state, "Server selected an unsupported protocol version");
                throw McpError(g_INVALID_REQUEST, "Server selected unsupported protocol version: " +
                                                      initialize_result->protocolVersion);
            }

            co_await notification(state, "notifications/initialized", std::nullopt);
            co_return std::move(*initialize_result);
        } catch (...) {
            request_close(state, "Client initialization failed");
            throw;
        }
    }

    static Task<void> discard_result(Task<nlohmann::json> request_task) {
        static_cast<void>(co_await std::move(request_task));
    }

    static void start_read_loop(const std::shared_ptr<Impl>& state) {
        boost::asio::co_spawn(state->strand, read_loop(state), boost::asio::detached);
    }

    static void request_close(const std::shared_ptr<Impl>& state, std::string message) {
        if (state->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        boost::asio::post(state->strand, [state, message = std::move(message)]() mutable {
            fail_pending_requests(state, Error{g_CONNECTION_CLOSED, std::move(message), std::nullopt});
            state->transport->close();
        });
    }

    static Task<nlohmann::json> send_request_wire(std::shared_ptr<Impl> state,
                                                  std::shared_ptr<const std::string> wire, int64_t id,
                                                  std::chrono::milliseconds timeout) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
        }

        const auto id_key = RequestId{std::to_string(id)}.correlation_key();
        PendingRequest* pending_request = nullptr;
        {
            std::lock_guard lock(state->pending_requests_mutex);
            auto [iter, inserted] = state->pending_requests.try_emplace(
                id_key, std::make_unique<PendingRequest>(state->strand, timeout));
            if (!inserted) {
                throw McpError(g_INVALID_REQUEST, "Duplicate pending request id: " + id_key);
            }
            pending_request = iter->second.get();
        }

        boost::asio::co_spawn(
            state->strand,
            state->writer.write_message(std::move(wire), pending_request->cancel_write_before_start),
            [state, id_key](std::exception_ptr write_error) {
                complete_request_write(state, id_key, std::move(write_error));
            });

        try {
            co_await pending_request->timer.async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            if (error.code() != boost::asio::error::operation_aborted) {
                erase_pending_request(state, id_key);
                throw;
            }
        }

        nlohmann::json result;
        std::optional<Error> error;
        std::exception_ptr write_error;
        {
            std::lock_guard lock(state->pending_requests_mutex);
            auto iter = state->pending_requests.find(id_key);
            if (iter == state->pending_requests.end()) {
                throw McpError(g_CONNECTION_CLOSED, "Pending request was removed: " + id_key);
            }
            if (!iter->second->completed) {
                iter->second->cancel_write_before_start->store(true, std::memory_order_release);
                iter->second->error = Error{g_REQUEST_TIMEOUT, "Request timed out",
                                            nlohmann::json{{"id", std::to_string(id)}}};
                iter->second->completed = true;
            }
            result = std::move(iter->second->result);
            error = std::move(iter->second->error);
            write_error = std::move(iter->second->write_error);
            state->pending_requests.erase(iter);
        }

        if (error) {
            throw McpError(std::move(*error));
        }
        if (write_error) {
            std::rethrow_exception(write_error);
        }
        co_return result;
    }

    static Task<void> send_notification_wire(std::shared_ptr<Impl> state,
                                             std::shared_ptr<const std::string> wire) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
        }

        try {
            co_await state->writer.write_message(wire);
        } catch (...) {
            if (state->closed.load(std::memory_order_acquire)) {
                throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
            }
            throw;
        }
    }

    static Task<void> read_loop(std::shared_ptr<Impl> state) {
        try {
            for (;;) {
                auto raw = co_await state->transport->read_message();
                auto json_message = nlohmann::json::parse(raw);

                if (!json_message.is_object()) {
                    throw McpError(g_INVALID_REQUEST, "JSON-RPC message must be an object");
                }
                if (state->options.strict_protocol_validation) {
                    if (!json_message.contains("jsonrpc") || !json_message.at("jsonrpc").is_string()) {
                        throw McpError(g_INVALID_REQUEST, "JSON-RPC message is missing jsonrpc");
                    }
                    detail::validate_jsonrpc_version(json_message.at("jsonrpc").get<std::string>());
                }

                const bool has_id = json_message.contains("id");
                const bool has_method = json_message.contains("method");
                if (has_id && !has_method) {
                    dispatch_response(state, json_message);
                } else if (has_id && has_method) {
                    boost::asio::co_spawn(state->strand,
                                          dispatch_incoming_request(state, std::move(json_message)),
                                          boost::asio::detached);
                } else if (!has_id && has_method) {
                    dispatch_notification(state, json_message);
                }
            }
        } catch (const std::exception& error) {
            state->closed.store(true, std::memory_order_release);
            fail_pending_requests(
                state, Error{g_CONNECTION_CLOSED,
                             "Client read loop stopped: " + std::string(error.what()), std::nullopt});
            state->transport->close();
        } catch (...) {
            state->closed.store(true, std::memory_order_release);
            fail_pending_requests(state,
                                  Error{g_CONNECTION_CLOSED, "Client read loop stopped", std::nullopt});
            state->transport->close();
        }
    }

    static void dispatch_response(const std::shared_ptr<Impl>& state,
                                  const nlohmann::json& json_message) {
        const auto& id = json_message.at("id");
        if (!id.is_string() && !id.is_number_integer()) {
            return;
        }
        const auto id_key = id.get<RequestId>().correlation_key();

        std::lock_guard lock(state->pending_requests_mutex);
        auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end()) {
            return;
        }
        if (std::chrono::steady_clock::now() >= iter->second->deadline) {
            return;
        }

        const bool has_error = json_message.contains("error");
        const bool has_result = json_message.contains("result");
        if (has_error == has_result) {
            iter->second->error =
                Error{g_INVALID_REQUEST,
                      "JSON-RPC response must contain exactly one of result or error", std::nullopt};
        } else if (has_error) {
            iter->second->error = json_message.at("error").get<Error>();
        } else {
            iter->second->result = json_message.at("result");
        }

        iter->second->completed = true;
        signal_pending_request(*iter->second);
    }

    static void dispatch_notification(const std::shared_ptr<Impl>& state,
                                      const nlohmann::json& json_message) {
        const auto method = json_message.at("method").get<std::string>();
        if (method == "notifications/cancelled") {
            if (json_message.contains("params")) {
                auto params = json_message.at("params").get<CancelledNotificationParams>();
                const auto id_key = params.requestId.correlation_key();

                std::lock_guard lock(state->pending_requests_mutex);
                auto iter = state->pending_requests.find(id_key);
                if (iter != state->pending_requests.end() &&
                    std::chrono::steady_clock::now() < iter->second->deadline) {
                    iter->second->error = Error{g_REQUEST_CANCELLED, "Request cancelled by server"};
                    iter->second->completed = true;
                    signal_pending_request(*iter->second);
                }
            }
            return;
        }

        NotificationCallback callback;
        {
            std::lock_guard lock(state->handlers_mutex);
            auto iter = state->notification_handlers.find(method);
            if (iter != state->notification_handlers.end()) {
                callback = iter->second;
            }
        }
        if (callback) {
            auto params =
                json_message.contains("params") ? json_message.at("params") : nlohmann::json{};
            callback(params);
        }
    }

    static Task<void> dispatch_incoming_request(std::shared_ptr<Impl> state,
                                                nlohmann::json json_message) {
        std::string_view method = json_message.at("method").get_ref<const nlohmann::json::string_t&>();

        RequestHandler request_handler;
        {
            std::lock_guard lock(state->handlers_mutex);
            auto iter = state->request_handlers.find(method);
            if (iter != state->request_handlers.end()) {
                request_handler = iter->second;
            }
        }

        if (request_handler) {
            auto params =
                json_message.contains("params") ? json_message.at("params") : nlohmann::json{};
            nlohmann::json error_payload;
            nlohmann::json result;
            try {
                result = co_await request_handler(params);
            } catch (const std::exception& error) {
                error_payload = error.what();
            }

            if (!error_payload.is_null()) {
                co_await state->writer.write_message(
                    make_error_wire(json_message.at("id").get<RequestId>(), g_INTERNAL_ERROR,
                                    error_payload.get<std::string>()));
            } else {
                co_await state->writer.write_message(
                    make_result_wire(json_message.at("id").get<RequestId>(), std::move(result)));
            }
        } else if (method == "ping") {
            co_await state->writer.write_message(
                make_result_wire(json_message.at("id").get<RequestId>(), nlohmann::json::object()));
        } else {
            co_await state->writer.write_message(
                make_error_wire(json_message.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                "Method not found: " + std::string(method)));
        }
    }

    static std::string make_result_wire(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);
        return nlohmann::json(std::move(response)).dump();
    }

    static std::string make_error_wire(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);
        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);
        return nlohmann::json(std::move(response)).dump();
    }

    static void signal_pending_request(PendingRequest& pending_request) {
        pending_request.timer.expires_at(std::chrono::steady_clock::now());
    }

    static void complete_request_write(const std::shared_ptr<Impl>& state, const std::string& id_key,
                                       std::exception_ptr write_error) {
        if (!write_error) {
            return;
        }

        std::lock_guard lock(state->pending_requests_mutex);
        auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end() || iter->second->completed ||
            std::chrono::steady_clock::now() >= iter->second->deadline) {
            return;
        }

        if (state->closed.load(std::memory_order_acquire)) {
            iter->second->error = Error{g_CONNECTION_CLOSED, "Client transport is closed"};
        } else {
            iter->second->write_error = std::move(write_error);
        }
        iter->second->completed = true;
        signal_pending_request(*iter->second);
    }

    static void erase_pending_request(const std::shared_ptr<Impl>& state, const std::string& id_key) {
        std::lock_guard lock(state->pending_requests_mutex);
        state->pending_requests.erase(id_key);
    }

    static void fail_pending_requests(const std::shared_ptr<Impl>& state, const Error& error) {
        std::lock_guard lock(state->pending_requests_mutex);
        for (auto& [id, pending_request] : state->pending_requests) {
            static_cast<void>(id);
            if (!pending_request->completed) {
                pending_request->error = error;
                pending_request->completed = true;
            }
            signal_pending_request(*pending_request);
        }
    }

    std::shared_ptr<ITransport> transport;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    ClientOptions options;
    detail::SerializedTransportWriter writer;

    mutable std::mutex pending_requests_mutex;
    std::map<std::string, std::unique_ptr<PendingRequest>, std::less<>> pending_requests;
    std::atomic<int64_t> next_request_id{1};
    std::atomic<bool> read_loop_started{false};
    std::atomic<bool> closed{false};

    mutable std::mutex handlers_mutex;
    std::map<std::string, NotificationCallback, std::less<>> notification_handlers;
    std::map<std::string, RequestHandler, std::less<>> request_handlers;
    std::vector<Root> roots;
    ClientCapabilities client_capabilities;
};

Client::Client(std::shared_ptr<ITransport> transport, const boost::asio::any_io_executor& executor,
               ClientOptions options) {
    if (!transport) {
        throw std::invalid_argument("Client transport must not be null");
    }
    if (options.request_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client request timeout must be positive");
    }
    impl_ = std::make_shared<Impl>(std::move(transport), executor, std::move(options));
}

Client::~Client() { close(); }

Task<InitializeResult> Client::connect(std::string_view name, std::string_view version) {
    Implementation info;
    info.name = std::string(name);
    info.version = std::string(version);
    return connect(info, {});
}

Task<InitializeResult> Client::connect(const Implementation& client_info,
                                       const ClientCapabilities& capabilities) {
    auto state = impl_;
    if (state->closed.load(std::memory_order_acquire)) {
        throw McpError(g_CONNECTION_CLOSED, "Client is closed");
    }
    if (state->read_loop_started.exchange(true, std::memory_order_acq_rel)) {
        throw McpError(g_INVALID_REQUEST, "Client has already been initialized");
    }

    Impl::start_read_loop(state);
    InitializeRequest initialize_request;
    initialize_request.protocolVersion = std::string(g_LATEST_PROTOCOL_VERSION);
    initialize_request.clientInfo = client_info;
    {
        std::lock_guard lock(state->handlers_mutex);
        state->client_capabilities = capabilities;
        if (state->request_handlers.contains("roots/list") &&
            !state->client_capabilities.roots.has_value()) {
            state->client_capabilities.roots = ClientCapabilities::RootsCapability{};
        }
        initialize_request.capabilities = state->client_capabilities;
    }
    auto request_task =
        Impl::request(state, "initialize", nlohmann::json(std::move(initialize_request)), {});
    return Impl::finish_connect(std::move(state), std::move(request_task));
}

Task<nlohmann::json> Client::send_request(std::string_view method,
                                          const std::optional<nlohmann::json>& params) {
    return send_request(method, params, {});
}

Task<nlohmann::json> Client::send_request(std::string_view method,
                                          const std::optional<nlohmann::json>& params,
                                          const RequestOptions& request_options) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(g_INVALID_REQUEST, "Client must connect before sending requests");
    }
    return Impl::request(impl_, method, params, request_options);
}

Task<void> Client::send_notification(std::string_view method,
                                     const std::optional<nlohmann::json>& params) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(g_INVALID_REQUEST, "Client must connect before sending notifications");
    }
    return Impl::notification(impl_, method, params);
}

std::size_t Client::pending_request_count() const {
    std::lock_guard lock(impl_->pending_requests_mutex);
    return impl_->pending_requests.size();
}

void Client::close() {
    if (impl_) {
        Impl::request_close(impl_, "Client transport closed");
    }
}

Task<ListToolsResult> Client::list_tools(const std::optional<std::string>& cursor) {
    return call_and_parse<ListToolsResult>("tools/list", make_paginated_params(cursor));
}

Task<ListResourcesResult> Client::list_resources(const std::optional<std::string>& cursor) {
    return call_and_parse<ListResourcesResult>("resources/list", make_paginated_params(cursor));
}

Task<ReadResourceResult> Client::read_resource(const std::string& uri) {
    ReadResourceRequestParams params;
    params.uri = uri;
    return call_and_parse<ReadResourceResult>("resources/read", nlohmann::json(std::move(params)));
}

Task<ListResourceTemplatesResult> Client::list_resource_templates(
    const std::optional<std::string>& cursor) {
    return call_and_parse<ListResourceTemplatesResult>("resources/templates/list",
                                                       make_paginated_params(cursor));
}

Task<ListPromptsResult> Client::list_prompts(const std::optional<std::string>& cursor) {
    return call_and_parse<ListPromptsResult>("prompts/list", make_paginated_params(cursor));
}

Task<GetPromptResult> Client::get_prompt(
    const std::string& name, const std::optional<std::map<std::string, std::string>>& arguments) {
    GetPromptRequestParams params;
    params.name = name;
    params.arguments = arguments;
    return call_and_parse<GetPromptResult>("prompts/get", nlohmann::json(std::move(params)));
}

Task<CompleteResult> Client::complete(const CompleteParams& params) {
    return call_and_parse<CompleteResult>("completion/complete", nlohmann::json(params));
}

Task<void> Client::ping() {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(g_INVALID_REQUEST, "Client must connect before sending requests");
    }
    return Impl::discard_result(Impl::request(impl_, "ping", std::nullopt, {}));
}

Task<void> Client::cancel(const RequestId& request_id, const std::optional<std::string>& reason) {
    CancelledNotificationParams params;
    params.requestId = request_id;
    params.reason = reason;
    return send_notification("notifications/cancelled", nlohmann::json(std::move(params)));
}

void Client::on_notification(const std::string& method, NotificationCallback callback) {
    std::lock_guard lock(impl_->handlers_mutex);
    impl_->notification_handlers[method] = std::move(callback);
}

void Client::on_progress(ProgressCallback callback) {
    on_notification("notifications/progress",
                    [callback = std::move(callback)](const nlohmann::json& params) {
                        callback(params.get<ProgressNotificationParams>());
                    });
}

void Client::on_request(const std::string& method, RequestHandler handler) {
    std::lock_guard lock(impl_->handlers_mutex);
    impl_->request_handlers[method] = std::move(handler);
}

void Client::on_elicitation(std::function<Task<ElicitResult>(const ElicitRequestParams&)> handler) {
    on_request("elicitation/create",
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params.get<ElicitRequestParams>());
                   co_return nlohmann::json(std::move(result));
               });
}

void Client::set_roots(const std::vector<Root>& roots, bool notify) {
    auto state = impl_;
    bool should_notify = false;
    {
        std::lock_guard lock(state->handlers_mutex);
        state->roots = roots;
        if (!state->request_handlers.contains("roots/list")) {
            std::weak_ptr<Impl> weak_state = state;
            state->request_handlers.emplace(
                "roots/list", [weak_state](const nlohmann::json&) -> Task<nlohmann::json> {
                    auto active_state = weak_state.lock();
                    if (!active_state) {
                        throw McpError(g_CONNECTION_CLOSED, "Client is closed");
                    }

                    ListRootsResult result;
                    {
                        std::lock_guard roots_lock(active_state->handlers_mutex);
                        result.roots = active_state->roots;
                    }
                    co_return nlohmann::json(std::move(result));
                });
        }

        should_notify = notify && state->client_capabilities.roots.has_value() &&
                        state->client_capabilities.roots->listChanged.value_or(false);
    }

    if (should_notify && !state->closed.load(std::memory_order_acquire)) {
        boost::asio::co_spawn(
            state->strand, Impl::notification(state, "notifications/roots/list_changed", std::nullopt),
            boost::asio::detached);
    }
}

void Client::on_roots_list(std::function<Task<ListRootsResult>(const nlohmann::json&)> handler) {
    on_request("roots/list",
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params);
                   co_return nlohmann::json(std::move(result));
               });
}

}  // namespace mcp
