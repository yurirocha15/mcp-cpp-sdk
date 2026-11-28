/// @file rich_content.cpp
/// @brief Demonstrates the content a server returns besides text: images, audio and resources.
///
/// This example shows:
/// - A tool result that holds text, image, audio and embedded resource blocks
/// - A binary resource, read as a BlobResourceContents with its base64 data
/// - A prompt whose messages hold text, an embedded resource and an image
/// - A client that reads each block by its type
/// - Loopback communication via MemoryTransport

#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

namespace asio = boost::asio;

namespace {

/// A 1x1 PNG image, base64-encoded.
constexpr const char* g_PNG_BASE64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

/// A WAV clip of eight samples, base64-encoded.
constexpr const char* g_WAV_BASE64 =
    "UklGRjQAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YRAAAAAAAEAfgD5AHwAAwOCAwcDg";

constexpr const char* g_LOGO_URI = "mcp://example/logo.png";
constexpr const char* g_NOTES_URI = "mcp://example/notes.txt";

/// Throws when a check of the example fails.
void expect(bool condition, const std::string& what) {
    std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << '\n';
    if (!condition) {
        throw std::runtime_error(what);
    }
}

void register_content(mcp::Server& server) {
    using namespace mcp;

    // docs-begin: rich-tool
    // A tool result is a list of content blocks, and each block has its own type.
    server.add_tool<nlohmann::json, CallToolResult>(
        "report", "Returns text, an image, audio and an embedded resource",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](nlohmann::json) -> CallToolResult {
            CallToolResult result;
            result.content.emplace_back(TextContent{.text = "Report for today"});
            result.content.emplace_back(ImageContent{.data = g_PNG_BASE64, .mimeType = "image/png"});
            result.content.emplace_back(AudioContent{.data = g_WAV_BASE64, .mimeType = "audio/wav"});

            EmbeddedResource notes;
            notes.resource = TextResourceContents{
                .uri = g_NOTES_URI, .text = "All systems nominal.", .mimeType = "text/plain"};
            result.content.emplace_back(std::move(notes));
            return result;
        });
    // docs-end: rich-tool

    // docs-begin: binary-resource
    // The bytes of a binary resource are sent base64-encoded in a BlobResourceContents.
    Resource logo;
    logo.uri = g_LOGO_URI;
    logo.name = "logo";
    logo.mimeType = "image/png";
    server.add_resource<ReadResourceRequestParams, ReadResourceResult>(
        logo, [](ReadResourceRequestParams request) -> ReadResourceResult {
            ReadResourceResult result;
            result.contents.emplace_back(BlobResourceContents{
                .uri = std::move(request.uri), .blob = g_PNG_BASE64, .mimeType = "image/png"});
            return result;
        });
    // docs-end: binary-resource

    // docs-begin: rich-prompt
    // A prompt message holds one content block of any type, including an embedded resource and
    // an image.
    Prompt prompt;
    prompt.name = "diagnose";
    prompt.description = "Asks for a diagnosis of a screenshot and the notes that go with it";
    server.add_prompt<GetPromptRequestParams, GetPromptResult>(
        prompt, [](GetPromptRequestParams) -> GetPromptResult {
            GetPromptResult result;
            result.messages.push_back(
                {Role::eUser, TextContent{.text = "What is wrong in this screenshot?"}});

            EmbeddedResource notes;
            notes.resource = TextResourceContents{
                .uri = g_NOTES_URI, .text = "All systems nominal.", .mimeType = "text/plain"};
            result.messages.push_back({Role::eUser, std::move(notes)});

            result.messages.push_back(
                {Role::eUser, ImageContent{.data = g_PNG_BASE64, .mimeType = "image/png"}});
            return result;
        });
    // docs-end: rich-prompt
}

mcp::Task<void> use_content(mcp::Client& client) {
    using namespace mcp;

    // docs-begin: rich-tool-client
    // Each block is one alternative of the ContentBlock variant.
    const auto report = co_await client.call_tool("report", nlohmann::json::object());
    expect(report.content.size() == 4, "the tool result has four blocks");
    expect(std::holds_alternative<TextContent>(report.content[0]), "block 0 is text");
    if (const auto* image = std::get_if<ImageContent>(&report.content[1])) {
        std::cout << "Client: image of type " << image->mimeType << ", " << image->data.size()
                  << " base64 characters\n";
    }
    if (const auto* audio = std::get_if<AudioContent>(&report.content[2])) {
        std::cout << "Client: audio of type " << audio->mimeType << ", " << audio->data.size()
                  << " base64 characters\n";
    }
    if (const auto* embedded = std::get_if<EmbeddedResource>(&report.content[3])) {
        if (const auto* text = std::get_if<TextResourceContents>(&embedded->resource)) {
            std::cout << "Client: embedded resource " << text->uri << ": " << text->text << '\n';
        }
    }
    // docs-end: rich-tool-client
    expect(std::holds_alternative<ImageContent>(report.content[1]), "block 1 is an image");
    expect(std::holds_alternative<AudioContent>(report.content[2]), "block 2 is audio");
    expect(std::holds_alternative<EmbeddedResource>(report.content[3]),
           "block 3 is an embedded resource");

    // docs-begin: binary-resource-client
    const auto logo = co_await client.read_resource(g_LOGO_URI);
    const auto* blob = std::get_if<BlobResourceContents>(&logo.contents.at(0));
    // docs-end: binary-resource-client
    expect(blob != nullptr && blob->blob == g_PNG_BASE64 && blob->mimeType == "image/png",
           "the logo is a blob with its base64 data");

    // docs-begin: rich-prompt-client
    const auto prompt = co_await client.get_prompt("diagnose");
    expect(prompt.messages.size() == 3, "the prompt has three messages");
    expect(std::holds_alternative<EmbeddedResource>(prompt.messages[1].content),
           "message 1 embeds a resource");
    expect(std::holds_alternative<ImageContent>(prompt.messages[2].content), "message 2 is an image");
    // docs-end: rich-prompt-client
}

}  // namespace

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;
        bool client_ok = false;

        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};
        server_caps.resources = ServerCapabilities::ResourcesCapability{};
        server_caps.prompts = ServerCapabilities::PromptsCapability{};

        Server server(Implementation{"rich-content-server", "1.0.0"}, server_caps);
        register_content(server);

        auto [client_transport, server_transport] = make_memory_transport_pair(io_ctx.get_executor());

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
                    Client client(client_transport, io_ctx.get_executor());
                    co_await client.connect(Implementation{"rich-content-client", "1.0.0"},
                                            ClientCapabilities{});
                    co_await use_content(client);
                    client.close();
                    client_ok = true;
                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
                io_ctx.stop();
            },
            asio::detached);

        // Fail rather than hang if something never answers.
        asio::steady_timer watchdog(io_ctx.get_executor());
        watchdog.expires_after(std::chrono::seconds(10));
        watchdog.async_wait([&io_ctx](const boost::system::error_code& error) {
            if (!error) {
                std::cerr << "Timed out\n";
                io_ctx.stop();
            }
        });

        io_ctx.run();

        if (!client_ok) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
        std::cout << "Example completed successfully\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
