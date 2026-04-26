/// @file elicitation.cpp
/// @brief Demonstrates Context::elicit() for requesting user input via form-based elicitation.
///
/// This example shows:
/// - A server tool that calls ctx.elicit() to request user information
/// - A form-based elicitation request with schema
/// - A client that registers on_elicitation() handler to respond
/// - The client returning ElicitResult with action=accept and form values
/// - The server using the elicited values to complete the tool execution

#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "elicitation-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        nlohmann::json tool_schema = {{"type", "object"},
                                      {"properties", {{"task", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"task"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "request_user_info", "A tool that elicits user information via form-based elicitation",
            tool_schema, [](Context& ctx, const nlohmann::json& args) -> Task<nlohmann::json> {
                std::string task = args.at("task").get<std::string>();
                std::cout << "Tool: Starting request_user_info for task: " << task << "\n";

                ElicitRequestFormParams form_params;
                form_params.mode = "form";
                form_params.message = "Please provide your contact information";
                form_params.requestedSchema = nlohmann::json{
                    {"type", "object"},
                    {"properties",
                     nlohmann::json{
                         {"name", nlohmann::json{{"type", "string"}, {"title", "Full Name"}}},
                         {"email", nlohmann::json{{"type", "string"}, {"title", "Email Address"}}},
                         {"priority",
                          nlohmann::json{{"type", "string"},
                                         {"title", "Priority Level"},
                                         {"enum", nlohmann::json::array({"low", "medium", "high"})}}}}},
                    {"required", nlohmann::json::array({"name", "email"})}};

                std::cout << "Tool: Sending elicitation request\n";
                ElicitRequestParams params = form_params;
                auto result = co_await ctx.elicit(params);

                std::cout << "Tool: Received elicitation result\n";

                if (result.action == ElicitAction::eAccept) {
                    std::cout << "Tool: User accepted the form\n";
                    if (result.content) {
                        std::cout << "Tool: Form data: " << result.content->dump() << "\n";
                        co_return nlohmann::json{
                            {"status", "success"}, {"task", task}, {"user_info", *result.content}};
                    }
                } else if (result.action == ElicitAction::eDecline) {
                    std::cout << "Tool: User declined the form\n";
                    co_return nlohmann::json{{"status", "declined"}, {"task", task}};
                } else if (result.action == ElicitAction::eCancel) {
                    std::cout << "Tool: User cancelled the form\n";
                    co_return nlohmann::json{{"status", "cancelled"}, {"task", task}};
                }

                co_return nlohmann::json{
                    {"status", "error"}, {"task", task}, {"message", "Unexpected elicitation result"}};
            });

        // ========== TRANSPORT & RUN ==========
        auto [client_transport, server_transport] =
            mcp::create_memory_transport_pair(io_ctx.get_executor());

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    co_await server.run(server_transport, io_ctx.get_executor());
                } catch (const std::exception& e) {
                    std::cerr << "Server error: " << e.what() << "\n";
                }
            },
            asio::detached);

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    std::cout << "Client: Waiting for server to start\n";
                    asio::steady_timer delay_timer(io_ctx.get_executor());
                    delay_timer.expires_after(std::chrono::milliseconds(50));
                    co_await delay_timer.async_wait(asio::use_awaitable);

                    std::cout << "Client: Creating client\n";
                    mcp::Client client(client_transport, io_ctx.get_executor());

                    Implementation client_info;
                    client_info.name = "elicitation-client";
                    client_info.version = "1.0.0";

                    std::cout << "Client: Connecting to server\n";
                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "Client: Connected to " << init_result.serverInfo.name << "\n";

                    bool elicitation_received = false;

                    client.on_elicitation([&](const ElicitRequestParams& params) -> Task<ElicitResult> {
                        std::cout << "Client: Received elicitation request\n";
                        elicitation_received = true;

                        if (std::holds_alternative<ElicitRequestFormParams>(params)) {
                            auto form_params = std::get<ElicitRequestFormParams>(params);
                            std::cout << "Client: Form message: " << form_params.message << "\n";
                            std::cout << "Client: Form schema: " << form_params.requestedSchema.dump()
                                      << "\n";

                            nlohmann::json form_data = {{"name", "Alice Johnson"},
                                                        {"email", "alice@example.com"},
                                                        {"priority", "high"}};

                            std::cout << "Client: Submitting form with data: " << form_data.dump()
                                      << "\n";

                            ElicitResult result;
                            result.action = ElicitAction::eAccept;
                            result.content = form_data;
                            co_return result;
                        }

                        ElicitResult result;
                        result.action = ElicitAction::eDecline;
                        co_return result;
                    });

                    std::cout << "Client: Calling request_user_info tool\n";
                    nlohmann::json call_args = {{"task", "collect_feedback"}};
                    try {
                        auto result = co_await client.call_tool("request_user_info", call_args);
                        std::cout << "Client: Tool completed with " << result.content.size()
                                  << " content blocks\n";
                        for (size_t i = 0; i < result.content.size(); ++i) {
                            if (auto* text = std::get_if<mcp::TextContent>(&result.content[i])) {
                                std::cout << "Client: Content block " << i << ": " << text->text
                                          << "\n";
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cout << "Client: Tool call error: " << e.what() << "\n";
                    }

                    if (elicitation_received) {
                        std::cout << "Client: Successfully handled elicitation request\n";
                    }

                    std::cout << "Client: Closing connection\n";
                    client.close();

                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
            },
            asio::detached);

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                asio::steady_timer exit_timer(io_ctx.get_executor());
                exit_timer.expires_after(std::chrono::seconds(5));
                co_await exit_timer.async_wait(asio::use_awaitable);
                io_ctx.stop();
            },
            asio::detached);

        io_ctx.run();

        std::cout << "Example completed successfully\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unknown exception\n";
        return EXIT_FAILURE;
    }
}
