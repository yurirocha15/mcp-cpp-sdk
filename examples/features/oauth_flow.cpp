/// @file oauth_flow.cpp
/// @brief Demonstrates a self-contained OAuth flow with mock discovery, token exchange, and refresh.
///
/// This example shows:
/// - In-process OAuth discovery + token endpoints served by a mock HTTP server
/// - OAuthDiscoveryClient resolving protected-resource and auth-server metadata
/// - OAuthAuthenticator with InMemoryTokenStore storing the exchanged token
/// - OAuthClientTransport wrapping a MemoryTransport client connection
/// - Automatic auth-token injection into MCP requests
/// - Automatic refresh after a server-side auth failure
/// - make_auth_middleware() protecting MCP tools on the server side

#include <mcp/auth/oauth.hpp>
#include <mcp/client.hpp>
#include <mcp/protocol.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

namespace {

std::string percent_decode(std::string_view encoded) {
    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        return -1;
    };

    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            const int hi = hex_value(encoded[i + 1]);
            const int lo = hex_value(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        decoded.push_back(encoded[i] == '+' ? ' ' : encoded[i]);
    }

    return decoded;
}

std::map<std::string, std::string, std::less<>> parse_form_body(std::string_view body) {
    std::map<std::string, std::string, std::less<>> params;

    std::size_t start = 0;
    while (start <= body.size()) {
        const auto end = body.find('&', start);
        const auto token =
            body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);
        if (!token.empty()) {
            const auto equals = token.find('=');
            const auto key = token.substr(0, equals);
            const auto value =
                equals == std::string_view::npos ? std::string_view{} : token.substr(equals + 1);
            params.emplace(percent_decode(key), percent_decode(value));
        }

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    return params;
}

mcp::CallToolResult make_text_result(std::string text, std::optional<bool> is_error = std::nullopt,
                                     std::optional<json> structured = std::nullopt) {
    mcp::CallToolResult result;
    mcp::TextContent content;
    content.text = std::move(text);
    result.content.emplace_back(std::move(content));
    result.isError = std::move(is_error);
    result.structuredContent = std::move(structured);
    return result;
}

class MockOAuthServer {
   public:
    MockOAuthServer(asio::io_context& io_ctx, unsigned short port)
        : acceptor_(io_ctx, {asio::ip::make_address("127.0.0.1"), port}),
          port_(port),
          issuer_("http://127.0.0.1:" + std::to_string(port_)),
          protected_resource_url_(issuer_ + "/memory-mcp") {}

    [[nodiscard]] const std::string& issuer() const { return issuer_; }

    [[nodiscard]] const std::string& protected_resource_url() const { return protected_resource_url_; }

    [[nodiscard]] const std::vector<std::string>& observed_tokens() const { return observed_tokens_; }

    [[nodiscard]] const std::string& last_observed_token() const { return last_observed_token_; }

    [[nodiscard]] int refresh_count() const { return refresh_count_; }

    [[nodiscard]] std::string issue_authorization_code(const std::string& expected_code_verifier) {
        const auto code = "demo-code-" + std::to_string(++next_code_id_);
        authorization_codes_[code] = expected_code_verifier;
        return code;
    }

    bool validate_token(const std::string& token) {
        observed_tokens_.push_back(token);
        last_observed_token_ = token;
        return token == refreshed_access_token_;
    }

    void stop() {
        beast::error_code ec;
        acceptor_.cancel(ec);
        acceptor_.close(ec);
    }

    asio::awaitable<void> listen() {
        try {
            for (;;) {
                auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
                asio::co_spawn(acceptor_.get_executor(), handle_session(std::move(socket)),
                               asio::detached);
            }
        } catch (const boost::system::system_error& error) {
            if (error.code() != asio::error::operation_aborted &&
                error.code() != asio::error::bad_descriptor) {
                throw;
            }
        }
    }

   private:
    static http::response<http::string_body> make_json_response(http::status status, int version,
                                                                const json& payload) {
        http::response<http::string_body> response{status, version};
        response.set(http::field::content_type, "application/json");
        response.keep_alive(false);
        response.body() = payload.dump();
        response.prepare_payload();
        return response;
    }

    http::response<http::string_body> handle_request(const http::request<http::string_body>& request) {
        if (request.method() == http::verb::get &&
            request.target() == "/.well-known/oauth-protected-resource/memory-mcp") {
            return make_json_response(http::status::ok, request.version(),
                                      json{{"resource", protected_resource_url_},
                                           {"authorization_servers", json::array({issuer_})},
                                           {"scopes_supported", json::array({"mcp:demo"})}});
        }

        if (request.method() == http::verb::get &&
            (request.target() == "/.well-known/oauth-authorization-server" ||
             request.target() == "/.well-known/openid-configuration")) {
            return make_json_response(
                http::status::ok, request.version(),
                json{{"issuer", issuer_},
                     {"authorization_endpoint", issuer_ + "/authorize"},
                     {"token_endpoint", issuer_ + "/token"},
                     {"grant_types_supported", json::array({"authorization_code", "refresh_token"})},
                     {"response_types_supported", json::array({"code"})},
                     {"code_challenge_methods_supported", json::array({"S256"})},
                     {"scopes_supported", json::array({"mcp:demo"})}});
        }

        if (request.method() == http::verb::post && request.target() == "/token") {
            const auto params = parse_form_body(request.body());
            const auto grant_type_it = params.find("grant_type");
            if (grant_type_it == params.end()) {
                return make_json_response(
                    http::status::bad_request, request.version(),
                    json{{"error", "invalid_request"}, {"error_description", "missing grant_type"}});
            }

            if (grant_type_it->second == "authorization_code") {
                const auto code_it = params.find("code");
                const auto verifier_it = params.find("code_verifier");
                if (code_it == params.end() || verifier_it == params.end()) {
                    return make_json_response(http::status::bad_request, request.version(),
                                              json{{"error", "invalid_request"},
                                                   {"error_description", "missing code or verifier"}});
                }

                const auto auth_code_it = authorization_codes_.find(code_it->second);
                if (auth_code_it == authorization_codes_.end() ||
                    auth_code_it->second != verifier_it->second) {
                    return make_json_response(
                        http::status::bad_request, request.version(),
                        json{{"error", "invalid_grant"},
                             {"error_description", "authorization code rejected"}});
                }

                authorization_codes_.erase(auth_code_it);
                std::cout
                    << "[OAuth HTTP] Exchanged auth code for an intentionally stale access token\n";
                return make_json_response(http::status::ok, request.version(),
                                          json{{"access_token", expired_access_token_},
                                               {"token_type", "Bearer"},
                                               {"refresh_token", refresh_token_},
                                               {"expires_in", 3600},
                                               {"scope", "mcp:demo"}});
            }

            if (grant_type_it->second == "refresh_token") {
                const auto refresh_it = params.find("refresh_token");
                if (refresh_it == params.end() || refresh_it->second != refresh_token_) {
                    return make_json_response(http::status::bad_request, request.version(),
                                              json{{"error", "invalid_grant"},
                                                   {"error_description", "refresh token rejected"}});
                }

                ++refresh_count_;
                std::cout << "[OAuth HTTP] Refreshed token after auth failure\n";
                return make_json_response(http::status::ok, request.version(),
                                          json{{"access_token", refreshed_access_token_},
                                               {"token_type", "Bearer"},
                                               {"refresh_token", refresh_token_},
                                               {"expires_in", 3600},
                                               {"scope", "mcp:demo"}});
            }
        }

        return make_json_response(http::status::not_found, request.version(),
                                  json{{"error", "not_found"}});
    }

    asio::awaitable<void> handle_session(tcp::socket socket) {
        beast::tcp_stream stream(std::move(socket));

        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        co_await http::async_read(stream, buffer, request, asio::use_awaitable);

        auto response = handle_request(request);
        co_await http::async_write(stream, response, asio::use_awaitable);

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    }

    tcp::acceptor acceptor_;
    unsigned short port_;
    std::string issuer_;
    std::string protected_resource_url_;
    std::map<std::string, std::string, std::less<>> authorization_codes_;
    std::vector<std::string> observed_tokens_;
    std::string last_observed_token_;
    int next_code_id_ = 0;
    int refresh_count_ = 0;
    const std::string expired_access_token_ = "expired-access-token";
    const std::string refreshed_access_token_ = "refreshed-access-token";
    const std::string refresh_token_ = "demo-refresh-token";
};

class MiddlewareAuthErrorBridgeTransport final : public mcp::ITransport {
   public:
    explicit MiddlewareAuthErrorBridgeTransport(std::shared_ptr<mcp::ITransport> inner)
        : inner_(std::move(inner)) {}

    mcp::Task<std::string> read_message() override { co_return co_await inner_->read_message(); }

    mcp::Task<void> write_message(std::string_view message) override {
        std::string outbound(message);

        try {
            const auto json_msg = json::parse(outbound);
            if (json_msg.contains("id") && json_msg.contains("result")) {
                const auto& result = json_msg.at("result");
                if (result.is_object() && result.value("isError", false) &&
                    result.contains("content") && result.at("content").is_array() &&
                    !result.at("content").empty()) {
                    const auto& first_block = result.at("content").front();
                    const auto text = first_block.value("text", std::string{});
                    if (text.starts_with("Unauthorized:")) {
                        mcp::JSONRPCErrorResponse error_response;
                        error_response.id = json_msg.at("id").get<mcp::RequestId>();
                        error_response.error.code = mcp::g_UNAUTHORIZED;
                        error_response.error.message = text;
                        outbound = json(error_response).dump();
                        std::cout << "[Server] Middleware rejection mapped to JSON-RPC auth error\n";
                    }
                }
            }
        } catch (const std::exception&) {
            // Keep the original payload on parse errors.
        }

        co_await inner_->write_message(outbound);
    }

    void close() override { inner_->close(); }

   private:
    std::shared_ptr<mcp::ITransport> inner_;
};

struct ClientFlowRuntime {
    asio::io_context* io_ctx;
    MockOAuthServer* mock_oauth;
    std::shared_ptr<MiddlewareAuthErrorBridgeTransport> server_transport;
    std::shared_ptr<mcp::ITransport> client_base_transport;
    std::shared_ptr<asio::steady_timer> exit_timer;
    int* exit_code;
};

struct ClientFlowState {
    std::shared_ptr<mcp::auth::OAuthHttpClient> oauth_http;
    std::shared_ptr<mcp::auth::InMemoryTokenStore> token_store;
    std::shared_ptr<mcp::auth::OAuthAuthenticator> authenticator;
    std::optional<mcp::auth::ProtectedResourceMetadata> protected_metadata;
    std::optional<mcp::auth::AuthServerMetadata> auth_metadata;
    std::optional<mcp::auth::TokenResponse> stored_after_refresh;
    mcp::auth::OAuthConfig oauth_config;
    mcp::auth::PkcePair pkce;
    std::string auth_code;
};

std::string make_initialize_request_wire() {
    return json{{"jsonrpc", "2.0"},
                {"id", "1"},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
                  {"clientInfo", {{"name", "oauth-flow-example-client"}, {"version", "1.0.0"}}},
                  {"capabilities", json::object()}}}}
        .dump();
}

std::string make_initialized_notification_wire() {
    return json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump();
}

std::string make_secure_echo_request_wire() {
    return json{{"jsonrpc", "2.0"},
                {"id", "2"},
                {"method", "tools/call"},
                {"params", {{"name", "secure_echo"}, {"arguments", {{"message", "hello oauth"}}}}}}
        .dump();
}

void stop_runtime(const ClientFlowRuntime& runtime) {
    runtime.mock_oauth->stop();
    runtime.server_transport->close();
    runtime.client_base_transport->close();
    runtime.exit_timer->cancel();
}

auto run_client_demo(ClientFlowRuntime runtime) -> mcp::Task<void> {
    auto state = std::make_shared<ClientFlowState>();

    try {
        asio::steady_timer startup_delay(runtime.io_ctx->get_executor());
        startup_delay.expires_after(std::chrono::milliseconds(75));
        co_await startup_delay.async_wait(asio::use_awaitable);

        std::cout << "OAuth mock server: " << runtime.mock_oauth->issuer() << '\n';
        std::cout << "MCP client/server transport: MemoryTransport loopback\n\n";

        state->oauth_http =
            std::make_shared<mcp::auth::OAuthHttpClient>(runtime.io_ctx->get_executor());
        mcp::auth::OAuthDiscoveryClient discovery(state->oauth_http);

        state->protected_metadata = co_await discovery.discover_protected_resource(
            runtime.mock_oauth->protected_resource_url());
        state->auth_metadata = co_await discovery.discover_auth_server(
            state->protected_metadata->authorization_servers.front());

        std::cout << "[Client] Discovered protected resource: " << state->protected_metadata->resource
                  << '\n';
        std::cout << "[Client] Discovered token endpoint: " << state->auth_metadata->token_endpoint
                  << '\n';

        state->pkce = mcp::auth::generate_pkce_pair();
        state->auth_code = runtime.mock_oauth->issue_authorization_code(state->pkce.code_verifier);
        std::cout << "[Client] Generated PKCE challenge: " << state->pkce.code_challenge << '\n';

        state->oauth_config.client_id = "oauth-flow-example-client";
        state->oauth_config.token_endpoint = state->auth_metadata->token_endpoint;
        state->oauth_config.authorization_endpoint = state->auth_metadata->authorization_endpoint;
        state->oauth_config.redirect_uri = "http://localhost/callback";
        state->oauth_config.scope = "mcp:demo";

        state->token_store = std::make_shared<mcp::auth::InMemoryTokenStore>();
        state->authenticator = std::make_shared<mcp::auth::OAuthAuthenticator>(
            state->token_store, state->oauth_http, state->oauth_config,
            state->protected_metadata->resource);

        {
            auto initial_token = co_await state->oauth_http->exchange_code(
                state->oauth_config, state->auth_code, state->pkce.code_verifier);
            state->authenticator->store_token(initial_token);
        }
        std::cout << "[Client] Stored initial access token: "
                  << state->authenticator->get_access_token() << "\n";

        {
            auto oauth_transport = std::make_shared<mcp::auth::OAuthClientTransport>(
                runtime.client_base_transport, state->authenticator);

            co_await oauth_transport->write_message(make_initialize_request_wire());

            auto init_response_wire = co_await oauth_transport->read_message();
            auto init_response = json::parse(init_response_wire);
            std::cout << "[Client] Connected to: "
                      << init_response.at("result").at("serverInfo").at("name").get<std::string>()
                      << '\n';

            co_await oauth_transport->write_message(make_initialized_notification_wire());

            std::cout << "[Client] Calling secure_echo with the stale token to trigger refresh...\n";
            co_await oauth_transport->write_message(make_secure_echo_request_wire());

            auto tool_response_wire = co_await oauth_transport->read_message();
            auto tool_response = json::parse(tool_response_wire);

            if (tool_response.contains("error")) {
                throw std::runtime_error("secure_echo returned a JSON-RPC error after retry");
            }

            const auto& tool_result = tool_response.at("result");
            if (tool_result.value("isError", false)) {
                throw std::runtime_error("secure_echo returned an unexpected tool error");
            }

            state->stored_after_refresh = state->token_store->load(state->protected_metadata->resource);
            if (!state->stored_after_refresh ||
                state->stored_after_refresh->access_token != "refreshed-access-token") {
                throw std::runtime_error("token refresh did not persist the refreshed access token");
            }
            if (runtime.mock_oauth->refresh_count() != 1) {
                throw std::runtime_error("expected exactly one refresh request");
            }
            if (runtime.mock_oauth->observed_tokens().size() < 2) {
                throw std::runtime_error("expected to observe both stale and refreshed bearer tokens");
            }

            if (tool_result.contains("content") && tool_result.at("content").is_array() &&
                !tool_result.at("content").empty()) {
                std::cout << "[Client] Tool response: "
                          << tool_result.at("content").front().at("text").get<std::string>() << '\n';
            }
            if (tool_result.contains("structuredContent")) {
                std::cout << "[Client] structuredContent: "
                          << tool_result.at("structuredContent").dump() << '\n';
            }

            oauth_transport->close();
        }

        std::cout << "[Client] Observed tokens on the server: ";
        for (std::size_t i = 0; i < runtime.mock_oauth->observed_tokens().size(); ++i) {
            if (i != 0) {
                std::cout << " -> ";
            }
            std::cout << runtime.mock_oauth->observed_tokens()[i];
        }
        std::cout << '\n';

        std::cout << "[Client] Refreshed access token in store: "
                  << state->stored_after_refresh->access_token << '\n';
        std::cout << "[Client] OAuth flow completed successfully\n";

        runtime.mock_oauth->stop();
        runtime.exit_timer->cancel();
    } catch (const std::exception& error) {
        *runtime.exit_code = EXIT_FAILURE;
        std::cerr << "[Client] Error: " << error.what() << '\n';
        stop_runtime(runtime);
    }
}

}  // namespace

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;
        int exit_code = EXIT_SUCCESS;

        constexpr unsigned short oauth_port = 18120;
        MockOAuthServer mock_oauth(io_ctx, oauth_port);

        // ========== MOCK AUTH SERVER SETUP ==========
        // (MockOAuthServer already initialized above)

        // ========== MCP SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "oauth-demo-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== AUTH MIDDLEWARE ==========
        server.use(
            mcp::auth::make_auth_middleware([&mock_oauth](const std::string& token) -> Task<bool> {
                const bool valid = mock_oauth.validate_token(token);
                std::cout << "[Server] Validating token: " << token
                          << (valid ? " (accepted)" : " (rejected)") << '\n';
                co_return valid;
            }));

        server.add_tool<json, CallToolResult>(
            "secure_echo", "Echo a message only when the bearer token is valid",
            json{{"type", "object"},
                 {"properties", {{"message", {{"type", "string"}}}}},
                 {"required", json::array({"message"})}},
            [&mock_oauth](const json& args) -> CallToolResult {
                return make_text_result("Server handled '" + args.at("message").get<std::string>() +
                                            "' using token " + mock_oauth.last_observed_token(),
                                        std::nullopt,
                                        json{{"tokenSeen", mock_oauth.last_observed_token()}});
            });

        auto [server_base_transport, client_base_transport] =
            create_memory_transport_pair(io_ctx.get_executor());
        auto server_transport =
            std::make_shared<MiddlewareAuthErrorBridgeTransport>(server_base_transport);

        // ========== TRANSPORT & RUN ==========
        auto exit_timer = std::make_shared<asio::steady_timer>(io_ctx.get_executor());
        exit_timer->expires_after(std::chrono::seconds(10));

        asio::co_spawn(io_ctx, mock_oauth.listen(), asio::detached);

        asio::co_spawn(
            io_ctx,
            [&, transport = server_transport]() -> Task<void> {
                co_await server.run(transport, io_ctx.get_executor());
            },
            asio::detached);

        asio::co_spawn(
            io_ctx,
            [&, exit_timer]() -> Task<void> {
                try {
                    co_await exit_timer->async_wait(asio::use_awaitable);
                    std::cerr << "[Main] Auto-exit timer fired before the OAuth flow completed\n";
                    exit_code = EXIT_FAILURE;
                    mock_oauth.stop();
                    server_transport->close();
                    client_base_transport->close();
                    io_ctx.stop();
                } catch (const boost::system::system_error& error) {
                    if (error.code() != asio::error::operation_aborted) {
                        throw;
                    }
                }
            },
            asio::detached);

        asio::co_spawn(
            io_ctx,
            run_client_demo(ClientFlowRuntime{&io_ctx, &mock_oauth, server_transport,
                                              client_base_transport, exit_timer, &exit_code}),
            asio::detached);

        io_ctx.run();
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
