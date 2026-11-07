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

/// Sends a request that is not JSON on @p stream and returns the status of the answer.
mcp::Task<unsigned int> status_of_a_request(beast::tcp_stream& stream) {
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    request.body() = "not json";
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    // Only a guard against a hang, for a server that stopped accepting.
    stream.expires_after(std::chrono::seconds(10));
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    co_return response.result_int();
}

/// What the server answered around a moment without descriptors: the connection that waited
/// through it, and one made afterwards. A status of 0 means the exchange failed, and @c error
/// says how.
struct Answers {
    unsigned int waited{0};
    std::string waited_error;
    unsigned int afterwards{0};
    std::string afterwards_error;
};

/// Connects while the server cannot accept, frees the descriptors, then sends a request on that
/// connection and on a new one.
mcp::Task<Answers> exhaust_and_ask(asio::any_io_executor executor, unsigned short port) {
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
    Answers answers;
    beast::tcp_stream waiting(executor);
    {
        DescriptorHog hog(1);
        co_await waiting.async_connect(endpoint, asio::use_awaitable);
        // The accept loop runs into the limit in this time.
        asio::steady_timer pause(executor, std::chrono::milliseconds(50));
        co_await pause.async_wait(asio::use_awaitable);
    }
    try {
        answers.waited = co_await status_of_a_request(waiting);
    } catch (const std::exception& error) {
        answers.waited_error = error.what();
    }
    try {
        beast::tcp_stream fresh(executor);
        co_await fresh.async_connect(endpoint, asio::use_awaitable);
        answers.afterwards = co_await status_of_a_request(fresh);
    } catch (const std::exception& error) {
        answers.afterwards_error = error.what();
    }
    co_return answers;
}

template <typename Listener>
void expect_accepting_after_exhaustion(asio::io_context& io, Listener& listener, unsigned short port) {
    Answers answers;
    asio::co_spawn(io, listener.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                answers = co_await exhaust_and_ask(io.get_executor(), port);
            } catch (const std::exception& error) {
                ADD_FAILURE() << "connecting while descriptors ran out failed: " << error.what();
            }
            listener.close();
        },
        asio::detached);
    io.run();

    // The accept loop still runs: a connection made afterwards is served.
    EXPECT_EQ(answers.afterwards, 400U) << answers.afterwards_error;
#if !defined(__APPLE__)
    // The connection that waited is served too. macOS drops a pending connection that accept()
    // cannot give a descriptor, so there it is reset instead.
    EXPECT_EQ(answers.waited, 400U) << answers.waited_error;
#endif
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
    expect_accepting_after_exhaustion(io, transport, transport.port());
}

TEST(AcceptRetry, SessionManagerOutlivesRunningOutOfDescriptors) {
    asio::io_context io;
    const auto port = free_port();
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port, [](const asio::any_io_executor&) {
            return std::make_unique<mcp::Server>(mcp::Implementation{"accept", "1.0"},
                                                 mcp::ServerCapabilities{});
        });
    expect_accepting_after_exhaustion(io, manager, port);
}

#endif
