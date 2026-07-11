#include <mcp/mcp.hpp>
#include <mcp/transport/http_server.hpp>

#include <cstdlib>

int main() {
    mcp::Runtime runtime;
    mcp::TransportFactory transport_factory(runtime);
    auto transport = transport_factory.create_stdio();

    mcp::ServerCapabilities capabilities;
    mcp::Server server({"test-package", std::string(mcp::g_VERSION)}, capabilities);

    mcp::EventStore event_store(2);
    const auto event_id = event_store.append("{}");
    const bool boundary_is_usable = transport != nullptr && event_id == "1";
    return boundary_is_usable && mcp::version() == mcp::g_VERSION ? EXIT_SUCCESS : EXIT_FAILURE;
}
