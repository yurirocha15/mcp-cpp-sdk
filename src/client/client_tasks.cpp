/// @file client_tasks.cpp
/// @brief Client::Impl's tasks: their state, waiting for one to finish, and tools/call as a task.

#include "client_impl.hpp"

namespace mcp {

Task<nlohmann::json> Client::Impl::task_request(std::shared_ptr<Impl> state, std::string_view method,
                                                nlohmann::json params, RequestOptions request_options) {
    co_return co_await request(state, method, params, request_options, true);
}

Task<DetailedTask> Client::Impl::task_state(std::shared_ptr<Impl> state, nlohmann::json params,
                                            RequestOptions request_options) {
    auto reply = co_await task_request(std::move(state), methods::g_TASKS_GET, std::move(params),
                                       request_options);
    co_return decode_task_state(reply);
}

Task<void> Client::Impl::task_ack(std::shared_ptr<Impl> state, std::string_view method,
                                  nlohmann::json params, RequestOptions request_options) {
    co_await task_request(std::move(state), method, std::move(params), request_options);
}

Task<nlohmann::json> Client::Impl::wait_for_task(std::shared_ptr<Impl> state, nlohmann::json params,
                                                 RequestOptions request_options) {
    const auto deadline = std::chrono::steady_clock::now() + state->options.task_timeout;
    const auto limit = static_cast<std::size_t>(request_options.max_input_requests);
    // The keys already answered, as an object so that no string is held across a suspension.
    nlohmann::json answered = nlohmann::json::object();
    for (;;) {
        nlohmann::json current =
            co_await task_request(state, methods::g_TASKS_GET, params, request_options);
        TaskStatus status = TaskStatus::eWorking;
        std::int64_t poll_ms = state->options.task_poll_interval.count();
        nlohmann::json pending = nlohmann::json::object();
        {
            const auto detailed = decode_task_state(current);
            status = detailed.task.status;
            if (detailed.task.pollIntervalMs && *detailed.task.pollIntervalMs > 0) {
                poll_ms = *detailed.task.pollIntervalMs;
            }
            if (detailed.inputRequests) {
                pending = *detailed.inputRequests;
            }
        }
        if (status == TaskStatus::eCompleted || status == TaskStatus::eFailed ||
            status == TaskStatus::eCancelled) {
            co_return current;
        }

        if (status == TaskStatus::eInputRequired) {
            auto fresh = std::make_shared<nlohmann::json>(nlohmann::json::object());
            for (const auto& entry : pending.items()) {
                if (!answered.contains(entry.key())) {
                    (*fresh)[entry.key()] = entry.value();
                    answered[entry.key()] = true;
                }
            }
            if (!fresh->empty()) {
                std::shared_ptr<const nlohmann::json> questions = std::move(fresh);
                auto responses = co_await answer_input_requests(state, std::move(questions), limit);
                nlohmann::json update = {{"taskId", params.at("taskId")},
                                         {"inputResponses", std::move(responses)}};
                co_await task_request(state, methods::g_TASKS_UPDATE, std::move(update),
                                      request_options);
            }
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT,
                           "Timed out waiting for the task to finish");
        }
        // A cancelled token ends the pause, and the next tasks/get then fails without being sent.
        auto strand = state->strand;
        auto pause =
            pause_unless_cancelled(state, std::min(std::chrono::milliseconds(poll_ms), remaining),
                                   request_options.cancellation);
        co_await detail::spawn_awaitable(strand, std::move(pause));
    }
}

Task<CallToolResult> Client::Impl::call_tool_following(std::shared_ptr<Impl> state,
                                                       nlohmann::json params, OutputCheck check,
                                                       RequestOptions request_options) {
    if (check.refusal) {
        throw *check.refusal;
    }
    const auto route = route_progress(state, params, request_options);
    nlohmann::json reply =
        co_await request(state, methods::g_TOOLS_CALL, params, request_options, true);
    if (is_task_result(reply)) {
        nlohmann::json task_params = {{"taskId", nlohmann::json()}};
        try {
            task_params["taskId"] = reply.get<CreateTaskResult>().task.taskId;
        } catch (const std::exception& error) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "Invalid task: " + detail::sanitize_for_diagnostics(error.what()));
        }
        // Nobody else holds this task, so a call that gives up on it asks the server to stop it.
        std::exception_ptr gave_up;
        nlohmann::json finished;
        try {
            finished = co_await wait_for_task(state, task_params, request_options);
        } catch (...) {
            gave_up = std::current_exception();
        }
        if (gave_up) {
            // The cancel goes out even when the caller's token is what ended the wait.
            RequestOptions cancel_options = request_options;
            cancel_options.cancellation.reset();
            try {
                co_await task_request(state, methods::g_TASKS_CANCEL, task_params, cancel_options);
            } catch (...) {
                // The caller gets the error that ended the wait; a cancel that fails changes
                // nothing.
            }
            std::rethrow_exception(gave_up);
        }
        reply = task_outcome(finished);
    }
    co_return finish_call_result(reply, check);
}

Task<std::variant<CallToolResult, TaskHandle>> Client::Impl::start_tool(
    std::shared_ptr<Impl> state, nlohmann::json params, OutputCheck check,
    RequestOptions request_options) {
    if (check.refusal) {
        throw *check.refusal;
    }
    std::optional<std::string> tool;
    if (params.contains("name") && params["name"].is_string()) {
        tool = params["name"].get<std::string>();
    }
    const auto route = route_progress(state, params, request_options);
    nlohmann::json reply =
        co_await request(state, methods::g_TOOLS_CALL, params, request_options, true);
    if (is_task_result(reply)) {
        try {
            co_return std::variant<CallToolResult, TaskHandle>(
                TaskHandle(state, reply.get<CreateTaskResult>().task, std::move(tool)));
        } catch (const std::exception& error) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "Invalid task: " + detail::sanitize_for_diagnostics(error.what()));
        }
    }
    co_return std::variant<CallToolResult, TaskHandle>(finish_call_result(reply, check));
}

}  // namespace mcp
