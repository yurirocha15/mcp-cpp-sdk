/// @file elicitation.cpp
/// @brief Demonstrates Context::elicit() for requesting user input via form-based elicitation.
///
/// This example shows:
/// - A server tool that calls ctx.elicit() to request user information
/// - A form-based elicitation request with schema
/// - A client that registers on_elicitation() handler to respond
/// - The client returning ElicitResult with action=accept and form values
/// - The server using the elicited values to complete the tool execution
/// - ConnectResult::era, and how the same ctx.elicit() call runs in each era
/// - ElicitationSchema, which builds the form from typed fields (string with a format, titled
///   single-select, multi-select, number, boolean) and decodes the accepted content
/// - The client checking its answer against the requested schema before it is sent
///
/// In a 2026-07-28 session the server cannot send a request to the client while it serves
/// another. When the tool calls ctx.elicit(), the server answers the tools/call with an
/// input_required result that holds the question; the client runs its on_elicitation() handler and
/// sends the tools/call again with the answer, and the tool then resumes. The tool and the handler
/// are written the same way in both eras; with a client pinned to ClientProtocol::legacy() the
/// server sends an elicitation/create request instead. Server::mrtr_stats() counts the retries
/// that resumed a waiting tool.

#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/server/server.hpp>
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
#include <stdexcept>
#include <string>
#include <vector>

namespace asio = boost::asio;

namespace {

/// The form the tool asks, built from typed fields. A function-local static keeps its strings out
/// of the tool's coroutine frame.
const mcp::ElicitationSchema& contact_form() {
    static const mcp::ElicitationSchema s_form = [] {
        using namespace mcp;
        ElicitationSchema built;
        built.add("name", StringSchema{.title = "Full Name", .minLength = 1}, true)
            .add("email", StringSchema{.title = "Email Address", .format = "email"}, true)
            .add("priority",
                 TitledEnumSchema{.title = "Priority Level",
                                  .options = {{"low", "Low"}, {"medium", "Medium"}, {"high", "High"}},
                                  .defaultValue = "medium"})
            .add("topics",
                 MultiSelectEnumSchema{
                     .title = "Topics", .minItems = 1, .enumValues = {"bugs", "features", "docs"}})
            .add("age", NumberSchema{.integer = true, .title = "Age", .minimum = 18})
            .add("subscribe", BooleanSchema{.title = "Send me updates", .defaultValue = false});
        return built;
    }();
    return s_form;
}

}  // namespace

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;
        bool succeeded = false;

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

                const auto& form = contact_form();

                ElicitRequestFormParams form_params;
                form_params.mode = "form";
                form_params.message = "Please provide your contact information";
                form_params.requestedSchema = form;

                std::cout << "Tool: Sending elicitation request\n";
                ElicitRequestParams params = form_params;
                auto result = co_await ctx.elicit(params);

                std::cout << "Tool: Received elicitation result\n";

                auto make_result = [](const std::string& text) {
                    return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                                          {"type", "text"}, {"text", text}}})}};
                };

                if (result.action == ElicitAction::eAccept) {
                    std::cout << "Tool: User accepted the form\n";
                    if (result.content) {
                        // Context::elicit() has checked the content against the schema; decode()
                        // reads it as typed values.
                        const auto values = form.decode(*result.content);
                        const auto name = values.get<std::string>("name").value_or("");
                        const auto topics = values.get<std::vector<std::string>>("topics").value_or(
                            std::vector<std::string>{});
                        std::cout << "Tool: Form data: " << result.content->dump() << "\n";
                        co_return make_result(
                            "Accepted: task=" + task + " name=" + name +
                            " priority=" + values.get<std::string>("priority").value_or("-") +
                            " topics=" + std::to_string(topics.size()));
                    }
                    co_return make_result("Accepted: task=" + task + " (no form data)");
                } else if (result.action == ElicitAction::eDecline) {
                    std::cout << "Tool: User declined the form\n";
                    co_return make_result("Declined: task=" + task);
                } else {
                    std::cout << "Tool: User cancelled the form\n";
                    co_return make_result("Cancelled: task=" + task);
                }
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
                    std::cout << "Client: Creating client\n";
                    mcp::Client client(client_transport, io_ctx.get_executor());

                    Implementation client_info;
                    client_info.name = "elicitation-client";
                    client_info.version = "1.0.0";

                    std::cout << "Client: Connecting to server\n";
                    bool modern_session = false;
                    {
                        auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                        std::cout << "Client: Connected, era: "
                                  << (init_result.era == ProtocolEra::eModern ? "modern" : "legacy")
                                  << ", protocol version: " << init_result.protocolVersion << "\n";
                        if (init_result.serverInfo) {
                            std::cout << "Client: Server is " << init_result.serverInfo->name << "\n";
                        }
                        modern_session = init_result.era == ProtocolEra::eModern;
                    }

                    bool elicitation_received = false;

                    client.on_elicitation([&](const ElicitRequestParams& params) -> Task<ElicitResult> {
                        std::cout << "Client: Received elicitation request\n";
                        elicitation_received = true;

                        if (std::holds_alternative<ElicitRequestFormParams>(params)) {
                            auto form_params = std::get<ElicitRequestFormParams>(params);
                            std::cout << "Client: Form message: " << form_params.message << "\n";
                            std::cout << "Client: Form schema: " << form_params.requestedSchema.dump()
                                      << "\n";

                            nlohmann::json form_data = {
                                {"name", "Alice Johnson"},
                                {"email", "alice@example.com"},
                                {"priority", "high"},
                                {"topics", nlohmann::json::array({"bugs", "docs"})},
                                {"age", 36},
                                {"subscribe", true}};

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
                    auto result = co_await client.call_tool("request_user_info", call_args);
                    std::cout << "Client: Tool completed with " << result.content.size()
                              << " content blocks\n";
                    for (size_t i = 0; i < result.content.size(); ++i) {
                        if (auto* text = std::get_if<mcp::TextContent>(&result.content[i])) {
                            std::cout << "Client: Content block " << i << ": " << text->text << "\n";
                        }
                    }

                    if (!elicitation_received) {
                        throw std::runtime_error("the elicitation handler was not called");
                    }
                    std::cout << "Client: Successfully handled elicitation request\n";

                    // In a modern session the answer came with a retry of the tools/call.
                    const auto retries = server.mrtr_stats().resume_hit;
                    std::cout << "Server: Retries that resumed the tool: " << retries << "\n";
                    if (modern_session && retries != 1) {
                        throw std::runtime_error("expected the tool to resume once");
                    }

                    std::cout << "Client: Closing connection\n";
                    client.close();
                    succeeded = true;
                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
                io_ctx.stop();
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

        if (!succeeded) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
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
