#pragma once

#include <mcp/core.hpp>
#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <string_view>

namespace mcp {

/**
 * @brief Request-scoped context provided to server handlers.
 *
 * @details Each incoming request receives a Context that provides
 * utilities for interacting with the client, such as sending log
 * messages. The Context holds a non-owning reference to the
 * transport, which must outlive the Context.
 */
class Context {
   public:
    /**
     * @brief Construct a Context.
     *
     * @param transport Non-owning reference to the transport for
     *                  sending notifications back to the client.
     */
    explicit Context(ITransport& transport) : transport_(transport) {}

    /**
     * @brief Send an info-level log message to the client.
     *
     * @details Sends a "notifications/message" notification with
     * LoggingLevel::Info and the given message as the data payload.
     *
     * @param msg The message to log.
     */
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

   private:
    ITransport& transport_;
};

}  // namespace mcp
