#pragma once

#include <mcp/core.hpp>
#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/// @brief Type alias for a function that sends a JSON-RPC request and returns the result.
using RequestSender = std::function<Task<nlohmann::json>(std::string, std::optional<nlohmann::json>)>;

/**
 * @brief Request-scoped context provided to server handlers.
 *
 * @details Each incoming request receives a Context that provides
 * utilities for interacting with the client, such as sending log
 * messages and performing reverse RPC (e.g. sampling). The Context
 * holds a non-owning reference to the transport and a request sender
 * callable, both of which must outlive the Context.
 */
class Context {
   public:
    /**
     * @brief Construct a Context with transport only (no reverse RPC support).
     *
     * @param transport Non-owning reference to the transport for
     *                  sending notifications back to the client.
     */
    explicit Context(ITransport& transport) : transport_(transport) {}

    /**
     * @brief Construct a Context with transport and request sender.
     *
     * @param transport Non-owning reference to the transport.
     * @param sender A callable that sends a JSON-RPC request and returns the result.
     */
    Context(ITransport& transport, RequestSender sender)
        : transport_(transport), sender_(std::move(sender)) {}

    Task<void> log_info(std::string_view msg) {
        LoggingMessageNotificationParams params;
        params.level = LoggingLevel::Info;
        params.data = std::string(msg);

        JSONRPCNotification notification;
        notification.method = "notifications/message";
        nlohmann::json p = std::move(params);
        notification.params = std::move(p);

        nlohmann::json json_msg = std::move(notification);
        co_await transport_.write_message(json_msg.dump());
    }

    /**
     * @brief Request the client to sample an LLM (reverse RPC).
     *
     * @details Sends a "sampling/createMessage" request to the client
     * and awaits the response. Requires the Context to be constructed
     * with a RequestSender.
     *
     * @param request The sampling request parameters.
     * @return A task that resolves to the client's CreateMessageResult.
     * @throws std::runtime_error If no request sender is available.
     */
    Task<CreateMessageResult> sample_llm(CreateMessageRequestParams request) {
        if (!sender_) {
            throw std::runtime_error("Context has no request sender for reverse RPC");
        }

        nlohmann::json params = std::move(request);
        auto result_json = co_await sender_("sampling/createMessage", std::move(params));
        co_return result_json.get<CreateMessageResult>();
    }

   private:
    ITransport& transport_;
    RequestSender sender_;
};

}  // namespace mcp
