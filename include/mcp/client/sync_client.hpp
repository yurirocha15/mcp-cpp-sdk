#pragma once

#include <mcp/client/client_protocol.hpp>
#include <mcp/client/request_options.hpp>
#include <mcp/client/subscription.hpp>
#include <mcp/client/task_handle.hpp>
#include <mcp/core/concepts.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/protocol.hpp>

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace mcp {

class Client;

/**
 * @brief Blocking calls to a Subscription; see Subscription::sync().
 */
class MCP_API SyncSubscription {
   public:
    explicit SyncSubscription(Subscription subscription);

    /// As Subscription::next(), waiting for the next event or the end of the stream.
    std::optional<SubscriptionEvent> next();
    /// As Subscription::close().
    void close();

   private:
    Subscription subscription_;
};

/**
 * @brief Blocking calls to a TaskHandle; see TaskHandle::sync().
 */
class MCP_API SyncTaskHandle {
   public:
    explicit SyncTaskHandle(TaskHandle handle);

    /// As TaskHandle::get().
    [[nodiscard]] DetailedTask get(const RequestOptions& request_options = {}) const;
    /// As TaskHandle::update().
    void update(const nlohmann::json& input_responses,
                const RequestOptions& request_options = {}) const;
    /// As TaskHandle::cancel().
    void cancel(const RequestOptions& request_options = {}) const;
    /// As TaskHandle::wait().
    [[nodiscard]] DetailedTask wait(const RequestOptions& request_options = {}) const;
    /// As TaskHandle::result().
    [[nodiscard]] CallToolResult result(const RequestOptions& request_options = {}) const;

   private:
    TaskHandle handle_;
};

/**
 * @brief Blocking calls to a Client; see Client::sync().
 *
 * @details Each method runs the Client coroutine member of the same name, with the same
 * arguments, on the client's event loop and returns its result or throws its exception. A
 * SyncClient refers to its Client, which must outlive it.
 */
class MCP_API SyncClient {
   public:
    explicit SyncClient(Client& client);

    /// As Client::connect().
    ConnectResult connect(std::string_view name, std::string_view version);
    /// As Client::connect().
    ConnectResult connect(const Implementation& client_info, const ClientCapabilities& capabilities);
    /// As Client::send_request().
    nlohmann::json send_request(std::string_view method, const std::optional<nlohmann::json>& params,
                                const RequestOptions& request_options = {});
    /// As Client::listen(); read the stream through Subscription::sync().
    Subscription listen(const SubscriptionFilter& filter, const RequestOptions& request_options = {});
    /// As Client::send_notification().
    void send_notification(std::string_view method, const std::optional<nlohmann::json>& params);
    /// As Client::close().
    void close();

    /// As Client::call_tool().
    CallToolResult call_tool(const std::string& name, const nlohmann::json& arguments,
                             const RequestOptions& request_options = {});
    /// As Client::call_tool(), with arguments of a type that converts to JSON.
    template <JsonSerializable Args>
        requires(!std::same_as<Args, nlohmann::json>)
    CallToolResult call_tool(const std::string& name, const Args& arguments,
                             const RequestOptions& request_options = {}) {
        return call_tool(name, nlohmann::json(arguments), request_options);
    }
    /// As Client::start_tool(); follow a task through TaskHandle::sync().
    std::variant<CallToolResult, TaskHandle> start_tool(const std::string& name,
                                                        const nlohmann::json& arguments,
                                                        const RequestOptions& request_options = {});

    /// As Client::list_tools().
    ListToolsResult list_tools(const std::optional<std::string>& cursor = std::nullopt,
                               const RequestOptions& request_options = {});
    /// As Client::list_resources().
    ListResourcesResult list_resources(const std::optional<std::string>& cursor = std::nullopt,
                                       const RequestOptions& request_options = {});
    /// As Client::read_resource().
    ReadResourceResult read_resource(const std::string& uri,
                                     const RequestOptions& request_options = {});
    /// As Client::list_resource_templates().
    ListResourceTemplatesResult list_resource_templates(
        const std::optional<std::string>& cursor = std::nullopt,
        const RequestOptions& request_options = {});
    /// As Client::list_prompts().
    ListPromptsResult list_prompts(const std::optional<std::string>& cursor = std::nullopt,
                                   const RequestOptions& request_options = {});
    /// As Client::get_prompt().
    GetPromptResult get_prompt(const std::string& name,
                               const std::map<std::string, std::string>& arguments = {},
                               const RequestOptions& request_options = {});
    /// As Client::complete().
    CompleteResult complete(const CompleteParams& params, const RequestOptions& request_options = {});
    /// As Client::ping().
    void ping();
    /// As Client::cancel().
    void cancel(const RequestId& request_id, const std::optional<std::string>& reason = std::nullopt);

   private:
    Client* client_;
};

}  // namespace mcp
