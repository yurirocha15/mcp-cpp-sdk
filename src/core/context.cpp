#include <mcp/core/constants.hpp>
#include "mcp/detail/async/input_activity.hpp"
#include "mcp/detail/protocol/notification_budget.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/context.hpp>
#include <mcp/detail/deprecation_reporter.hpp>
#include <mcp/detail/protocol/elicitation_id.hpp>
#include <mcp/transport/transport.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).

namespace methods = mcp::constants::methods;

namespace mcp {

namespace {

Task<bool> not_sent() { co_return false; }

}  // namespace

namespace detail {

std::string generate_elicitation_id() {
    static std::atomic<std::uint64_t> s_counter{0};
    static const std::uint64_t s_seed = std::random_device{}();
    return "elicit-" + std::to_string(s_seed) + "-" + std::to_string(s_counter.fetch_add(1));
}

}  // namespace detail

void Context::report_deprecated(DeprecatedFeature feature) const {
    if (deprecations_) {
        deprecations_->report(feature);
    }
}

Task<void> Context::log_info(std::string_view msg) { co_await log(LoggingLevel::eInfo, msg); }

Task<void> Context::log(LoggingLevel level, std::string_view msg,
                        const std::optional<std::string>& logger) {
    report_deprecated(DeprecatedFeature::eLogging);
    if (request_meta_) {
        if (!request_meta_->logLevel ||
            static_cast<uint8_t>(level) > static_cast<uint8_t>(*request_meta_->logLevel)) {
            co_return;
        }
    } else if (log_level_ != nullptr) {
        auto current = log_level_->load(std::memory_order_relaxed);
        if (static_cast<uint8_t>(level) > static_cast<uint8_t>(current)) {
            co_return;
        }
    }
    if (budget_ && !budget_->allow_log()) {
        co_return;
    }

    // [gcc11-sso: scope-before-await]
    std::shared_ptr<const std::string> wire;
    {
        LoggingMessageNotificationParams params;
        params.level = level;
        params.data = std::string(msg);
        params.logger = logger;

        JSONRPCNotification notification;
        notification.method = methods::g_NOTIFICATIONS_MESSAGE;
        notification.params = nlohmann::json(std::move(params));
        wire = std::make_shared<const std::string>(
            mcp::detail::dump_json(nlohmann::json(std::move(notification))));
    }
    if (message_sender_) {
        co_await message_sender_(*wire);
    } else {
        co_await transport_.write_message(*wire);
    }
}

Task<void> Context::report_progress(double progress, std::optional<double> total,
                                    const std::optional<std::string>& message) {
    if (progress_sink_) {
        progress_sink_(progress, total, message);
        co_return;
    }
    if (!progress_token_) {
        co_return;
    }
    if (budget_ && !budget_->allow_progress()) {
        co_return;
    }

    // [gcc11-sso: scope-before-await]
    std::shared_ptr<const std::string> wire;
    {
        ProgressNotificationParams params;
        params.progressToken = *progress_token_;
        params.progress = progress;
        params.total = total;
        params.message = message;

        JSONRPCNotification notification;
        notification.method = methods::g_NOTIFICATIONS_PROGRESS;
        notification.params = nlohmann::json(std::move(params));
        wire = std::make_shared<const std::string>(
            mcp::detail::dump_json(nlohmann::json(std::move(notification))));
    }
    if (message_sender_) {
        co_await message_sender_(*wire);
    } else {
        co_await transport_.write_message(*wire);
    }
}

Task<CreateMessageResult> Context::sample_llm(const CreateMessageRequestParams& request,
                                              InputOptions options) {
    report_deprecated(DeprecatedFeature::eSampling);
    if (request.includeContext && *request.includeContext != constants::include_context::g_NONE) {
        report_deprecated(DeprecatedFeature::eIncludeContext);
    }
    if (!sender_) {
        throw std::runtime_error("Context has no request sender for reverse RPC");
    }

    nlohmann::json params = request;
    const detail::OpenQuestion open(input_activity_);
    auto result_json = co_await sender_(std::string(methods::g_SAMPLING_CREATE_MESSAGE),
                                        std::move(params), std::move(options.key));
    co_return result_json.get<CreateMessageResult>();
}

Task<ElicitResult> Context::elicit(const ElicitRequestParams& params, InputOptions options) {
    if (!sender_) {
        throw std::runtime_error("Context has no request sender for reverse RPC");
    }

    nlohmann::json json_params = params;
    if (json_params.value("mode", "") == "url") {
        if (!json_params.contains("elicitationId")) {
            // Before 2026-07-28 the field is required on the wire.
            json_params["elicitationId"] = detail::generate_elicitation_id();
        }
        if (elicitation_recorder_) {
            elicitation_recorder_(json_params.at("elicitationId").get<std::string>());
        }
    }
    const detail::OpenQuestion open(input_activity_);
    auto result_json = co_await sender_(std::string(methods::g_ELICITATION_CREATE),
                                        std::move(json_params), std::move(options.key));
    auto result = result_json.get<ElicitResult>();
    if (options.validate_content) {
        check_form_content(params, result);
    }
    co_return result;
}

Task<bool> Context::notify_elicitation_complete(const std::string& elicitation_id) {
    // [gcc11-sso: not-a-coroutine] The completer copies the id before the task starts, so the
    // caller's string need not outlive the task.
    if (!elicitation_completer_) {
        return not_sent();
    }
    return elicitation_completer_(elicitation_id);
}

Task<ListRootsResult> Context::request_roots(InputOptions options) {
    report_deprecated(DeprecatedFeature::eRoots);
    if (!sender_) {
        throw std::runtime_error("Context has no request sender for reverse RPC");
    }

    const detail::OpenQuestion open(input_activity_);
    auto result_json =
        co_await sender_(std::string(methods::g_ROOTS_LIST), std::nullopt, std::move(options.key));
    co_return result_json.get<ListRootsResult>();
}

}  // namespace mcp
