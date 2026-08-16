// Scenario-driven client fixture for the official MCP conformance runner.

#include <mcp/auth/oauth.hpp>
#include <mcp/client/client.hpp>
#include <mcp/transport/http_client.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
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

std::optional<std::string> optional_environment(std::string_view name) {
    const auto* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
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
    if (scenario.starts_with("auth/")) {
        // Every authorization scenario drives the same flow: one authenticated MCP request. What
        // differs between them is what the fixture's servers advertise, which the SDK reacts to on
        // its own.
        return run_initialize(client);
    }
    throw std::runtime_error("unsupported conformance client scenario: " + std::string(scenario));
}

// --- Authorization wiring -------------------------------------------------------------------
//
// This fixture is a host application: the SDK never opens a browser and never binds a listener, so
// carrying the user agent to the authorization endpoint and collecting the redirect is work the
// application does. Here that means one plain HTTP GET whose redirect is deliberately not followed,
// because the redirect target *is* the authorization response.

/// The client ID metadata document this fixture publishes. It is never fetched during the flow;
/// the URL is the client identifier itself.
constexpr std::string_view g_client_metadata_url =
    "https://conformance-test.local/client-metadata.json";

/// Redirect URI registered for the flow. The authorization server only echoes it back in a
/// `Location` header, so nothing ever connects to it.
constexpr std::string_view g_redirect_uri = "http://127.0.0.1:8080/callback";

struct RedirectLeg {
    std::string host;
    std::string port;
    std::string target;
    std::string location;
};

/// Split an `http://host[:port]/path` URL into the pieces a request needs.
RedirectLeg split_http_url(const std::string& url) {
    constexpr std::string_view scheme = "http://";
    if (!url.starts_with(scheme)) {
        throw std::runtime_error("Authorization URL is not plain HTTP: " + url);
    }
    auto rest = url.substr(scheme.size());
    const auto path_start = rest.find('/');

    RedirectLeg leg;
    leg.target = path_start == std::string::npos ? "/" : rest.substr(path_start);
    auto authority = path_start == std::string::npos ? rest : rest.substr(0, path_start);

    const auto colon = authority.find(':');
    if (colon == std::string::npos) {
        leg.host = std::move(authority);
        leg.port = "80";
    } else {
        leg.host = authority.substr(0, colon);
        leg.port = authority.substr(colon + 1);
    }
    return leg;
}

/// True for an origin served on this machine's loopback interface.
bool is_loopback_origin(const std::string& origin) {
    constexpr std::string_view scheme = "http://";
    if (!origin.starts_with(scheme)) {
        return false;
    }
    auto authority = origin.substr(scheme.size());
    if (authority.starts_with("[")) {
        const auto closing = authority.find(']');
        if (closing == std::string::npos) {
            return false;
        }
        authority = authority.substr(1, closing - 1);
    } else if (const auto colon = authority.find(':'); colon != std::string::npos) {
        authority = authority.substr(0, colon);
    }
    if (authority == "localhost") {
        return true;
    }
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(authority, error);
    return !error && address.is_loopback();
}

mcp::Task<mcp::auth::AuthorizationResponse> run_redirect_leg(boost::asio::any_io_executor executor,
                                                             std::shared_ptr<RedirectLeg> leg) {
    namespace beast = boost::beast;
    namespace http = beast::http;

    boost::asio::ip::tcp::resolver resolver(executor);
    const auto endpoints =
        co_await resolver.async_resolve(leg->host, leg->port, boost::asio::use_awaitable);

    beast::tcp_stream stream(executor);
    stream.expires_after(std::chrono::seconds(10));
    co_await stream.async_connect(endpoints, boost::asio::use_awaitable);

    http::request<http::empty_body> request(http::verb::get, leg->target, 11);
    request.set(http::field::host, leg->host);
    co_await http::async_write(stream, request, boost::asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, boost::asio::use_awaitable);

    boost::system::error_code ignored;
    (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);

    const auto location = response.find(http::field::location);
    if (location == response.end()) {
        throw std::runtime_error("Authorization endpoint returned no redirect to the redirect URI");
    }
    leg->location = std::string(location->value());
    co_return mcp::auth::parse_authorization_response(leg->location);
}

mcp::auth::AuthorizationCallback make_authorization_callback(boost::asio::any_io_executor executor) {
    return [executor](const mcp::auth::AuthorizationRequest& request)
               -> mcp::Task<mcp::auth::AuthorizationResponse> {
        return run_redirect_leg(
            executor, std::make_shared<RedirectLeg>(split_http_url(request.authorization_url)));
    };
}

mcp::auth::OAuthAuthorizationConfig make_authorization_config(const std::string& server_url) {
    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = server_url;
    config.redirect_uri = std::string(g_redirect_uri);
    config.client_identity.client_metadata_url = std::string(g_client_metadata_url);
    config.client_identity.metadata.client_name = "mcp-cpp-sdk-conformance-client";
    config.credential_store = std::make_shared<mcp::auth::InMemoryClientCredentialStore>();

    // Credentials the runner hands the fixture out of band. When they are present the SDK presents
    // them and never registers; when they are absent it chooses between the metadata document and
    // registration on what the authorization server advertises.
    if (const auto context = optional_environment("MCP_CONFORMANCE_CONTEXT")) {
        const auto parsed = nlohmann::json::parse(*context, nullptr, false);
        if (parsed.is_object() && parsed.contains("client_id")) {
            mcp::auth::OAuthClientInformation injected;
            injected.client_id = parsed.at("client_id").get<std::string>();
            if (parsed.contains("client_secret")) {
                injected.client_secret = parsed.at("client_secret").get<std::string>();
            }
            injected.source = mcp::auth::ClientIdentitySource::pre_registered;
            config.client_identity.pre_registered = std::move(injected);
        }
    }

    // The narrow loopback opt-out, enabled here and only here: the fixture's servers speak plain
    // HTTP on ephemeral loopback ports, which is not production-ready OAuth. The authorization
    // server's origin is only learned at run time from the protected resource's metadata, so the
    // allow list states the rule rather than an enumeration.
    config.policy.allow_plain_http_loopback = true;
    config.policy.allowed_origins.push_back(mcp::auth::metadata_url_origin(server_url));
    config.policy.origin_allowance = is_loopback_origin;
    return config;
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
        auto executor = io_context.get_executor();
        std::shared_ptr<mcp::ITransport> transport =
            std::make_shared<mcp::HttpClientTransport>(executor, server_url);

        if (std::string_view(scenario).starts_with("auth/")) {
            auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
                executor, std::make_shared<mcp::auth::InMemoryTokenStore>(),
                make_authorization_config(server_url), make_authorization_callback(executor));
            transport = std::make_shared<mcp::auth::OAuthClientTransport>(transport, manager);
        }

        mcp::Client client(transport, executor);
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
