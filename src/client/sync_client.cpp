#include <mcp/client/sync_client.hpp>

#include "client_impl.hpp"

namespace mcp {

SyncSubscription::SyncSubscription(Subscription subscription)
    : subscription_(std::move(subscription)) {}

std::optional<SubscriptionEvent> SyncSubscription::next() {
    return detail::run_blocking<std::optional<SubscriptionEvent>>(
        subscription_.state_->blocking_loop, [this] { return subscription_.next(); });
}

void SyncSubscription::close() {
    detail::run_blocking<void>(subscription_.state_->blocking_loop,
                               [this] { return subscription_.close(); });
}

SyncTaskHandle::SyncTaskHandle(TaskHandle handle) : handle_(std::move(handle)) {}

DetailedTask SyncTaskHandle::get(const RequestOptions& request_options) const {
    return detail::run_blocking<DetailedTask>(handle_.blocking_loop(),
                                              [&] { return handle_.get(request_options); });
}

void SyncTaskHandle::update(const nlohmann::json& input_responses,
                            const RequestOptions& request_options) const {
    detail::run_blocking<void>(handle_.blocking_loop(),
                               [&] { return handle_.update(input_responses, request_options); });
}

void SyncTaskHandle::cancel(const RequestOptions& request_options) const {
    detail::run_blocking<void>(handle_.blocking_loop(),
                               [&] { return handle_.cancel(request_options); });
}

DetailedTask SyncTaskHandle::wait(const RequestOptions& request_options) const {
    return detail::run_blocking<DetailedTask>(handle_.blocking_loop(),
                                              [&] { return handle_.wait(request_options); });
}

CallToolResult SyncTaskHandle::result(const RequestOptions& request_options) const {
    return detail::run_blocking<CallToolResult>(handle_.blocking_loop(),
                                                [&] { return handle_.result(request_options); });
}

SyncClient::SyncClient(Client& client) : client_(&client) {}

ConnectResult SyncClient::connect(std::string_view name, std::string_view version) {
    return detail::run_blocking<ConnectResult>(client_->impl_->blocking_loop,
                                               [&] { return client_->connect(name, version); });
}

ConnectResult SyncClient::connect(const Implementation& client_info,
                                  const ClientCapabilities& capabilities) {
    return detail::run_blocking<ConnectResult>(
        client_->impl_->blocking_loop, [&] { return client_->connect(client_info, capabilities); });
}

nlohmann::json SyncClient::send_request(std::string_view method,
                                        const std::optional<nlohmann::json>& params,
                                        const RequestOptions& request_options) {
    return detail::run_blocking<nlohmann::json>(client_->impl_->blocking_loop, [&] {
        return client_->send_request(method, params, request_options);
    });
}

Subscription SyncClient::listen(const SubscriptionFilter& filter,
                                const RequestOptions& request_options) {
    return detail::run_blocking<Subscription>(client_->impl_->blocking_loop,
                                              [&] { return client_->listen(filter, request_options); });
}

void SyncClient::send_notification(std::string_view method,
                                   const std::optional<nlohmann::json>& params) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->send_notification(method, params); });
}

void SyncClient::close() { client_->close(); }

CallToolResult SyncClient::call_tool(const std::string& name, const nlohmann::json& arguments,
                                     const RequestOptions& request_options) {
    return detail::run_blocking<CallToolResult>(client_->impl_->blocking_loop, [&] {
        return client_->call_tool(name, arguments, request_options);
    });
}

std::variant<CallToolResult, TaskHandle> SyncClient::start_tool(const std::string& name,
                                                                const nlohmann::json& arguments,
                                                                const RequestOptions& request_options) {
    return detail::run_blocking<std::variant<CallToolResult, TaskHandle>>(
        client_->impl_->blocking_loop,
        [&] { return client_->start_tool(name, arguments, request_options); });
}

ListToolsResult SyncClient::list_tools(const std::optional<std::string>& cursor,
                                       const RequestOptions& request_options) {
    return detail::run_blocking<ListToolsResult>(
        client_->impl_->blocking_loop, [&] { return client_->list_tools(cursor, request_options); });
}

ListResourcesResult SyncClient::list_resources(const std::optional<std::string>& cursor,
                                               const RequestOptions& request_options) {
    return detail::run_blocking<ListResourcesResult>(client_->impl_->blocking_loop, [&] {
        return client_->list_resources(cursor, request_options);
    });
}

ReadResourceResult SyncClient::read_resource(const std::string& uri,
                                             const RequestOptions& request_options) {
    return detail::run_blocking<ReadResourceResult>(
        client_->impl_->blocking_loop, [&] { return client_->read_resource(uri, request_options); });
}

ListResourceTemplatesResult SyncClient::list_resource_templates(
    const std::optional<std::string>& cursor, const RequestOptions& request_options) {
    return detail::run_blocking<ListResourceTemplatesResult>(client_->impl_->blocking_loop, [&] {
        return client_->list_resource_templates(cursor, request_options);
    });
}

ListPromptsResult SyncClient::list_prompts(const std::optional<std::string>& cursor,
                                           const RequestOptions& request_options) {
    return detail::run_blocking<ListPromptsResult>(
        client_->impl_->blocking_loop, [&] { return client_->list_prompts(cursor, request_options); });
}

GetPromptResult SyncClient::get_prompt(const std::string& name,
                                       const std::map<std::string, std::string>& arguments,
                                       const RequestOptions& request_options) {
    return detail::run_blocking<GetPromptResult>(client_->impl_->blocking_loop, [&] {
        return client_->get_prompt(name, arguments, request_options);
    });
}

CompleteResult SyncClient::complete(const CompleteParams& params,
                                    const RequestOptions& request_options) {
    return detail::run_blocking<CompleteResult>(
        client_->impl_->blocking_loop, [&] { return client_->complete(params, request_options); });
}

void SyncClient::subscribe_resource(const std::string& uri, const RequestOptions& request_options) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->subscribe_resource(uri, request_options); });
}

void SyncClient::unsubscribe_resource(const std::string& uri, const RequestOptions& request_options) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->unsubscribe_resource(uri, request_options); });
}

void SyncClient::set_log_level(LoggingLevel level, const RequestOptions& request_options) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->set_log_level(level, request_options); });
}

void SyncClient::ping() {
    detail::run_blocking<void>(client_->impl_->blocking_loop, [&] { return client_->ping(); });
}

void SyncClient::cancel(const RequestId& request_id, const std::optional<std::string>& reason) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->cancel(request_id, reason); });
}

}  // namespace mcp
