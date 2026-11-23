#include <mcp/client/task_handle.hpp>

#include "client_impl.hpp"

namespace mcp {

TaskHandle::TaskHandle(std::shared_ptr<void> client, TaskData created, std::optional<std::string> tool)
    : client_(std::move(client)), created_(std::move(created)), tool_(std::move(tool)) {}

namespace {

std::shared_ptr<void> attached_client(const std::shared_ptr<void>& client) {
    if (!client) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "The task handle is not attached to a task");
    }
    return client;
}

Task<DetailedTask> detailed_task(Task<nlohmann::json> finished) {
    auto reply = co_await std::move(finished);
    co_return decode_task_state(reply);
}

Task<CallToolResult> task_result(Task<nlohmann::json> finished, OutputCheck check) {
    auto reply = co_await std::move(finished);
    co_return finish_call_result(task_outcome(reply), check);
}

}  // namespace

Task<DetailedTask> TaskHandle::get(const RequestOptions& request_options) const {
    // [gcc11-sso: not-a-coroutine]
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    return Client::Impl::task_state(std::move(state), std::move(params), request_options);
}

Task<void> TaskHandle::update(const nlohmann::json& input_responses,
                              const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}, {"inputResponses", input_responses}};
    return Client::Impl::task_ack(std::move(state), methods::g_TASKS_UPDATE, std::move(params),
                                  request_options);
}

Task<void> TaskHandle::cancel(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    return Client::Impl::task_ack(std::move(state), methods::g_TASKS_CANCEL, std::move(params),
                                  request_options);
}

Task<DetailedTask> TaskHandle::wait(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    auto finished = Client::Impl::wait_for_task(std::move(state), std::move(params), request_options);
    return detailed_task(std::move(finished));
}

Task<CallToolResult> TaskHandle::result(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    auto check = tool_ ? state->output_check_for(*tool_) : OutputCheck{};
    if (check.refusal) {
        throw *check.refusal;
    }
    nlohmann::json params{{"taskId", created_.taskId}};
    auto finished = Client::Impl::wait_for_task(std::move(state), std::move(params), request_options);
    return task_result(std::move(finished), std::move(check));
}

std::shared_ptr<const detail::BlockingLoop> TaskHandle::blocking_loop() const {
    return std::static_pointer_cast<Client::Impl>(attached_client(client_))->blocking_loop;
}

SyncTaskHandle TaskHandle::sync() const {
    if (!blocking_loop()) {
        throw std::logic_error(
            "mcp::TaskHandle::sync() needs a task of a client constructed from an Endpoint");
    }
    return SyncTaskHandle(*this);
}

}  // namespace mcp
