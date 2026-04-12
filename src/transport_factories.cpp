#include <mcp/transport_factory.hpp>

#include <mcp/detail/runtime_access.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/stdio.hpp>

namespace mcp {

std::unique_ptr<ITransport> make_stdio_transport(Runtime& runtime) {
    return std::make_unique<StdioTransport>(detail::get_executor(runtime));
}

std::unique_ptr<ITransport> make_http_client_transport(Runtime& runtime, const std::string& url) {
    return std::make_unique<HttpClientTransport>(detail::get_executor(runtime), url);
}

}  // namespace mcp
