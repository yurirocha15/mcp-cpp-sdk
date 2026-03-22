#pragma once

#include <mcp/concepts.hpp>
#include <mcp/context.hpp>
#include <mcp/core.hpp>
#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp {

/// @brief Type-erased handler: takes a Context and JSON params, returns JSON result.
using TypeErasedHandler = std::function<Task<nlohmann::json>(Context&, const nlohmann::json&)>;

namespace detail {

/// @brief Detects whether Fn is invocable with (In) returning Task<Out>.
template <typename Fn, typename In, typename Out>
concept AsyncHandlerNoCtx = requires(Fn fn, In in) {
    { fn(std::move(in)) } -> std::same_as<Task<Out>>;
};

/// @brief Detects whether Fn is invocable with (Context&, In) returning Task<Out>.
template <typename Fn, typename In, typename Out>
concept AsyncHandlerWithCtx = requires(Fn fn, Context& ctx, In in) {
    { fn(ctx, std::move(in)) } -> std::same_as<Task<Out>>;
};

/// @brief Detects whether Fn is invocable with (In) returning Out (sync, no context).
template <typename Fn, typename In, typename Out>
concept SyncHandlerNoCtx = requires(Fn fn, In in) {
    { fn(std::move(in)) } -> std::same_as<Out>;
};

/// @brief Detects whether Fn is invocable with (Context&, In) returning Out (sync, with context).
template <typename Fn, typename In, typename Out>
concept SyncHandlerWithCtx = requires(Fn fn, Context& ctx, In in) {
    { fn(ctx, std::move(in)) } -> std::same_as<Out>;
};

/**
 * @brief Wraps a handler of any supported signature into a TypeErasedHandler.
 *
 * @details Supports four handler signatures via if constexpr:
 * - Fn(In) -> Task<Out>         (async, no context)
 * - Fn(Context&, In) -> Task<Out>  (async, with context)
 * - Fn(In) -> Out               (sync, no context)
 * - Fn(Context&, In) -> Out     (sync, with context)
 *
 * @tparam In  The input type (must satisfy JsonSerializable).
 * @tparam Out The output type (must satisfy JsonSerializable).
 * @tparam Fn  The handler callable type.
 * @param fn The handler to wrap.
 * @return A TypeErasedHandler that deserializes JSON to In, calls fn, and serializes Out to JSON.
 */
template <JsonSerializable In, JsonSerializable Out, typename Fn>
    requires AsyncHandlerWithCtx<Fn, In, Out> || AsyncHandlerNoCtx<Fn, In, Out> ||
             SyncHandlerWithCtx<Fn, In, Out> || SyncHandlerNoCtx<Fn, In, Out>
TypeErasedHandler wrap_handler(Fn fn) {
    return
        [handler = std::move(fn)](Context& ctx, const nlohmann::json& params) -> Task<nlohmann::json> {
            auto input = params.template get<In>();
            if constexpr (AsyncHandlerWithCtx<Fn, In, Out>) {
                Out output = co_await handler(ctx, std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            } else if constexpr (AsyncHandlerNoCtx<Fn, In, Out>) {
                Out output = co_await handler(std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            } else if constexpr (SyncHandlerWithCtx<Fn, In, Out>) {
                Out output = handler(ctx, std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            } else {
                Out output = handler(std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            }
        };
}

}  // namespace detail

/**
 * @brief MCP server that handles client requests over a transport.
 *
 * @details Manages the JSON-RPC message loop, dispatching incoming
 * requests to the appropriate handler. Supports the MCP initialize
 * handshake and shutdown flow, as well as tool, resource, and prompt
 * handler registration and dispatch. Creates a Context per request
 * for handler use.
 *
 * All async operations execute on a boost::asio::strand for thread
 * safety without std::mutex.
 */
class Server {
   public:
    /**
     * @brief Construct a Server.
     *
     * @param server_info Information about this server implementation.
     * @param capabilities The capabilities this server advertises.
     */
    Server(Implementation server_info, ServerCapabilities capabilities)
        : server_info_(std::move(server_info)), capabilities_(std::move(capabilities)) {}

    ~Server() = default;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /**
     * @brief Register a tool with the server.
     *
     * @details Stores the tool metadata and wraps the handler into a
     * type-erased function. The handler is called when a tools/call
     * request matches the tool's name.
     *
     * @tparam In  The input type for the handler (must satisfy JsonSerializable).
     * @tparam Out The output type for the handler (must satisfy JsonSerializable).
     * @tparam Fn  The handler callable type (sync/async, with/without Context).
     * @param name The name of the tool.
     * @param description A human-readable description of the tool.
     * @param input_schema The JSON schema describing the tool's input.
     * @param handler The handler function to invoke on tools/call.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_tool(std::string name, std::string description, nlohmann::json input_schema, Fn handler) {
        Tool tool;
        tool.name = name;
        tool.description = std::move(description);
        tool.inputSchema = std::move(input_schema);
        tools_.push_back(std::move(tool));

        tool_handlers_.emplace(std::move(name), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Register a resource with the server.
     *
     * @details Stores the resource metadata and wraps the handler into a
     * type-erased function. The handler is called when a resources/read
     * request matches the resource's URI.
     *
     * @tparam In  The input type for the handler (must satisfy JsonSerializable).
     * @tparam Out The output type for the handler (must satisfy JsonSerializable).
     * @tparam Fn  The handler callable type (sync/async, with/without Context).
     * @param resource The resource metadata.
     * @param handler The handler function to invoke on resources/read.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_resource(Resource resource, Fn handler) {
        auto uri = resource.uri;
        resources_.push_back(std::move(resource));
        resource_handlers_.emplace(std::move(uri), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Register a resource template with the server.
     *
     * @details Stores the resource template metadata for listing.
     * Resource templates describe parameterized resources that clients
     * can discover via resources/templates/list.
     *
     * @param tmpl The resource template metadata.
     */
    void add_resource_template(ResourceTemplate tmpl) {
        resource_templates_.push_back(std::move(tmpl));
    }

    /**
     * @brief Register a prompt with the server.
     *
     * @details Stores the prompt metadata and wraps the handler into a
     * type-erased function. The handler is called when a prompts/get
     * request matches the prompt's name.
     *
     * @tparam In  The input type for the handler (must satisfy JsonSerializable).
     * @tparam Out The output type for the handler (must satisfy JsonSerializable).
     * @tparam Fn  The handler callable type (sync/async, with/without Context).
     * @param prompt The prompt metadata.
     * @param handler The handler function to invoke on prompts/get.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_prompt(Prompt prompt, Fn handler) {
        auto name = prompt.name;
        prompts_.push_back(std::move(prompt));
        prompt_handlers_.emplace(std::move(name), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Start the server session loop on the given transport.
     *
     * @details Reads messages from the transport, parses them as
     * JSON-RPC, and dispatches each to the appropriate handler.
     * The loop runs until the transport is closed or an error occurs.
     *
     * @param transport The transport to use for message exchange.
     *                  Ownership is transferred to the server.
     * @param executor The executor to use for async operations.
     */
    Task<void> run(std::unique_ptr<ITransport> transport,
                   const boost::asio::any_io_executor& executor) {
        transport_ = std::move(transport);
        strand_ = std::make_unique<boost::asio::strand<boost::asio::any_io_executor>>(
            boost::asio::make_strand(executor));

        try {
            for (;;) {
                auto raw = co_await transport_->read_message();
                auto json_msg = nlohmann::json::parse(raw);
                co_await dispatch(json_msg);
            }
        } catch (const std::exception&) {
        }
    }

    /**
     * @brief Dispatch a parsed JSON-RPC message to the appropriate handler.
     *
     * @details Routes requests based on the "method" field. Handles
     * "initialize" and "shutdown" internally. Routes tool, resource,
     * and prompt methods to their respective handlers. Returns a
     * JSON-RPC error response for unknown methods. Notifications
     * (messages without "id") are silently ignored.
     *
     * @param json_msg The parsed JSON-RPC message.
     */
    Task<void> dispatch(const nlohmann::json& json_msg) {
        if (!json_msg.contains("id")) {
            co_return;
        }

        auto request = json_msg.get<JSONRPCRequest>();

        if (request.method == "initialize") {
            co_await handle_initialize(request);
        } else if (request.method == "shutdown") {
            co_await handle_shutdown(request);
        } else if (request.method == "tools/call") {
            co_await handle_tools_call(request);
        } else if (request.method == "tools/list") {
            co_await handle_tools_list(request);
        } else if (request.method == "resources/list") {
            co_await handle_resources_list(request);
        } else if (request.method == "resources/read") {
            co_await handle_resources_read(request);
        } else if (request.method == "resources/templates/list") {
            co_await handle_resource_templates_list(request);
        } else if (request.method == "prompts/list") {
            co_await handle_prompts_list(request);
        } else if (request.method == "prompts/get") {
            co_await handle_prompts_get(request);
        } else {
            co_await send_error(request.id, METHOD_NOT_FOUND, "Method not found: " + request.method);
        }
    }

    /**
     * @brief Check whether the server has been initialized.
     *
     * @return true if an initialize request has been handled.
     */
    [[nodiscard]] bool is_initialized() const { return initialized_; }

    /**
     * @brief Check whether a shutdown has been requested.
     *
     * @return true if a shutdown request has been handled.
     */
    [[nodiscard]] bool is_shutdown_requested() const { return shutdown_requested_; }

   private:
    Task<void> handle_initialize(const JSONRPCRequest& request) {
        InitializeResult result;
        result.protocolVersion = std::string(LATEST_PROTOCOL_VERSION);
        result.capabilities = capabilities_;
        result.serverInfo = server_info_;

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
        initialized_ = true;
    }

    Task<void> handle_shutdown(const JSONRPCRequest& request) {
        shutdown_requested_ = true;
        co_await send_result(request.id, nlohmann::json::object());
    }

    Task<void> handle_tools_call(const JSONRPCRequest& request) {
        auto params = request.params.value().get<CallToolParams>();
        auto iter = tool_handlers_.find(params.name);
        if (iter == tool_handlers_.end()) {
            co_await send_error(request.id, METHOD_NOT_FOUND, "Unknown tool: " + params.name);
            co_return;
        }

        Context ctx(*transport_);
        nlohmann::json result = co_await iter->second(ctx, params.arguments);
        co_await send_result(request.id, std::move(result));
    }

    Task<void> handle_tools_list(const JSONRPCRequest& request) {
        ListToolsResult result;
        result.tools = tools_;

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
    }

    Task<void> handle_resources_list(const JSONRPCRequest& request) {
        ListResourcesResult result;
        result.resources = resources_;

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
    }

    Task<void> handle_resources_read(const JSONRPCRequest& request) {
        auto params = request.params.value().get<ReadResourceRequestParams>();
        auto iter = resource_handlers_.find(params.uri);
        if (iter == resource_handlers_.end()) {
            co_await send_error(request.id, INVALID_PARAMS, "Unknown resource: " + params.uri);
            co_return;
        }

        nlohmann::json params_json = std::move(params);
        Context ctx(*transport_);
        nlohmann::json result = co_await iter->second(ctx, params_json);
        co_await send_result(request.id, std::move(result));
    }

    Task<void> handle_resource_templates_list(const JSONRPCRequest& request) {
        ListResourceTemplatesResult result;
        result.resourceTemplates = resource_templates_;

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
    }

    Task<void> handle_prompts_list(const JSONRPCRequest& request) {
        ListPromptsResult result;
        result.prompts = prompts_;

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
    }

    Task<void> handle_prompts_get(const JSONRPCRequest& request) {
        auto params = request.params.value().get<GetPromptRequestParams>();
        auto iter = prompt_handlers_.find(params.name);
        if (iter == prompt_handlers_.end()) {
            co_await send_error(request.id, METHOD_NOT_FOUND, "Unknown prompt: " + params.name);
            co_return;
        }

        nlohmann::json params_json = std::move(params);
        Context ctx(*transport_);
        nlohmann::json result = co_await iter->second(ctx, params_json);
        co_await send_result(request.id, std::move(result));
    }

    Task<void> send_result(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);

        nlohmann::json json_msg = std::move(response);
        co_await transport_->write_message(json_msg.dump());
    }

    Task<void> send_error(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);

        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);

        nlohmann::json json_msg = std::move(response);
        co_await transport_->write_message(json_msg.dump());
    }

    Implementation server_info_;
    ServerCapabilities capabilities_;
    std::unique_ptr<ITransport> transport_;
    std::unique_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;

    /// @brief Registered tool metadata.
    std::vector<Tool> tools_;
    /// @brief Type-erased tool handlers keyed by tool name.
    std::map<std::string, TypeErasedHandler, std::less<>> tool_handlers_;

    /// @brief Registered resource metadata.
    std::vector<Resource> resources_;
    /// @brief Type-erased resource handlers keyed by resource URI.
    std::map<std::string, TypeErasedHandler, std::less<>> resource_handlers_;

    /// @brief Registered resource template metadata.
    std::vector<ResourceTemplate> resource_templates_;

    /// @brief Registered prompt metadata.
    std::vector<Prompt> prompts_;
    /// @brief Type-erased prompt handlers keyed by prompt name.
    std::map<std::string, TypeErasedHandler, std::less<>> prompt_handlers_;
};

}  // namespace mcp
