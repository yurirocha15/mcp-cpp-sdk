// client_add_http.cpp sending a bearer token with each request, as server_add_auth.cpp requires.

#include <mcp/mcp.hpp>
#include <mcp/transport/http_client.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/mcp";
    mcp::HttpEndpointOptions http;
    http.configure = [](mcp::HttpClientTransport& transport) {
        transport.set_bearer_token_provider([] { return std::string("greeter-secret-token"); });
    };
    mcp::Client client(mcp::Endpoint::http(url, http));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
}
