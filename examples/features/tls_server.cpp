/// @file tls_server.cpp
/// @brief Serves an MCP server over https, and asks clients for a certificate when told to.
///
/// Usage: example-feature-tls-server [CERT_FILE KEY_FILE [CLIENT_CA_FILE]]
///
/// This example shows:
/// - run_http() ending TLS itself, with the server's certificate chain and key read from PEM files
/// - Mutual TLS: with CLIENT_CA_FILE, a client must show a certificate that authority issued
///
/// Without the files it says what to pass and stops.

#include <mcp/mcp.hpp>
#include <mcp/transport/http_session_manager.hpp>
#include <mcp/transport/tls.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Pass CERT_FILE KEY_FILE [CLIENT_CA_FILE] to serve https on port 8443\n";
        return 0;
    }

    mcp::Server server("tls-server", "1.0.0");
    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    // ========== TLS SETTINGS ==========
    mcp::TlsServerOptions tls;
    tls.certificate_chain_file = argv[1];  // the certificate, then its intermediates
    tls.private_key_file = argv[2];
    if (argc > 3) {
        tls.client_auth = mcp::TlsServerOptions::ClientAuth::eRequired;
        tls.client_ca_files = {argv[3]};
    }
    // ========== END TLS SETTINGS ==========

    try {
        // ========== SERVE ==========
        server.run_http("0.0.0.0", 8443,
                        [&tls](mcp::StreamableHttpSessionManager& http) { http.set_tls(tls); });
        // ========== END SERVE ==========
    } catch (const mcp::TlsConfigError& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
