// Project fixture for the official MCP conformance runner: the 2025-11-25 baseline and the diagnostic
// tools that the 2026-07-28 scenarios call.

#include <mcp/server/server.hpp>
#include <mcp/server/skills.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <charconv>
#include <chrono>
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

constexpr std::string_view g_TEST_IMAGE_BASE64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
constexpr std::string_view g_TEST_AUDIO_BASE64 =
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
    content.data = g_TEST_IMAGE_BASE64;
    content.mimeType = "image/png";
    return content;
}

mcp::AudioContent audio_content() {
    mcp::AudioContent content;
    content.data = g_TEST_AUDIO_BASE64;
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

    // The keywords SEP-2106 allows next to the root `type`: composition, conditional and $anchor.
    const auto schema_2020_12 = nlohmann::json::parse(R"json({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "$defs": {
            "address": {
                "$anchor": "addressDef",
                "type": "object",
                "properties": {"street": {"type": "string"}, "city": {"type": "string"}}
            }
        },
        "properties": {
            "name": {"type": "string"},
            "address": {"$ref": "#/$defs/address"},
            "contactMethod": {"type": "string", "enum": ["phone", "email"]},
            "phone": {"type": "string"},
            "email": {"type": "string"}
        },
        "allOf": [{"anyOf": [{"required": ["phone"]}, {"required": ["email"]}]}],
        "if": {"properties": {"contactMethod": {"const": "phone"}}, "required": ["contactMethod"]},
        "then": {"required": ["phone"]},
        "else": {"required": ["email"]},
        "additionalProperties": false
    })json");
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "json_schema_2020_12_tool", "Tool with JSON Schema 2020-12 features", schema_2020_12,
        [](const nlohmann::json&) { return mcp::make_tool_text_result("Schema preserved"); });
}

// --- Tools the 2026-07-28 scenarios call ------------------------------------------------------
//
// Under 2026-07-28 a handler's question (Context::elicit(), sample_llm(), request_roots()) reaches the
// client in an input_required result and the handler resumes when the client retries, so these tools
// are ordinary handlers.

nlohmann::json single_string_schema(std::string_view property) {
    return {{"type", "object"},
            {"properties", {{std::string(property), {{"type", "string"}}}}},
            {"required", nlohmann::json::array({std::string(property)})}};
}

mcp::ElicitRequestParams form_request(std::string message, std::string_view property) {
    mcp::ElicitRequestFormParams request;
    request.message = std::move(message);
    request.requestedSchema = single_string_schema(property);
    return mcp::ElicitRequestParams{std::move(request)};
}

mcp::CreateMessageRequestParams sampling_request(std::string text, int max_tokens) {
    mcp::SamplingMessage message;
    message.role = mcp::Role::eUser;
    message.content = mcp::SamplingMessageContentBlock{text_content(std::move(text))};

    mcp::CreateMessageRequestParams request;
    request.messages.emplace_back(std::move(message));
    request.maxTokens = max_tokens;
    return request;
}

/// The string the client answered for `property`, or an empty string.
std::string answered_text(const mcp::ElicitResult& answer, std::string_view property) {
    if (!answer.content) {
        return {};
    }
    const auto found = answer.content->find(std::string(property));
    return found != answer.content->end() && found->is_string() ? found->get<std::string>()
                                                                : std::string();
}

void register_input_required_tools(mcp::Server& server) {
    const auto empty_schema = empty_object_schema();

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_input_required_result_elicitation", "Ask one elicitation question", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            const auto question = form_request("What is your name?", "name");
            mcp::InputOptions options;
            options.key = "user_name";
            const auto answer = co_await context.elicit(question, options);
            co_return mcp::make_tool_text_result("Hello, " + answered_text(answer, "name") + "!");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_input_required_result_sampling", "Ask one sampling question", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            const auto question = sampling_request("What is the capital of France?", 100);
            (void)co_await context.sample_llm(question);
            co_return mcp::make_tool_text_result("Sampling response received");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_input_required_result_list_roots", "Ask for the client's roots", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            const auto roots = co_await context.request_roots();
            co_return mcp::make_tool_text_result("Received " + std::to_string(roots.roots.size()) +
                                                 " root(s)");
        });

    for (const char* name :
         {"test_input_required_result_request_state", "test_input_required_result_tampered_state"}) {
        server.add_tool<nlohmann::json, mcp::CallToolResult>(
            name, "Ask a question that carries requestState", empty_schema,
            [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
                const auto question = form_request("Please confirm", "ok");
                (void)co_await context.elicit(question);
                co_return mcp::make_tool_text_result("state-ok");
            });
    }

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_input_required_result_multiple_inputs",
        "Ask an elicitation, a sampling and a roots question together", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            using namespace boost::asio::experimental::awaitable_operators;
            const auto elicitation = form_request("What is your name?", "name");
            const auto sampling = sampling_request("Generate a greeting", 50);
            auto ask_name = context.elicit(elicitation);
            auto ask_greeting = context.sample_llm(sampling);
            auto ask_roots = context.request_roots();
            (void)co_await (std::move(ask_name) && std::move(ask_greeting) && std::move(ask_roots));
            co_return mcp::make_tool_text_result("All inputs received");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_input_required_result_multi_round", "Ask two questions one after the other", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            const auto first = form_request("Step 1: What is your name?", "name");
            const auto name = co_await context.elicit(first);
            const auto second = form_request("Step 2: What is your favorite color?", "color");
            const auto color = co_await context.elicit(second);
            co_return mcp::make_tool_text_result(answered_text(name, "name") + " likes " +
                                                 answered_text(color, "color"));
        });

    // Asks only what the request's declared client capabilities can answer.
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_input_required_result_capabilities", "Ask what the client declared it can answer",
        empty_schema, [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            const auto* meta = context.request_meta();
            if (meta != nullptr && meta->clientCapabilities.sampling) {
                const auto question = sampling_request("What is the capital of France?", 100);
                (void)co_await context.sample_llm(question);
            } else if (meta != nullptr && meta->clientCapabilities.elicitation) {
                const auto question = form_request("What is your name?", "name");
                (void)co_await context.elicit(question);
            } else if (meta != nullptr && meta->clientCapabilities.roots) {
                (void)co_await context.request_roots();
            }
            co_return mcp::make_tool_text_result("Capabilities respected");
        });

    mcp::Prompt prompt;
    prompt.name = "test_input_required_result_prompt";
    prompt.description = "A prompt that asks for context";
    server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        prompt,
        [](mcp::Context& context, mcp::GetPromptRequestParams) -> mcp::Task<mcp::GetPromptResult> {
            const auto question = form_request("What context should the prompt use?", "context");
            const auto answer = co_await context.elicit(question);
            mcp::GetPromptResult result;
            result.messages.emplace_back(
                prompt_text("Prompt context: " + answered_text(answer, "context")));
            co_return result;
        });
}

void register_stateless_tools(mcp::Server& server) {
    const auto empty_schema = empty_object_schema();

    // Needs a capability the request does not declare, so the server answers with
    // MissingRequiredClientCapabilityError.
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_missing_capability", "Needs the sampling capability", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            const auto question = sampling_request("Needs sampling", 10);
            (void)co_await context.sample_llm(question);
            co_return mcp::make_tool_text_result("Sampling answered");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_streaming_elicitation", "Asks one elicitation question", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            const auto question = form_request("What is your name?", "name");
            (void)co_await context.elicit(question);
            co_return mcp::make_tool_text_result("Elicitation answered");
        });

    // Nothing is sent unless the request sets a log level.
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_logging_tool", "Logs without asking for a level", empty_schema,
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            co_await context.log_info("Tool execution started");
            co_await context.log(mcp::LoggingLevel::eError, "Tool reports an error level message");
            co_return mcp::make_tool_text_result("Logging complete");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_trigger_tool_change", "Announces a change of the tool list", empty_schema,
        [&server](nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            co_await server.notify_tools_list_changed();
            co_return mcp::make_tool_text_result("Tool list change announced");
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_trigger_prompt_change", "Announces a change of the prompt list", empty_schema,
        [&server](nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            co_await server.notify_prompts_list_changed();
            co_return mcp::make_tool_text_result("Prompt list change announced");
        });

    // Parameters mirrored into the Mcp-Param-Region and Mcp-Param-Priority headers.
    const nlohmann::json header_schema = {
        {"type", "object"},
        {"properties",
         {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}},
          {"priority", {{"type", "integer"}, {"x-mcp-header", "Priority"}}}}},
        {"required", nlohmann::json::array({"region"})}};
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_custom_headers", "Mirrors parameters into headers", header_schema,
        [](const nlohmann::json& arguments) {
            return mcp::make_tool_text_result("Region: " + arguments.value("region", std::string()));
        });
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
            contents.blob = g_TEST_IMAGE_BASE64;
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

/// Waits up to `seconds`, in slices, and returns early when the task is cancelled.
mcp::Task<void> sleep_unless_cancelled(mcp::Context& context, double seconds) {
    using namespace std::chrono;
    const auto deadline =
        steady_clock::now() + duration_cast<steady_clock::duration>(duration<double>(seconds));
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    while (!context.is_cancelled() && steady_clock::now() < deadline) {
        timer.expires_after(milliseconds(50));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
}

// --- Tools the tasks scenarios call -------------------------------------------------------------
//
// Under 2026-07-28 a client that declares the tasks extension gets a task handle for most of these
// tools; the others, and every other client, get the result in the call.

void register_task_tools(mcp::Server& server) {
    const nlohmann::json name_schema = {{"type", "object"},
                                        {"properties", {{"name", {{"type", "string"}}}}}};
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "greet", "Greet by name; never a task", name_schema, [](const nlohmann::json& arguments) {
            const auto found = arguments.find("name");
            const std::string name =
                found != arguments.end() && found->is_string() ? found->get<std::string>() : "World";
            return mcp::make_tool_text_result("Hello, " + name + "!");
        });

    const nlohmann::json compute_schema = {
        {"type", "object"},
        {"properties", {{"seconds", {{"type", "number"}}}, {"label", {{"type", "string"}}}}}};
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "slow_compute", "Sleep for the given seconds, then return", compute_schema,
        [](mcp::Context& context, nlohmann::json arguments) -> mcp::Task<mcp::CallToolResult> {
            const auto seconds = arguments.value("seconds", 0.0);
            const auto label = arguments.value("label", std::string());
            co_await sleep_unless_cancelled(context, seconds);
            co_return mcp::make_tool_text_result("slept " + std::to_string(seconds) + "s " + label);
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "failing_job", "Run for about a second, then report a tool error", empty_object_schema(),
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            co_await sleep_unless_cancelled(context, 1.0);
            co_return mcp::make_tool_error_result("the job failed");
        });

    // The result does not fit the output schema, which fails the task with a JSON-RPC error.
    const nlohmann::json protocol_error_output = {{"type", "object"},
                                                  {"properties", {{"value", {{"type", "integer"}}}}},
                                                  {"required", nlohmann::json::array({"value"})}};
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "protocol_error_job", "Return a result that breaks the output schema", empty_object_schema(),
        protocol_error_output, [](const nlohmann::json&) {
            mcp::CallToolResult result = mcp::make_tool_text_result("not an integer");
            result.structuredContent = nlohmann::json{{"value", "not an integer"}};
            return result;
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "confirm_delete", "Ask the user to confirm a deletion", single_string_schema("filename"),
        [](mcp::Context& context, nlohmann::json arguments) -> mcp::Task<mcp::CallToolResult> {
            const auto filename = arguments.value("filename", std::string());
            const auto question = form_request("Delete " + filename + "?", "confirm");
            mcp::InputOptions options;
            options.validate_content = false;
            (void)co_await context.elicit(question, options);
            co_return mcp::make_tool_text_result("Deleted " + filename);
        });

    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "multi_input", "Ask two questions at once", empty_object_schema(),
        [](mcp::Context& context, nlohmann::json) -> mcp::Task<mcp::CallToolResult> {
            using namespace boost::asio::experimental::awaitable_operators;
            const auto first = form_request("First answer?", "first");
            const auto second = form_request("Second answer?", "second");
            mcp::InputOptions first_options;
            first_options.key = "first";
            first_options.validate_content = false;
            mcp::InputOptions second_options;
            second_options.key = "second";
            second_options.validate_content = false;
            auto ask_first = context.elicit(first, first_options);
            auto ask_second = context.elicit(second, second_options);
            (void)co_await (std::move(ask_first) && std::move(ask_second));
            co_return mcp::make_tool_text_result("Both answered");
        });

    // Asks for a name in the multi round-trip flow of the call, then greets it from the task.
    server.add_tool<nlohmann::json, mcp::CallToolResult>(
        "test_tool_with_task", "Ask for a name, then greet it from a task", empty_object_schema(),
        [](const nlohmann::json& arguments) {
            return mcp::make_tool_text_result("Hello, " +
                                              arguments.value("user_name", std::string("World")) + "!");
        });
    server.set_tool_task_preparation(
        "test_tool_with_task",
        [](mcp::Context& context, const nlohmann::json& arguments) -> mcp::Task<mcp::TaskPreparation> {
            const auto question = form_request("What is your name?", "name");
            mcp::InputOptions options;
            options.key = "user_name";
            const auto answer = co_await context.elicit(question, options);
            mcp::TaskPreparation prepared;
            prepared.arguments = arguments;
            prepared.arguments["user_name"] = answered_text(answer, "name");
            co_return prepared;
        });

    mcp::TaskOptions options;
    options.ttl = std::chrono::minutes(10);
    options.poll_interval = std::chrono::milliseconds(200);
    server.enable_tasks(options);
    server.set_tool_task_mode("slow_compute", mcp::ToolTaskMode::eAuto);
    server.set_tool_task_mode("failing_job", mcp::ToolTaskMode::eRequired);
    server.set_tool_task_mode("protocol_error_job", mcp::ToolTaskMode::eAuto);
    server.set_tool_task_mode("confirm_delete", mcp::ToolTaskMode::eAuto);
    server.set_tool_task_mode("multi_input", mcp::ToolTaskMode::eAuto);
    server.set_tool_task_mode("test_tool_with_task", mcp::ToolTaskMode::eRequired);
}

/// Serves a few skills of the Skills extension, for the alpha runner's skills scenarios. Off unless
/// MCP_CONFORMANCE_SKILLS is set, so the pinned suites see the server they always have.
void register_skills(mcp::Server& server) {
    const auto* enabled = std::getenv("MCP_CONFORMANCE_SKILLS");
    if (enabled == nullptr || *enabled == '\0') {
        return;
    }
    mcp::skills::SkillsOptions options;
    options.page_size = 2;
    // The registered handlers hold the skills, so this object can go out of scope.
    mcp::skills::SkillsExtension skills(server, options);

    mcp::skills::SkillDefinition git;
    git.path = "git-workflow";
    git.frontmatter = {{"name", "git-workflow"},
                       {"description", "Follow this team's Git conventions for branching and commits"}};
    git.files.push_back({"SKILL.md",
                         "---\nname: git-workflow\ndescription: Follow this team's Git conventions for "
                         "branching and commits\n---\n\nUse short-lived branches.\n",
                         std::nullopt,
                         {}});
    git.files.push_back({"references/branching.md", "# Branching\n", std::nullopt, {}});
    skills.add_skill(std::move(git));

    mcp::skills::SkillDefinition refunds;
    refunds.path = "acme/billing/refunds";
    refunds.frontmatter = {{"name", "refunds"},
                           {"description", "Process customer refund requests per company policy"},
                           {"license", "Apache-2.0"}};
    refunds.files.push_back({"SKILL.md",
                             "---\nname: refunds\ndescription: Process customer refund requests per "
                             "company policy\nlicense: Apache-2.0\n---\n\nSee examples/email.md.\n",
                             std::nullopt,
                             {}});
    refunds.files.push_back({"examples/email.md", "Dear customer,\n", std::nullopt, {}});
    refunds.files.push_back({"templates/regional/uk.md", "# UK refunds\n", std::nullopt, {}});
    skills.add_skill(std::move(refunds));

    mcp::skills::SkillDefinition pdf;
    pdf.path = "pdf-processing";
    pdf.frontmatter = {{"name", "pdf-processing"},
                       {"description", "Extract, fill, and assemble PDF documents"}};
    pdf.files.push_back({"SKILL.md",
                         "---\nname: pdf-processing\ndescription: Extract, fill, and assemble PDF "
                         "documents\n---\n\nUse the forms reference.\n",
                         std::nullopt,
                         {}});
    pdf.files.push_back({"references/FORMS.md", "# Forms\n", std::nullopt, {}});
    skills.add_skill(std::move(pdf));
}

void configure_server(mcp::Server& server, const boost::asio::any_io_executor& handler_executor) {
    server.set_handler_executor(handler_executor);
    register_skills(server);
    register_content_tools(server);
    register_context_tools(server);
    register_schema_tools(server);
    register_input_required_tools(server);
    register_task_tools(server);
    register_stateless_tools(server);
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

        // Task handlers run on this pool, apart from the connection's executor.
        boost::asio::thread_pool handler_pool(4);
        auto server_factory = [implementation, capabilities,
                               &handler_pool](const boost::asio::any_io_executor&) {
            auto server = std::make_unique<mcp::Server>(implementation, capabilities);
            configure_server(*server, handler_pool.get_executor());
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
