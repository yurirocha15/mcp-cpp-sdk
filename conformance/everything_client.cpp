// Scenario-driven client fixture for the official MCP conformance runner. The 2025-11-25 scenarios
// and the 2026-07-28 request-shape scenarios share it; MCP_CONFORMANCE_SCENARIO names the one to run.

#include <mcp/auth/client_credentials.hpp>
#include <mcp/auth/enterprise_authorization.hpp>
#include <mcp/auth/oauth.hpp>
#include <mcp/client/client.hpp>
#include <mcp/client/skills.hpp>
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
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// The alpha runner names the revision each scenario runs at in MCP_CONFORMANCE_PROTOCOL_VERSION:
/// the one --spec-version or --requirements gives, otherwise 2026-07-28 for a scenario that
/// revision introduced and 2025-11-25 for the rest. The pinned runner does not set it, so there
/// the client keeps its default protocol detection.
mcp::ClientOptions client_options_from_environment() {
    mcp::ClientOptions options;
    if (const auto version = optional_environment("MCP_CONFORMANCE_PROTOCOL_VERSION")) {
        options.protocol = mcp::ClientProtocol::pinned(*version);
    }
    return options;
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

/// The scenario being run; the authorization extension scenarios declare their extension.
std::string g_scenario;

mcp::Task<mcp::ConnectResult> connect_conformance_client(mcp::Client& client) {
    mcp::Implementation implementation;
    implementation.name = "mcp-cpp-sdk-conformance-client";
    implementation.version = "0.2.0";

    mcp::ClientCapabilities capabilities;
    mcp::ClientCapabilities::ElicitationCapability elicitation;
    elicitation.form = nlohmann::json::object();
    capabilities.elicitation = std::move(elicitation);
    capabilities.sampling = mcp::ClientCapabilities::SamplingCapability{};
    capabilities.roots = mcp::ClientCapabilities::RootsCapability{false};
    if (g_scenario.starts_with("auth/client-credentials")) {
        mcp::auth::declare_client_credentials_support(capabilities);
    } else if (g_scenario == "auth/enterprise-managed-authorization") {
        mcp::auth::declare_enterprise_authorization_support(capabilities);
    }

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

mcp::Task<void> run_scope_step_up(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.list_tools();
    // The step-up fixture returns its 403 broader-scope challenge only for tools/call, so the
    // escalation leg exists only if the client actually calls a tool after the initial grant.
    (void)co_await client.call_tool("test-tool", EmptyArguments{});
    client.close();
}

/// The credentials and test values the runner hands the fixture out of band, if it handed it any.
std::optional<nlohmann::json> conformance_context();

/// Arguments that satisfy the required properties of a tool's input schema.
nlohmann::json minimal_arguments(const nlohmann::json& schema) {
    nlohmann::json arguments = nlohmann::json::object();
    if (!schema.contains("required") || !schema.at("required").is_array() ||
        !schema.contains("properties") || !schema.at("properties").is_object()) {
        return arguments;
    }
    for (const auto& name : schema.at("required")) {
        const auto& property =
            schema.at("properties").value(name.get<std::string>(), nlohmann::json::object());
        const auto type =
            property.is_object() ? property.value("type", std::string("string")) : "string";
        if (type == "integer" || type == "number") {
            arguments[name.get<std::string>()] = 0;
        } else if (type == "boolean") {
            arguments[name.get<std::string>()] = false;
        } else if (type == "array") {
            arguments[name.get<std::string>()] = nlohmann::json::array();
        } else if (type == "object") {
            arguments[name.get<std::string>()] = nlohmann::json::object();
        } else {
            arguments[name.get<std::string>()] = "test-default";
        }
    }
    return arguments;
}

/// Runs one call of a scenario and reports an error instead of ending the scenario: the scenario's
/// mock server judges what was sent, and several scenarios answer some calls with errors.
mcp::Task<void> try_call_tool(mcp::Client& client, std::string name, nlohmann::json arguments) {
    try {
        (void)co_await client.call_tool(name, arguments);
    } catch (const std::exception& error) {
        std::cerr << "tools/call " << name << " failed: " << error.what() << '\n';
    }
}

/// Lists the server's tools and calls each one, then reads its resources and gets its prompts.
/// The mock servers of the request-metadata, standard-header and invalid-tool-header scenarios judge
/// the requests this sends; a tool the client left out of the list is never called.
mcp::Task<void> run_server_walk(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    const auto tools = co_await client.list_tools();
    for (const auto& tool : tools.tools) {
        nlohmann::json arguments = minimal_arguments(tool.inputSchema);
        co_await try_call_tool(client, tool.name, std::move(arguments));
    }
    try {
        const auto resources = co_await client.list_resources();
        for (const auto& resource : resources.resources) {
            (void)co_await client.read_resource(resource.uri);
        }
    } catch (const std::exception& error) {
        std::cerr << "resources failed: " << error.what() << '\n';
    }
    try {
        const auto prompts = co_await client.list_prompts();
        for (const auto& prompt : prompts.prompts) {
            (void)co_await client.get_prompt(prompt.name);
        }
    } catch (const std::exception& error) {
        std::cerr << "prompts failed: " << error.what() << '\n';
    }
    client.close();
}

/// Lists the tools, whose schemas carry a `$ref` the client must not fetch.
mcp::Task<void> run_list_tools_only(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.list_tools();
    client.close();
}

/// Calls the tools the runner names in its context with the arguments it names.
mcp::Task<void> run_custom_headers(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.list_tools();
    const auto context = conformance_context();
    if (context && context->contains("toolCalls")) {
        for (const auto& call : context->at("toolCalls")) {
            std::string name = call.at("name").get<std::string>();
            nlohmann::json arguments = call.value("arguments", nlohmann::json::object());
            co_await try_call_tool(client, std::move(name), std::move(arguments));
        }
    }
    client.close();
}

/// Calls the tools of the request-state scenario one after the other; each answer to a question
/// comes from the elicitation handler.
mcp::Task<void> run_request_state(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    (void)co_await client.list_tools();
    const std::vector<std::string> names = {"test_mrtr_echo_state", "test_mrtr_no_state",
                                            "test_mrtr_unrelated", "test_mrtr_no_result_type"};
    for (const auto& name : names) {
        nlohmann::json arguments = nlohmann::json::object();
        co_await try_call_tool(client, name, std::move(arguments));
    }
    client.close();
}

/// Sends the schema of the focal tool back through the scenario's echo tool, unchanged.
mcp::Task<void> run_schema_preservation(mcp::Client& client) {
    (void)co_await connect_conformance_client(client);
    const auto tools = co_await client.list_tools();
    for (const auto& tool : tools.tools) {
        if (tool.name == "json_schema_2020_12_tool") {
            nlohmann::json arguments = nlohmann::json::object();
            arguments["schema"] = tool.inputSchema;
            co_await try_call_tool(client, "json_schema_echo", std::move(arguments));
        }
    }
    client.close();
}

/// The `key: value` lines of a SKILL.md's frontmatter block, as strings.
std::map<std::string, std::string> skill_frontmatter(const std::string& text) {
    std::map<std::string, std::string> fields;
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line) || line != "---") {
        return fields;
    }
    while (std::getline(lines, line) && line != "---") {
        const auto colon = line.find(": ");
        if (colon != std::string::npos) {
            fields[line.substr(0, colon)] = line.substr(colon + 2);
        }
    }
    return fields;
}

/// Whether the frontmatter of a fetched SKILL.md says what the skill's entry says.
bool frontmatter_matches(const mcp::Skill& entry, const std::string& text) {
    const auto fields = skill_frontmatter(text);
    for (const auto& [key, value] : entry.frontmatter.items()) {
        const auto found = fields.find(key);
        if (found == fields.end() || !value.is_string() || found->second != value.get<std::string>()) {
            return false;
        }
    }
    return fields.size() == entry.frontmatter.size();
}

/// Lists a server's skills. Unless the scenario is the no-prefetch one, loads the first skill: reads
/// its SKILL.md, which the SDK checks against the listed size and digest, compares the frontmatter,
/// and reads the supporting files only when every check holds.
mcp::Task<void> run_skills_host(mcp::Client& client, std::string_view scenario) {
    (void)co_await connect_conformance_client(client);
    const auto listing = co_await mcp::skills::list_skills(client);
    if (scenario != "sep-2640-client-no-prefetch" && !listing.skills.empty()) {
        const auto& entry = listing.skills.front();
        const auto skill_md = co_await mcp::skills::read_skill_file(client, entry, entry.uri);
        if (frontmatter_matches(entry, skill_md)) {
            for (const auto& resource : entry.resources) {
                if (resource.uri != entry.uri) {
                    (void)co_await mcp::skills::read_skill_file(client, entry, resource.uri);
                }
            }
        }
    }
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
    if (scenario == "auth/scope-step-up") {
        // Kept narrow on purpose: other auth fixtures use server builders that never register
        // "test-tool", and only scope-retry-limit tolerates a client error.
        return run_scope_step_up(client);
    }
    if (scenario == "request-metadata" || scenario == "http-standard-headers" ||
        scenario == "http-invalid-tool-headers") {
        return run_server_walk(client);
    }
    if (scenario == "json-schema-ref-no-deref") {
        return run_list_tools_only(client);
    }
    if (scenario == "http-custom-headers") {
        return run_custom_headers(client);
    }
    if (scenario == "sep-2322-client-request-state") {
        return run_request_state(client);
    }
    if (scenario == "json-schema-2020-12-preservation") {
        return run_schema_preservation(client);
    }
    if (scenario.starts_with("sep-2640-client-")) {
        return run_skills_host(client, scenario);
    }
    if (scenario.starts_with("auth/")) {
        // Every other authorization scenario drives the same flow: one authenticated MCP request.
        // What differs between them is what the fixture's servers advertise, which the SDK reacts
        // to on its own.
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
constexpr std::string_view g_CLIENT_METADATA_URL =
    "https://conformance-test.local/client-metadata.json";

/// Redirect URI registered for the flow. The authorization server only echoes it back in a
/// `Location` header, so nothing ever connects to it.
constexpr std::string_view g_REDIRECT_URI = "http://127.0.0.1:8080/callback";

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

/// The outbound-request policy every fixture request runs under.
///
/// The narrow loopback opt-out lives here and only here: the fixture's servers speak plain HTTP on
/// ephemeral loopback ports, which is not production-ready OAuth. The authorization server's origin
/// is only learned at run time from the protected resource's metadata, so the allow list states the
/// rule rather than an enumeration.
mcp::auth::MetadataFetchPolicy make_fetch_policy(const std::string& server_url) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allow_plain_http_loopback = true;
    policy.allowed_origins.push_back(mcp::auth::metadata_url_origin(server_url));
    policy.origin_allowance = is_loopback_origin;
    return policy;
}

/// The credentials the runner hands the fixture out of band, if it handed it any.
std::optional<nlohmann::json> conformance_context() {
    const auto context = optional_environment("MCP_CONFORMANCE_CONTEXT");
    if (!context) {
        return std::nullopt;
    }
    auto parsed = nlohmann::json::parse(*context, nullptr, false);
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

/// Resolve the issuer the runner's injected credentials are bound to.
///
/// The SDK refuses to present a `client_secret` that names no issuer, because on a real deployment
/// the authorization server is named by a document the client did not write. The fixture is the one
/// case where that document is trustworthy: the runner starts both the MCP server and the
/// authorization server for the scenario, and hands over the credentials it minted for that pair.
/// The runner does not put the issuer in `MCP_CONFORMANCE_CONTEXT` (conformance 0.1.16 sends only
/// `client_id` and `client_secret`), so this reads an `issuer` key when a future runner supplies
/// one and otherwise performs the protected-resource discovery step itself, under the same fetch
/// policy the SDK will use, and binds to the authorization server that document names.
///
/// This is a deliberate fixture-only decision and must not be copied into an application: an
/// application that binds its secret to whatever issuer the protected-resource document names has
/// re-created the very misbinding the SDK refusal exists to prevent. An application knows its own
/// issuer out of band, because that is where it registered.
std::string resolve_injected_issuer(boost::asio::io_context& io_context, const std::string& server_url,
                                    const nlohmann::json& context) {
    if (context.contains("issuer") && context.at("issuer").is_string()) {
        return context.at("issuer").get<std::string>();
    }

    auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(io_context.get_executor());
    http_client->set_metadata_policy(make_fetch_policy(server_url));
    auto discovery = std::make_shared<mcp::auth::OAuthDiscoveryClient>(http_client);

    auto resolved = boost::asio::co_spawn(
        io_context,
        [discovery, server_url]() -> boost::asio::awaitable<std::string> {
            const auto metadata = co_await discovery->discover_protected_resource(server_url);
            if (metadata.authorization_servers.empty()) {
                throw std::runtime_error(
                    "Protected resource metadata listed no authorization server to bind the "
                    "runner-supplied credentials to");
            }
            co_return metadata.authorization_servers.front();
        },
        boost::asio::use_future);

    io_context.run();
    io_context.restart();
    return resolved.get();
}

mcp::auth::OAuthAuthorizationConfig make_authorization_config(boost::asio::io_context& io_context,
                                                              const std::string& server_url) {
    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = server_url;
    config.redirect_uri = std::string(g_REDIRECT_URI);
    config.client_identity.client_metadata_url = std::string(g_CLIENT_METADATA_URL);
    config.client_identity.metadata.client_name = "mcp-cpp-sdk-conformance-client";
    config.credential_store = std::make_shared<mcp::auth::InMemoryClientCredentialStore>();
    config.policy = make_fetch_policy(server_url);

    // Credentials the runner hands the fixture out of band. When they are present the SDK presents
    // them and never registers; when they are absent it chooses between the metadata document and
    // registration on what the authorization server advertises.
    if (const auto context = conformance_context(); context && context->contains("client_id")) {
        mcp::auth::OAuthClientInformation injected;
        injected.client_id = context->at("client_id").get<std::string>();
        if (context->contains("client_secret")) {
            injected.client_secret = context->at("client_secret").get<std::string>();
            // A secret must name the issuer it belongs to or the SDK refuses to present it.
            injected.issuer = resolve_injected_issuer(io_context, server_url, *context);
        }
        injected.source = mcp::auth::ClientIdentitySource::ePreRegistered;
        config.client_identity.pre_registered = std::move(injected);
    }

    return config;
}

/// The authenticator of the authorization extension scenarios, which authorize without a person: the
/// runner hands over the credentials in `MCP_CONFORMANCE_CONTEXT`. Null for every other scenario.
std::shared_ptr<mcp::auth::Authenticator> make_machine_authenticator(
    const boost::asio::any_io_executor& executor, std::string_view scenario,
    const std::string& server_url) {
    const bool client_credentials = scenario.starts_with("auth/client-credentials");
    if (!client_credentials && scenario != "auth/enterprise-managed-authorization") {
        return nullptr;
    }
    const auto context = conformance_context();
    if (!context) {
        throw std::runtime_error("MCP_CONFORMANCE_CONTEXT is required for " + std::string(scenario));
    }
    auto token_store = std::make_shared<mcp::auth::InMemoryTokenStore>();

    if (client_credentials) {
        mcp::auth::ClientCredentialsConfig config;
        config.server_url = server_url;
        config.policy = make_fetch_policy(server_url);
        config.client.client_id = context->at("client_id").get<std::string>();
        if (context->contains("private_key_pem")) {
            mcp::auth::PrivateKeyJwtConfig key;
            key.private_key_pem = context->at("private_key_pem").get<std::string>();
            if (context->contains("signing_algorithm")) {
                key.algorithm = context->at("signing_algorithm").get<std::string>();
            }
            config.client.private_key_jwt = std::move(key);
        } else {
            config.client.client_secret = context->at("client_secret").get<std::string>();
        }
        return std::make_shared<mcp::auth::ClientCredentialsAuthenticator>(executor, token_store,
                                                                           std::move(config));
    }

    mcp::auth::EnterpriseAuthorizationConfig config;
    config.server_url = server_url;
    config.policy = make_fetch_policy(server_url);
    config.idp_token_endpoint = context->at("idp_token_endpoint").get<std::string>();
    config.idp_client.client_id = context->at("idp_client_id").get<std::string>();
    config.client.client_id = context->at("client_id").get<std::string>();
    config.client.client_secret = context->at("client_secret").get<std::string>();
    auto id_token = context->at("idp_id_token").get<std::string>();
    return std::make_shared<mcp::auth::EnterpriseAuthorizationAuthenticator>(
        executor, token_store, std::move(config),
        [id_token]() -> mcp::Task<mcp::auth::IdentityAssertion> {
            co_return mcp::auth::IdentityAssertion{id_token};
        });
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: mcp-conformance-everything-client <server-url>\n";
        return EXIT_FAILURE;
    }

    try {
        auto scenario = required_environment("MCP_CONFORMANCE_SCENARIO");
        g_scenario = scenario;
        std::string server_url = argv[argc - 1];

        boost::asio::io_context io_context;
        auto executor = io_context.get_executor();

        // Built before the transport exists, because binding runner-supplied credentials to their
        // issuer may need a discovery round trip that drives this io_context to completion first.
        auto machine_authenticator = make_machine_authenticator(executor, scenario, server_url);
        std::optional<mcp::auth::OAuthAuthorizationConfig> authorization_config;
        if (!machine_authenticator && std::string_view(scenario).starts_with("auth/")) {
            authorization_config = make_authorization_config(io_context, server_url);
        }

        std::shared_ptr<mcp::ITransport> transport =
            std::make_shared<mcp::HttpClientTransport>(executor, server_url);

        if (authorization_config) {
            auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
                executor, std::make_shared<mcp::auth::InMemoryTokenStore>(),
                std::move(*authorization_config), make_authorization_callback(executor));
            transport = std::make_shared<mcp::auth::OAuthClientTransport>(transport, manager);
        }

        if (machine_authenticator) {
            transport =
                std::make_shared<mcp::auth::OAuthClientTransport>(transport, machine_authenticator);
        }

        mcp::Client client(transport, executor, client_options_from_environment());
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
