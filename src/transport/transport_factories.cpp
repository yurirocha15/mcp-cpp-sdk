#include <mcp/transport/transport_factory.hpp>

#include <mcp/core/runtime.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/stdio.hpp>

namespace mcp {

std::shared_ptr<ITransport> make_stdio_transport(Runtime& runtime) {
    return std::make_shared<StdioTransport>(runtime.executor());
}

std::shared_ptr<ITransport> make_http_client_transport(Runtime& runtime, const std::string& url) {
    return std::make_shared<HttpClientTransport>(runtime.executor(), url);
}

}  // namespace mcp
