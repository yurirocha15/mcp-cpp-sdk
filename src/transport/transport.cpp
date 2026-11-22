#include <mcp/core/async.hpp>
#include <mcp/transport/transport.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace mcp {

Task<IncomingMessage> ITransport::read_incoming() {
    auto message = co_await read_message();
    co_return IncomingMessage{std::move(message), {}};
}

Task<void> ITransport::write_related_message(std::string_view message,
                                             std::string_view related_request) {
    static_cast<void>(related_request);
    co_await write_message(message);
}

}  // namespace mcp
