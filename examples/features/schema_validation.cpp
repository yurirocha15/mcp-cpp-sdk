/// @file schema_validation.cpp
/// @brief Demonstrates JSON Schema validation of tools on the server and the client.
///
/// This example shows:
/// - Registration refusing a tool whose schema cannot be used, with SchemaError::category()
/// - Arguments that break the input schema answered with an isError result; the handler never runs
/// - A result whose structuredContent breaks the output schema refused by the server
/// - The client's own check of structuredContent (ClientOptions::validate_tool_schemas), shown
///   against a server whose output check is turned off with set_tool_validation()
/// - Server::tool_validation_stats()
///
/// Each step checks its outcome; the example exits with EXIT_FAILURE if any differs.

#include <mcp/client/client.hpp>
#include <mcp/protocol/schema.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/memory.hpp>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;
using nlohmann::json;

namespace {

/// Arguments: two integers. Result: their sum, as structured content.
const json g_add_input_schema = {
    {"type", "object"},
    {"properties", {{"a", {{"type", "integer"}}}, {"b", {{"type", "integer"}}}}},
    {"required", json::array({"a", "b"})}};
const json g_add_output_schema = {{"type", "object"},
                                  {"properties", {{"sum", {{"type", "integer"}}}}},
                                  {"required", json::array({"sum"})}};

std::atomic<int> g_add_handler_runs{0};

bool check(bool condition, const std::string& what) {
    std::cout << (condition ? "[ok]   " : "[FAIL] ") << what << "\n";
    return condition;
}

const char* category_name(mcp::SchemaError::Category category) {
    switch (category) {
        case mcp::SchemaError::Category::eInvalidSchema:
            return "eInvalidSchema";
        case mcp::SchemaError::Category::eUnsupportedDialect:
            return "eUnsupportedDialect";
        case mcp::SchemaError::Category::eUnresolvedRef:
            return "eUnresolvedRef";
    }
    return "unknown";
}

/// Registers a tool with @p input_schema and reports the category it is refused with.
bool expect_refused(mcp::Server& server, const std::string& name, const json& input_schema,
                    mcp::SchemaError::Category expected) {
    try {
        server.add_tool(name, "Never registered", input_schema,
                        [](const json& /*args*/) -> json { return json::object(); });
    } catch (const mcp::SchemaError& error) {
        std::cout << "       " << name << ": " << error.what() << "\n";
        return check(error.category() == expected,
                     name + " refused at registration as " + category_name(error.category()));
    }
    return check(false, name + " refused at registration");
}

/// The tools both servers offer: `add`, which sums correctly, and `broken_add`, whose result
/// breaks its own output schema.
void add_tools(mcp::Server& server) {
    server.add_tool<json, json>(
        "add", "Adds two integers", g_add_input_schema, g_add_output_schema,
        [](const json& args) -> json {
            g_add_handler_runs.fetch_add(1);
            return json{{"sum", args.at("a").get<int>() + args.at("b").get<int>()}};
        });
    server.add_tool<json, json>("broken_add", "Returns the sum as text, against its output schema",
                                g_add_input_schema, g_add_output_schema, [](const json& args) -> json {
                                    const int sum = args.at("a").get<int>() + args.at("b").get<int>();
                                    return json{{"sum", std::to_string(sum)}};
                                });
}

std::unique_ptr<mcp::Server> make_server(const std::string& name) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    return std::make_unique<mcp::Server>(mcp::Implementation{name, "1.0.0"}, caps);
}

mcp::Task<void> serve(mcp::Server* server, std::shared_ptr<mcp::ITransport> transport,
                      asio::any_io_executor executor) {
    co_await server->run(std::move(transport), std::move(executor));
}

/// Server-side checks: arguments and structured results.
mcp::Task<bool> exercise_server_checks(std::shared_ptr<mcp::ITransport> transport,
                                       asio::any_io_executor executor) {
    bool ok = true;
    mcp::Client client(std::move(transport), executor);
    co_await client.connect("schema-client", "1.0.0");
    co_await client.list_tools();

    std::cout << "\n=== Server checks ===\n";
    const json good_args = {{"a", 2}, {"b", 3}};
    auto good = co_await client.call_tool("add", good_args);
    ok &= check(good.structuredContent && (*good.structuredContent)["sum"] == 5,
                "add(2, 3) returns structuredContent {\"sum\": 5}");

    const int runs_before = g_add_handler_runs.load();
    const json bad_args = {{"a", 2}, {"b", "three"}};
    auto refused = co_await client.call_tool("add", bad_args);
    ok &= check(refused.isError.value_or(false), "add(2, \"three\") is answered with isError");
    if (!refused.content.empty()) {
        if (const auto* text = std::get_if<mcp::TextContent>(&refused.content.front())) {
            std::cout << "       violations: " << text->text << "\n";
        }
    }
    ok &= check(g_add_handler_runs.load() == runs_before, "the add handler did not run");

    try {
        co_await client.call_tool("broken_add", good_args);
        ok &= check(false, "broken_add is refused by the server");
    } catch (const mcp::McpError& error) {
        std::cout << "       server error: " << error.what() << "\n";
        ok &= check(error.code() == mcp::g_INTERNAL_ERROR,
                    "broken_add is refused by the server with g_INTERNAL_ERROR");
    }
    co_return ok;
}

/// The client's check of structured results, against a server that no longer makes its own.
mcp::Task<bool> exercise_client_check(std::shared_ptr<mcp::ITransport> transport,
                                      asio::any_io_executor executor) {
    bool ok = true;
    mcp::Client client(std::move(transport), executor);
    co_await client.connect("schema-client", "1.0.0");
    // The client checks results only for tools it has seen listed: it takes the output schema
    // from the list.
    co_await client.list_tools();

    std::cout << "\n=== Client check (server output check turned off) ===\n";
    const json args = {{"a", 2}, {"b", 3}};
    try {
        co_await client.call_tool("broken_add", args);
        ok &= check(false, "the client refuses broken_add's result");
    } catch (const mcp::StructuredContentError& error) {
        std::cout << "       client error: " << error.what() << "\n";
        ok &= check(true, "the client refuses broken_add's result with StructuredContentError");
    }
    co_return ok;
}

mcp::Task<void> run_clients(asio::io_context* io_ctx, std::shared_ptr<mcp::ITransport> checking,
                            std::shared_ptr<mcp::ITransport> lenient, bool* ok) {
    try {
        const bool server_ok =
            co_await exercise_server_checks(std::move(checking), io_ctx->get_executor());
        const bool client_ok =
            co_await exercise_client_check(std::move(lenient), io_ctx->get_executor());
        *ok = server_ok && client_ok;
    } catch (const std::exception& error) {
        std::cerr << "[Client] Unexpected error: " << error.what() << "\n";
        *ok = false;
    }
    io_ctx->stop();
}

}  // namespace

int main() {
    try {
        asio::io_context io_ctx;
        bool ok = true;

        // ========== REGISTRATION ==========
        std::cout << "=== Registration ===\n";
        auto checking_server = make_server("checking-server");
        ok &= expect_refused(*checking_server, "misspelled_type",
                             {{"type", "object"}, {"properties", {{"n", {{"type", "integr"}}}}}},
                             mcp::SchemaError::Category::eInvalidSchema);
        ok &= expect_refused(
            *checking_server, "remote_ref",
            {{"type", "object"}, {"properties", {{"n", {{"$ref", "https://example.com/n.json"}}}}}},
            mcp::SchemaError::Category::eUnresolvedRef);
        add_tools(*checking_server);

        // The same tools on a server that does not check results, so the client's check shows.
        auto lenient_server = make_server("lenient-server");
        mcp::ToolValidationOptions lenient;
        lenient.validate_structured_content = false;
        lenient_server->set_tool_validation(lenient);
        add_tools(*lenient_server);

        auto [checking_server_end, checking_client_end] =
            mcp::create_memory_transport_pair(io_ctx.get_executor());
        auto [lenient_server_end, lenient_client_end] =
            mcp::create_memory_transport_pair(io_ctx.get_executor());

        asio::co_spawn(io_ctx, serve(checking_server.get(), checking_server_end, io_ctx.get_executor()),
                       asio::detached);
        asio::co_spawn(io_ctx, serve(lenient_server.get(), lenient_server_end, io_ctx.get_executor()),
                       asio::detached);

        bool clients_ok = false;
        asio::co_spawn(io_ctx,
                       run_clients(&io_ctx, checking_client_end, lenient_client_end, &clients_ok),
                       asio::detached);

        // ========== AUTO-EXIT TIMER ==========
        // Stop after 10 seconds if the clients never finish, and report that as a failure.
        asio::steady_timer exit_timer(io_ctx.get_executor(), std::chrono::seconds(10));
        exit_timer.async_wait([&io_ctx](const boost::system::error_code& error) {
            if (!error) {
                std::cerr << "[Main] Timed out\n";
                io_ctx.stop();
            }
        });

        io_ctx.run();
        ok &= clients_ok;

        const auto stats = checking_server->tool_validation_stats();
        std::cout << "\n=== Stats ===\n";
        ok &= check(stats.argument_rejections == 1, "tool_validation_stats().argument_rejections == 1");
        ok &= check(stats.structured_content_rejections == 1,
                    "tool_validation_stats().structured_content_rejections == 1");

        std::cout << (ok ? "\nAll checks passed\n" : "\nSome checks failed\n");
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
