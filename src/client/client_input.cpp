/// @file client_input.cpp
/// @brief Client::Impl's answers to a server's input_required results: the requests the client answers
/// and the retry that carries the answers.

#include "client_impl.hpp"

namespace mcp {

bool Client::Impl::supports_input_required(std::string_view method) {
    return method == methods::g_TOOLS_CALL || method == methods::g_PROMPTS_GET ||
           method == methods::g_RESOURCES_READ;
}

Task<nlohmann::json> Client::Impl::answer_input_requests(std::shared_ptr<Impl> state,
                                                         std::shared_ptr<const nlohmann::json> requests,
                                                         std::size_t limit) {
    if (requests->size() > limit) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "The server asked " + std::to_string(requests->size()) +
                           " questions in one input_required result "
                           "(RequestOptions::max_input_requests)");
    }
    nlohmann::json responses = nlohmann::json::object();
    for (const auto& entry : requests->items()) {
        const auto& request = entry.value();
        if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "input_required entry " + detail::sanitize_for_diagnostics(entry.key()) +
                               " is not a request");
        }
        const auto method = std::make_shared<const std::string>(request["method"].get<std::string>());
        RequestHandler handler;
        {
            std::lock_guard lock(state->handlers_mutex);
            const auto found = state->request_handlers.find(*method);
            if (found != state->request_handlers.end()) {
                handler = found->second;
            }
        }
        const bool answerable = *method == methods::g_ELICITATION_CREATE ||
                                *method == methods::g_SAMPLING_CREATE_MESSAGE ||
                                *method == methods::g_ROOTS_LIST;
        if (!answerable || !handler) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "The server asked for " + detail::sanitize_for_diagnostics(*method) +
                               " in an input_required result, but " +
                               (answerable ? "no handler is registered for it"
                                           : "that is not a request a client answers"));
        }
        const nlohmann::json handler_params =
            request.contains("params") ? request["params"] : nlohmann::json::object();
        nlohmann::json answer;
        try {
            answer = co_await handler(handler_params);
        } catch (const nlohmann::json::exception& error) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "input_required entry " + detail::sanitize_for_diagnostics(entry.key()) +
                               " cannot be decoded: " + detail::sanitize_for_diagnostics(error.what()));
        }
        responses[entry.key()] = std::move(answer);
    }
    co_return responses;
}

Task<nlohmann::json> Client::Impl::answers_by(std::shared_ptr<Impl> state,
                                              std::shared_ptr<const nlohmann::json> requests,
                                              std::size_t limit,
                                              std::chrono::steady_clock::time_point deadline) {
    auto outcome = std::make_shared<InputAnswers>(state->strand);
    auto handlers = boost::asio::make_strand(state->strand.get_inner_executor());
    boost::asio::co_spawn(
        handlers, answer_input_requests(state, std::move(requests), limit),
        boost::asio::bind_executor(state->strand,
                                   [outcome](std::exception_ptr failure, nlohmann::json answers) {
                                       outcome->failure = std::move(failure);
                                       outcome->answers = std::move(answers);
                                       outcome->done = true;
                                       outcome->wake.cancel();
                                   }));
    while (!outcome->done && std::chrono::steady_clock::now() < deadline) {
        outcome->wake.expires_at(deadline);
        boost::system::error_code ignored;
        co_await outcome->wake.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
    }
    if (!outcome->done) {
        throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT, "Request timed out");
    }
    if (outcome->failure) {
        std::rethrow_exception(outcome->failure);
    }
    co_return std::move(outcome->answers);
}

Task<nlohmann::json> Client::Impl::resume_input_required(std::shared_ptr<Impl> state,
                                                         std::shared_ptr<const std::string> method,
                                                         std::optional<nlohmann::json> params,
                                                         RequestOptions request_options,
                                                         std::shared_ptr<const std::string> version,
                                                         bool declare_tasks,
                                                         std::shared_ptr<bool> answered_input) {
    const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // A tools/call that declared the extension may be answered with a task, which the caller
    // takes over.
    const bool task_allowed = *method == methods::g_TOOLS_CALL &&
                              (declare_tasks || state->options.tasks == ClientTaskMode::eAuto);
    std::shared_ptr<const nlohmann::json> input_responses;
    std::shared_ptr<const nlohmann::json> request_state;
    for (int round = 0;; ++round) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT, "Request timed out");
        }
        request_options.timeout = remaining;
        auto round_params =
            state->with_request_meta(*version, params, request_options.log_level, declare_tasks);
        if (input_responses) {
            (*round_params)["inputResponses"] = *input_responses;
        }
        if (request_state) {
            (*round_params)["requestState"] = *request_state;
        }
        auto reply = co_await request_wire(state, *method, round_params, request_options, false);
        const auto type = reply.is_object() ? reply.find("resultType") : reply.end();
        if (!reply.is_object() || type == reply.end() || !type->is_string() ||
            *type != "input_required") {
            if (!(task_allowed && is_task_result(reply))) {
                check_complete(reply);
            }
            if (answered_input && round > 0) {
                *answered_input = true;
            }
            co_return reply;
        }
        if (round >= request_options.max_input_rounds) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "The server still asked for input after " +
                               std::to_string(request_options.max_input_rounds) +
                               " rounds (RequestOptions::max_input_rounds)");
        }
        const auto requests = reply.find("inputRequests");
        const auto reply_state = reply.find("requestState");
        const bool has_requests = requests != reply.end();
        const bool has_state = reply_state != reply.end();
        if ((has_requests && !requests->is_object()) || (has_state && !reply_state->is_string())) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "input_required result: inputRequests must be an object and "
                           "requestState a string");
        }
        if (!has_requests && !has_state) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "input_required result has neither inputRequests nor requestState");
        }
        request_state = has_state ? std::make_shared<const nlohmann::json>(*reply_state) : nullptr;
        if (has_requests) {
            auto questions = std::make_shared<const nlohmann::json>(*requests);
            const auto limit = static_cast<std::size_t>(request_options.max_input_requests);
            auto strand = state->strand;
            auto answers = co_await detail::spawn_awaitable(
                strand, answers_by(state, std::move(questions), limit, deadline));
            input_responses = std::make_shared<const nlohmann::json>(std::move(answers));
        } else {
            input_responses = nullptr;
        }
    }
}

}  // namespace mcp
