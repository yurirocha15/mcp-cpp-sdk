#include <mcp/transport_factory.hpp>

#include <mcp/detail/runtime_access.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/stdio.hpp>

namespace mcp {

TransportFactory::TransportFactory(Runtime& runtime) : runtime_(runtime) {}

std::shared_ptr<ITransport> TransportFactory::create_stdio() const {
    return std::make_shared<StdioTransport>(detail::get_executor(runtime_));
}

std::shared_ptr<ITransport> TransportFactory::create_http_client(const std::string& url) const {
    return std::make_shared<HttpClientTransport>(detail::get_executor(runtime_), url);
}

std::shared_ptr<ITransport> make_stdio_transport(Runtime& runtime) {
    return TransportFactory(runtime).create_stdio();
}

std::shared_ptr<ITransport> make_http_client_transport(Runtime& runtime, const std::string& url) {
    return TransportFactory(runtime).create_http_client(url);
}

}  // namespace mcp
