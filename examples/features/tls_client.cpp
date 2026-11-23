/// @file tls_client.cpp
/// @brief Connects to an MCP server over https, through the proxy the environment names.
///
/// Usage: example-feature-tls-client [URL [CA_FILE]]
///
/// This example shows:
/// - TLS settings shared by every connection: the system's certificate authorities, and a private
///   one from a PEM file
/// - An Endpoint::http() client whose transport uses those settings and the proxy that
///   HTTPS_PROXY, HTTP_PROXY and NO_PROXY name
/// - The TlsError a failed handshake throws, nested in the McpError of the call
///
/// Without a URL it builds the settings, which reads the system's certificate authorities, and
/// stops.

#include <mcp/client/client.hpp>
#include <mcp/client/endpoint.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/proxy.hpp>
#include <mcp/transport/tls.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

int main(int argc, char** argv) {
    // ========== TLS SETTINGS ==========
    mcp::TlsClientOptions tls_options;  // trusts the system's certificate authorities
    if (argc > 2) {
        tls_options.ca_files = {argv[2]};  // and a private one
    }
    const auto tls = mcp::TlsClientContext::create(tls_options);
    // ========== END TLS SETTINGS ==========

    if (argc < 2) {
        std::cout << "TLS settings ready; pass an https:// URL to connect\n";
        return 0;
    }

    try {
        // ========== CONNECT ==========
        mcp::HttpEndpointOptions endpoint;
        endpoint.configure = [tls](mcp::HttpClientTransport& transport) {
            transport.set_tls(tls);
            transport.set_proxy(mcp::ProxyOptions::from_environment());
        };
        mcp::Client client(mcp::Endpoint::http(argv[1], std::move(endpoint)));
        const auto server = client.sync().connect("tls-client", "1.0");
        std::cout << "Connected over TLS, protocol " << server.protocolVersion << "\n";
        // ========== END CONNECT ==========
    } catch (const mcp::McpError& error) {
        // ========== FAILURE ==========
        try {
            std::rethrow_if_nested(error);
        } catch (const mcp::TlsError& tls_error) {
            if (tls_error.reason() == mcp::TlsError::Reason::eUntrustedCertificate) {
                std::cerr << "Name the server's certificate authority as CA_FILE\n";
            }
        } catch (...) {
            // Another cause: the message below describes it.
        }
        std::cerr << error.what() << "\n";
        return 1;
        // ========== END FAILURE ==========
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
