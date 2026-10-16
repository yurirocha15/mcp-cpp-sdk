// Project fixture for the official MCP conformance runner's 2025-11-25 baseline.

#include <mcp/server/server.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view g_test_image_base64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
constexpr std::string_view g_test_audio_base64 =
    "UklGRiYAAABXQVZFZm10IBAAAAABAAEAQB8AAAB9AAACABAAZGF0YQIAAAA=";

nlohmann::json empty_object_schema() {
    return {{"type", "object"}, {"properties", nlohmann::json::object()}};
}

mcp::TextContent text_content(std::string text) {
    mcp::TextContent content;
    content.text = std::move(text);
    return content;
}

mcp::ImageContent image_content() {
    mcp::ImageContent content;
    content.data = g_test_image_base64;
    content.mimeType = "image/png";
    return content;
}

mcp::AudioContent audio_content() {
    mcp::AudioContent content;
    content.data = g_test_audio_base64;
    content.mimeType = "audio/wav";
    return content;
}

mcp::EmbeddedResource embedded_text(std::string uri, std::string mime_type, std::string text) {
    mcp::TextResourceContents resource;
    resource.uri = std::move(uri);
    resource.mimeType = std::move(mime_type);
    resource.text = std::move(text);

    mcp::EmbeddedResource content;
    content.resource = std::move(resource);
    return content;
}

mcp::PromptMessage prompt_text(std::string text) {
    mcp::PromptMessage message;
    message.role = mcp::Role::eUser;
    message.content = text_content(std::move(text));
    return message;
}

unsigned short parse_port(int argc, char** argv) {
    constexpr unsigned short default_port = 3001;
    if (argc < 2) {
        return default_port;
    }

    unsigned int value = 0;
    const std::string_view argument(argv[1]);
    const auto [end, error] =
        std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (error != std::errc{} || end != argument.data() + argument.size() || value == 0 ||
        value > 65535) {
        throw std::invalid_argument("port must be an integer in the range 1..65535");
    }
    return static_cast<unsigned short>(value);
}

void register_content_tools(mcp::Server& server) {
    const auto empty_schema = empty_object_schema();

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_simple_text", "Return text content", empty_schema, [](const nlohmann::json&) {
            return mcp::make_tool_text_result("This is a simple text response for testing.");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_image_content", "Return image content", empty_schema, [](const nlohmann::json&) {
            mcp::CallToolResult result;
            result.content.emplace_back(image_content());
            return result;
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_audio_content", "Return audio content", empty_schema, [](const nlohmann::json&) {
            mcp::CallToolResult result;
            result.content.emplace_back(audio_content());
            return result;
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_embedded_resource", "Return an embedded resource", empty_schema,
        [](const nlohmann::json&) {
            mcp::CallToolResult result;
            result.content.emplace_back(embedded_text("test://embedded-resource", "text/plain",
                                                      "This is an embedded resource content."));
            return result;
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_multiple_content_types", "Return mixed content", empty_schema, [](const nlohmann::json&) {
            mcp::CallToolResult result;
            result.content.emplace_back(text_content("Multiple content types test:"));
            result.content.emplace_back(image_content());
            result.content.emplace_back(embedded_text(
                "test://mixed-content-resource", "application/json", R"({"test":"data","value":123})"));
            return result;
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_error_handling", "Return a tool-level error", empty_schema, [](const nlohmann::json&) {
            return mcp::make_tool_error_result("This tool intentionally returns an error for testing");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_reconnection", "Exercise SSE reconnection", empty_schema, [](const nlohmann::json&) {
            return mcp::make_tool_text_result(
                "The baseline transport returned the result on the initial stream");
        });
}

void register_context_tools(mcp::Server& server) {
    const auto empty_schema = empty_object_schema();

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_tool_with_logging", "Emit three log messages", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            co_await context.log_info("Tool execution started");
            co_await context.log_info("Tool processing data");
            co_await context.log_info("Tool execution completed");
            co_return mcp::make_tool_text_result("Tool with logging executed successfully");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_tool_with_progress", "Emit progress notifications", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            co_await context.report_progress(0, 100, "Completed step 0 of 100");
            co_await context.report_progress(50, 100, "Completed step 50 of 100");
            co_await context.report_progress(100, 100, "Completed step 100 of 100");
            co_return mcp::make_tool_text_result("Progress complete");
        });

    const nlohmann::json sampling_schema = {{"type", "object"},
                                            {"properties", {{"prompt", {{"type", "string"}}}}},
                                            {"required", nlohmann::json::array({"prompt"})}};
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_sampling", "Request sampling from the client", sampling_schema,
        [](mcp::Context& context, nlohmann::json arguments) -> mcp::Task<mcp::CallToolResult> {
            mcp::SamplingMessage message;
            message.role = mcp::Role::eUser;
            message.content = mcp::SamplingMessageContentBlock{
                text_content(arguments.value("prompt", "Test prompt for sampling"))};

            mcp::CreateMessageRequestParams request;
            request.messages.emplace_back(std::move(message));
            request.maxTokens = 100;
            (void)co_await context.sample_llm(request);
            co_return mcp::make_tool_text_result("LLM response received");
        });

    const nlohmann::json elicitation_schema = {{"type", "object"},
                                               {"properties", {{"message", {{"type", "string"}}}}},
                                               {"required", nlohmann::json::array({"message"})}};
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_elicitation", "Request form input from the client", elicitation_schema,
        [](mcp::Context& context, nlohmann::json arguments) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestFormParams request;
            request.message = arguments.value("message", "Please provide your information");
            request.requestedSchema = {
                {"type", "object"},
                {"properties",
                 {{"username", {{"type", "string"}, {"description", "User's response"}}},
                  {"email", {{"type", "string"}, {"description", "User's email address"}}}}},
                {"required", nlohmann::json::array({"username", "email"})}};
            (void)co_await context.elicit(mcp::ElicitRequestParams{std::move(request)});
            co_return mcp::make_tool_text_result("User response received");
        });
}

nlohmann::json defaults_schema() {
    return {{"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"default", "John Doe"}}},
              {"age", {{"type", "integer"}, {"default", 30}}},
              {"score", {{"type", "number"}, {"default", 95.5}}},
              {"status",
               {{"type", "string"},
                {"enum", nlohmann::json::array({"active", "inactive", "pending"})},
                {"default", "active"}}},
              {"verified", {{"type", "boolean"}, {"default", true}}}}},
            {"required", nlohmann::json::array()}};
}

nlohmann::json enums_schema() {
    const auto titled = nlohmann::json::array({{{"const", "value1"}, {"title", "First Option"}},
                                               {{"const", "value2"}, {"title", "Second Option"}}});
    return {
        {"type", "object"},
        {"properties",
         {{"untitledSingle",
           {{"type", "string"}, {"enum", nlohmann::json::array({"option1", "option2", "option3"})}}},
          {"titledSingle", {{"type", "string"}, {"oneOf", titled}}},
          {"legacyEnum",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"opt1", "opt2", "opt3"})},
            {"enumNames", nlohmann::json::array({"Option One", "Option Two", "Option Three"})}}},
          {"untitledMulti",
           {{"type", "array"},
            {"items",
             {{"type", "string"},
              {"enum", nlohmann::json::array({"option1", "option2", "option3"})}}}}},
          {"titledMulti", {{"type", "array"}, {"items", {{"anyOf", titled}}}}}}},
        {"required", nlohmann::json::array()}};
}

void register_elicitation_schema_tool(mcp::Server& server, std::string name,
                                      nlohmann::json requested_schema) {
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        name, "Exercise elicitation schema support", empty_object_schema(),
        [schema = std::move(requested_schema)](mcp::Context& context,
                                               nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestFormParams request;
            request.message = "Conformance elicitation schema test";
            request.requestedSchema = schema;
            (void)co_await context.elicit(mcp::ElicitRequestParams{std::move(request)});
            co_return mcp::make_tool_text_result("Elicitation completed");
        });
}

void register_schema_tools(mcp::Server& server) {
    register_elicitation_schema_tool(server, "test_elicitation_sep1034_defaults", defaults_schema());
    register_elicitation_schema_tool(server, "test_elicitation_sep1330_enums", enums_schema());

    const nlohmann::json schema_2020_12 = {
        {"$schema", "https://json-schema.org/draft/2020-12/schema"},
        {"type", "object"},
        {"$defs",
         {{"address", {{"type", "object"}, {"properties", {{"street", {{"type", "string"}}}}}}}}},
        {"properties", {{"address", {{"$ref", "#/$defs/address"}}}}},
        {"additionalProperties", false}};
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "json_schema_2020_12_tool", "Tool with JSON Schema 2020-12 features", schema_2020_12,
        [](const nlohmann::json&) { return mcp::make_tool_text_result("Schema preserved"); });
}

void register_resources(mcp::Server& server) {
    mcp::Resource text_resource;
    text_resource.uri = "test://static-text";
    text_resource.name = "Static text resource";
    text_resource.description = "Conformance text resource";
    text_resource.mimeType = "text/plain";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        text_resource, [](mcp::ReadResourceRequestParams request) {
            mcp::ReadResourceResult result;
            mcp::TextResourceContents contents;
            contents.uri = std::move(request.uri);
            contents.mimeType = "text/plain";
            contents.text = "This is the content of the static text resource.";
            result.contents.emplace_back(std::move(contents));
            return result;
        });

    mcp::Resource binary_resource;
    binary_resource.uri = "test://static-binary";
    binary_resource.name = "Static binary resource";
    binary_resource.description = "Conformance binary resource";
    binary_resource.mimeType = "image/png";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        binary_resource, [](mcp::ReadResourceRequestParams request) {
            mcp::ReadResourceResult result;
            mcp::BlobResourceContents contents;
            contents.uri = std::move(request.uri);
            contents.mimeType = "image/png";
            contents.blob = g_test_image_base64;
            result.contents.emplace_back(std::move(contents));
            return result;
        });

    mcp::Resource watched_resource;
    watched_resource.uri = "test://watched-resource";
    watched_resource.name = "Watched resource";
    watched_resource.description = "Resource used by subscription scenarios";
    watched_resource.mimeType = "text/plain";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        watched_resource, [](mcp::ReadResourceRequestParams request) {
            mcp::ReadResourceResult result;
            mcp::TextResourceContents contents;
            contents.uri = std::move(request.uri);
            contents.mimeType = "text/plain";
            contents.text = "Watched resource content";
            result.contents.emplace_back(std::move(contents));
            return result;
        });

    mcp::ResourceTemplate resource_template;
    resource_template.uriTemplate = "test://template/{id}/data";
    resource_template.name = "Parameterized test resource";
    resource_template.description = "Substitutes the id URI segment";
    resource_template.mimeType = "application/json";
    server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        resource_template, [](mcp::ReadResourceRequestParams request) {
            mcp::ReadResourceResult result;
            mcp::TextResourceContents contents;
            constexpr std::string_view prefix = "test://template/";
            constexpr std::string_view suffix = "/data";
            const std::string uri = std::move(request.uri);
            if (!uri.starts_with(prefix) || !uri.ends_with(suffix) ||
                uri.size() <= prefix.size() + suffix.size()) {
                throw std::invalid_argument("unexpected resource-template URI");
            }
            const auto id = uri.substr(prefix.size(), uri.size() - prefix.size() - suffix.size());
            contents.uri = uri;
            contents.mimeType = "application/json";
            contents.text =
                nlohmann::json{{"id", id}, {"templateTest", true}, {"data", "Data for ID: " + id}}
                    .dump();
            result.contents.emplace_back(std::move(contents));
            return result;
        });
}

void register_prompts(mcp::Server& server) {
    mcp::Prompt simple;
    simple.name = "test_simple_prompt";
    simple.description = "A simple conformance prompt";
    server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        simple, [](mcp::GetPromptRequestParams) {
            mcp::GetPromptResult result;
            result.messages.emplace_back(prompt_text("This is a simple prompt for testing."));
            return result;
        });

    mcp::Prompt with_arguments;
    with_arguments.name = "test_prompt_with_arguments";
    with_arguments.description = "A parameterized conformance prompt";
    with_arguments.arguments =
        std::vector<mcp::PromptArgument>{{"arg1", "First test argument", true, std::nullopt},
                                         {"arg2", "Second test argument", true, std::nullopt}};
    server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        with_arguments, [](mcp::GetPromptRequestParams request) {
            const auto arguments = request.arguments.value_or(std::map<std::string, std::string>{});
            const auto arg1 = arguments.contains("arg1") ? arguments.at("arg1") : "";
            const auto arg2 = arguments.contains("arg2") ? arguments.at("arg2") : "";
            mcp::GetPromptResult result;
            result.messages.emplace_back(
                prompt_text("Prompt with arguments: arg1='" + arg1 + "', arg2='" + arg2 + "'"));
            return result;
        });

    mcp::Prompt with_resource;
    with_resource.name = "test_prompt_with_embedded_resource";
    with_resource.description = "A prompt containing an embedded resource";
    with_resource.arguments =
        std::vector<mcp::PromptArgument>{{"resourceUri", "URI to embed", true, std::nullopt}};
    server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        with_resource, [](mcp::GetPromptRequestParams request) {
            const auto arguments = request.arguments.value_or(std::map<std::string, std::string>{});
            const auto uri =
                arguments.contains("resourceUri") ? arguments.at("resourceUri") : "test://resource";
            mcp::GetPromptResult result;
            mcp::PromptMessage resource_message;
            resource_message.role = mcp::Role::eUser;
            resource_message.content =
                embedded_text(uri, "text/plain", "Embedded resource content for testing.");
            result.messages.emplace_back(std::move(resource_message));
            result.messages.emplace_back(prompt_text("Please process the embedded resource above."));
            return result;
        });

    mcp::Prompt with_image;
    with_image.name = "test_prompt_with_image";
    with_image.description = "A prompt containing image content";
    server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        with_image, [](mcp::GetPromptRequestParams) {
            mcp::GetPromptResult result;
            mcp::PromptMessage image_message;
            image_message.role = mcp::Role::eUser;
            image_message.content = image_content();
            result.messages.emplace_back(std::move(image_message));
            result.messages.emplace_back(prompt_text("Please analyze the image above."));
            return result;
        });
}

void register_completion(mcp::Server& server) {
    server.set_completion_provider(
        [](const mcp::CompleteParams& params) -> mcp::Task<mcp::CompleteResult> {
            mcp::CompleteResult result;
            result.completion.values = {params.argument.value + "-completion"};
            result.completion.total = 1;
            result.completion.hasMore = false;
            co_return result;
        });
}

void configure_server(mcp::Server& server) {
    register_content_tools(server);
    register_context_tools(server);
    register_schema_tools(server);
    register_resources(server);
    register_prompts(server);
    register_completion(server);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto port = parse_port(argc, argv);

        mcp::Implementation implementation;
        implementation.name = "mcp-cpp-sdk-conformance-server";
        implementation.version = "0.2.0";

        mcp::ServerCapabilities capabilities;
        capabilities.tools = mcp::ServerCapabilities::ToolsCapability{true};
        capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{true, true};
        capabilities.prompts = mcp::ServerCapabilities::PromptsCapability{true};
        capabilities.logging = nlohmann::json::object();
        capabilities.completions = nlohmann::json::object();

        auto server_factory = [implementation, capabilities](const boost::asio::any_io_executor&) {
            auto server = std::make_unique<mcp::Server>(implementation, capabilities);
            configure_server(*server);
            return server;
        };

        boost::asio::io_context io_context;
        mcp::StreamableHttpSessionManager manager(io_context.get_executor(), "127.0.0.1", port,
                                                  std::move(server_factory));
        manager.set_allowed_origins(
            {"http://127.0.0.1:" + std::to_string(port), "http://localhost:" + std::to_string(port)});

        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&manager](const boost::system::error_code& error, int) {
            if (!error) {
                manager.close();
            }
        });

        std::exception_ptr listen_error;
        boost::asio::co_spawn(io_context, manager.listen(),
                              [&listen_error, &signals](std::exception_ptr error) {
                                  listen_error = std::move(error);
                                  signals.cancel();
                              });
        std::cerr << "MCP conformance server listening on http://127.0.0.1:" << port << "/mcp\n";
        io_context.run();
        if (listen_error) {
            std::rethrow_exception(listen_error);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Conformance server failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
