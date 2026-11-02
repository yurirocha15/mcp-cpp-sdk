#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MCP_TEST_ADDRESS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MCP_TEST_ADDRESS_SANITIZER 1
#endif

// Not with AddressSanitizer: its runtime and UBSan's need descriptors of their own to check a type
// and to report, so with none left they call healthy objects invalid and then abort.
#if !defined(_WIN32) && !defined(MCP_TEST_ADDRESS_SANITIZER)

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

/// Takes every descriptor the process may open, leaving @p spare, until it is destroyed.
class DescriptorHog {
   public:
    explicit DescriptorHog(int spare) {
        getrlimit(RLIMIT_NOFILE, &original_);
        rlimit lowered = original_;
        lowered.rlim_cur = std::min<rlim_t>(original_.rlim_cur, 512);
        setrlimit(RLIMIT_NOFILE, &lowered);
        for (;;) {
            const int descriptor = open("/dev/null", O_RDONLY);
            if (descriptor < 0) {
                break;
            }
            held_.push_back(descriptor);
        }
        for (int count = 0; count < spare && !held_.empty(); ++count) {
            close(held_.back());
            held_.pop_back();
        }
    }
    DescriptorHog(const DescriptorHog&) = delete;
    DescriptorHog& operator=(const DescriptorHog&) = delete;
    ~DescriptorHog() {
        for (const int descriptor : held_) {
            close(descriptor);
        }
        setrlimit(RLIMIT_NOFILE, &original_);
    }

   private:
    rlimit original_{};
    std::vector<int> held_;
};

/// Connects while the server cannot accept, frees the descriptors, and returns the status the
/// server gives the connection that waited.
mcp::Task<unsigned int> status_after_exhaustion(asio::any_io_executor executor, unsigned short port) {
    auto stream = std::make_shared<beast::tcp_stream>(executor);
    {
        DescriptorHog hog(1);
        co_await stream->async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
        // The accept loop runs into the limit in this time.
        asio::steady_timer pause(executor, std::chrono::milliseconds(50));
        co_await pause.async_wait(asio::use_awaitable);
    }
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    request.body() = "not json";
    request.prepare_payload();
    co_await http::async_write(*stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    // Only a guard against a hang, for a server that stopped accepting.
    stream->expires_after(std::chrono::seconds(10));
    co_await http::async_read(*stream, buffer, response, asio::use_awaitable);
    co_return response.result_int();
}

template <typename Listener>
unsigned int run_exhaustion(asio::io_context& io, Listener& listener, unsigned short port) {
    unsigned int status = 0;
    asio::co_spawn(io, listener.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                status = co_await status_after_exhaustion(io.get_executor(), port);
            } catch (const std::exception&) {
                status = 0;
            }
            listener.close();
        },
        asio::detached);
    io.run();
    return status;
}

unsigned short free_port() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io,
                                     asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

}  // namespace

TEST(AcceptRetry, HttpServerTransportOutlivesRunningOutOfDescriptors) {
    asio::io_context io;
    mcp::HttpServerTransport transport(io.get_executor(), "127.0.0.1", 0);
    EXPECT_EQ(run_exhaustion(io, transport, transport.port()), 400u);
}

TEST(AcceptRetry, SessionManagerOutlivesRunningOutOfDescriptors) {
    asio::io_context io;
    const auto port = free_port();
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port, [](const asio::any_io_executor&) {
            return std::make_unique<mcp::Server>(mcp::Implementation{"accept", "1.0"},
                                                 mcp::ServerCapabilities{});
        });
    EXPECT_EQ(run_exhaustion(io, manager, port), 400u);
}

#endif
