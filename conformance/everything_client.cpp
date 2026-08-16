// Scenario-driven client fixture for the official MCP conformance runner.

#include <mcp/client/client.hpp>
#include <mcp/transport/http_client.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct EmptyArguments {};

void to_json(nlohmann::json& json, const EmptyArguments&) { json = nlohmann::json::object(); }

void from_json(const nlohmann::json&, EmptyArguments&) {}

struct AddNumbersArguments {
    int a;
    int b;
};

void to_json(nlohmann::json& json, const AddNumbersArguments& arguments) {
    json = {{"a", arguments.a}, {"b", arguments.b}};
}

void from_json(const nlohmann::json& json, AddNumbersArguments& arguments) {
    json.at("a").get_to(arguments.a);
    json.at("b").get_to(arguments.b);
}

std::string required_environment(std::string_view name) {
    const auto* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string(name) + " is not set");
    }
    return value;
}

nlohmann::json apply_schema_defaults(const nlohmann::json& request) {
    nlohmann::json content = nlohmann::json::object();
    if (!request.contains("requestedSchema")) {
        return content;
    }

    const auto& schema = request.at("requestedSchema");
    if (!schema.contains("properties") || !schema.at("properties").is_object()) {
        return content;
    }

    for (const auto& [name, property] : schema.at("properties").items()) {
        if (property.is_object() && property.contains("default")) {
            content[name] = property.at("default");
        }
    }
    return content;
}

void install_reverse_rpc_handlers(mcp::Client& client) {
    client.on_request(
        "elicitation/create", [](const nlohmann::json& request) -> mcp::Task<nlohmann::json> {
            nlohmann::json result = {{"action", "accept"}, {"content", apply_schema_defaults(request)}};
            co_return result;
        });

    client.on_request("sampling/createMessage", [](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
        nlohmann::json result = {
            {"role", "assistant"},
            {"content", {{"type", "text"}, {"text", "Conformance sample response"}}},
            {"model", "mcp-cpp-sdk-conformance-model"},
            {"stopReason", "endTurn"}};
        co_return result;
    });
}

mcp::Task<mcp::InitializeResult> connect_conformance_client(mcp::Client& client) {
    mcp::Implementation implementation;
    implementation.name = "mcp-cpp-sdk-conformance-client";
    implementation.version = "0.2.0";

    mcp::ClientCapabilities capabilities;
    mcp::ClientCapabilities::ElicitationCapability elicitation;
    elicitation.form = nlohmann::json::object();
    capabilities.elicitation = std::move(elicitation);
    capabilities.sampling = mcp::ClientCapabilities::SamplingCapability{};
    capabilities.roots = mcp::ClientCapabilities::RootsCapability{false};

    return client.connect(implementation, capabilities);
}

mcp::Task<void> run_initialize(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.list_tools();
    client.close();
}

mcp::Task<void> run_tools_call(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.list_tools();
    (void)co_await client.call_tool("add_numbers", AddNumbersArguments{5, 3});
    client.close();
}

mcp::Task<void> run_elicitation_defaults(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.call_tool("test_client_elicitation_defaults", EmptyArguments{});
    client.close();
}

mcp::Task<void> run_sse_retry(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.call_tool("test_reconnection", EmptyArguments{});
    client.close();
}

mcp::Task<void> run_scenario(mcp::Client& client, std::string_view scenario) {
    if (scenario == "initialize") {
        return run_initialize(client);
    }
    if (scenario == "tools_call") {
        return run_tools_call(client);
    }
    if (scenario == "elicitation-sep1034-client-defaults") {
        return run_elicitation_defaults(client);
    }
    if (scenario == "sse-retry") {
        return run_sse_retry(client);
    }
    throw std::runtime_error("unsupported conformance client scenario: " + std::string(scenario));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: mcp-conformance-everything-client <server-url>\n";
        return EXIT_FAILURE;
    }

    try {
        auto scenario = required_environment("MCP_CONFORMANCE_SCENARIO");
        std::string server_url = argv[argc - 1];

        boost::asio::io_context io_context;
        auto transport =
            std::make_shared<mcp::HttpClientTransport>(io_context.get_executor(), server_url);
        mcp::Client client(transport, io_context.get_executor());
        install_reverse_rpc_handlers(client);

        auto completion =
            boost::asio::co_spawn(io_context, run_scenario(client, scenario), boost::asio::use_future);
        io_context.run();
        completion.get();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Conformance client failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
