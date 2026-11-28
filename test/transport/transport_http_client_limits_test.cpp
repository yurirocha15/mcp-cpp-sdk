#include "support/hang_guard.hpp"
#include "support/spawn.hpp"

#include "mcp/core/async.hpp"
#include "mcp/transport/http_client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace transport_http_client_limits_test {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

/// The limit most cases set: small enough to keep every message in kilobytes.
constexpr std::size_t g_limit = 4096;

/// The first request of a session: it travels the exchange that also carries session replacement.
const std::string g_initialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})";

/// Any other request of a session of protocol revision 2025-11-25.
const std::string g_request = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";

/// A JSON-RPC response to request @p id that is exactly @p size bytes long.
std::string response_of_size(int id, std::size_t size) {
    const std::string head = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":{"p":")";
    const std::string tail = R"("}})";
    return head + std::string(size - head.size() - tail.size(), 'x') + tail;
}

std::string http_head(const std::string& content_type, const std::string& framing) {
    return "HTTP/1.1 200 OK\r\nContent-Type: " + content_type + "\r\n" + framing + "\r\n";
}

/// A complete HTTP response whose body has a Content-Length.
std::string sized_response(const std::string& content_type, const std::string& body) {
    return http_head(content_type, "Content-Length: " + std::to_string(body.size()) + "\r\n") + body;
}

/// A complete HTTP response whose body is chunked, @p piece bytes at a time.
std::string chunked_response(const std::string& content_type, const std::string& body,
                             std::size_t piece) {
    std::string text = http_head(content_type, "Transfer-Encoding: chunked\r\n");
    for (std::size_t offset = 0; offset < body.size(); offset += piece) {
        const auto part = body.substr(offset, piece);
        char size[24];
        std::snprintf(size, sizeof(size), "%zx", part.size());
        text += std::string(size) + "\r\n" + part + "\r\n";
    }
    return text + "0\r\n\r\n";
}

/// An event stream holding one event whose data is @p data.
std::string event_of(const std::string& data) { return "data: " + data + "\n\n"; }

/// What one request and its answer came to.
struct Outcome {
    bool ok{false};
    std::string message;
    std::optional<mcp::TransportError::Kind> kind;
};

/// Answers every POST with @p raw_response, as the bytes of an HTTP response, and nothing else.
class ScriptedServer {
   public:
    ScriptedServer(asio::io_context& io, std::string raw_response)
        : io_(io),
          acceptor_(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          response_(std::make_shared<const std::string>(std::move(raw_response))) {}

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + "/mcp";
    }

    void start() { mcp_test::spawn_detached(io_, accept_loop(this)); }

    void close() {
        closing_ = true;
        beast::error_code ignored;
        (void)acceptor_.close(ignored);
        for (auto& weak : sockets_) {
            if (auto socket = weak.lock()) {
                (void)socket->close(ignored);
            }
        }
    }

   private:
    static mcp::Task<void> accept_loop(ScriptedServer* self) {
        while (!self->closing_) {
            auto socket = std::make_shared<asio::ip::tcp::socket>(self->io_);
            try {
                co_await self->acceptor_.async_accept(*socket, asio::use_awaitable);
            } catch (const std::exception&) {
                co_return;
            }
            self->sockets_.push_back(socket);
            mcp_test::spawn_detached(self->io_, serve(self->response_, socket));
        }
    }

    static mcp::Task<void> serve(std::shared_ptr<const std::string> response,
                                 std::shared_ptr<asio::ip::tcp::socket> socket) {
        const auto refusal = std::make_shared<const std::string>(
            "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n");
        try {
            for (;;) {
                beast::flat_buffer buffer;
                http::request<http::string_body> request;
                co_await http::async_read(*socket, buffer, request, asio::use_awaitable);
                const auto& answer = request.method() == http::verb::post ? response : refusal;
                co_await asio::async_write(*socket, asio::buffer(*answer), asio::use_awaitable);
            }
        } catch (const std::exception&) {
            // The client left, or its connection was reset after a refused answer.
        }
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<const std::string> response_;
    bool closing_{false};
    std::vector<std::weak_ptr<asio::ip::tcp::socket>> sockets_;
};

mcp::Task<void> send_and_receive(std::shared_ptr<mcp::HttpClientTransport> transport,
                                 std::string request, Outcome& outcome) {
    try {
        co_await transport->write_message(request);
        outcome.message = co_await transport->read_message();
        outcome.ok = true;
    } catch (const mcp::TransportError& error) {
        outcome.kind = error.kind();
        outcome.message = error.what();
    } catch (const std::exception& error) {
        outcome.message = std::string("other: ") + error.what();
    }
}

/// Sends @p request to a server that answers with @p raw_response, with the limit @p limit set
/// unless it is empty, and reports what came of it.
Outcome run_case(const std::string& request, const std::string& raw_response,
                 std::optional<std::size_t> limit) {
    asio::io_context io;
    ScriptedServer server(io, raw_response);
    auto transport = std::make_shared<mcp::HttpClientTransport>(io.get_executor(), server.url());
    if (limit) {
        transport->set_max_message_bytes(*limit);
    }
    Outcome outcome;
    asio::steady_timer guard(io);
    bool fired = false;
    guard.expires_after(g_hang_guard);
    guard.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            fired = true;
            io.stop();
        }
    });
    server.start();
    mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
        co_await send_and_receive(transport, request, outcome);
        transport->close();
        server.close();
        guard.cancel();
    });
    io.run();
    EXPECT_FALSE(fired) << "the test did not finish";
    return outcome;
}

void expect_accepted(const Outcome& outcome, const std::string& message) {
    EXPECT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.message.size(), message.size());
    EXPECT_EQ(outcome.message, message);
}

void expect_too_large(const Outcome& outcome) {
    EXPECT_FALSE(outcome.ok);
    ASSERT_TRUE(outcome.kind.has_value());
    EXPECT_EQ(*outcome.kind, mcp::TransportError::Kind::eProtocol);
}

/// The request each path of the transport is reached with: a read of a request of a session
/// (`false`) and the initialize exchange (`true`).
const std::string& request_for(bool initialize) { return initialize ? g_initialize : g_request; }

int id_for(bool initialize) { return initialize ? 1 : 2; }

class HttpClientLimits : public ::testing::TestWithParam<bool> {};

}  // namespace

TEST_P(HttpClientLimits, JsonBodyAtTheLimitIsAcceptedAndOneByteOverFails) {
    const bool initialize = GetParam();
    const auto at_limit = response_of_size(id_for(initialize), g_limit);
    expect_accepted(
        run_case(request_for(initialize), sized_response("application/json", at_limit), g_limit),
        at_limit);
    expect_too_large(
        run_case(request_for(initialize),
                 sized_response("application/json", response_of_size(id_for(initialize), g_limit + 1)),
                 g_limit));
}

TEST_P(HttpClientLimits, ChunkedJsonBodyCountsByDecodedBytes) {
    const bool initialize = GetParam();
    const auto at_limit = response_of_size(id_for(initialize), g_limit);
    expect_accepted(
        run_case(request_for(initialize), chunked_response("application/json", at_limit, 100), g_limit),
        at_limit);
    expect_too_large(run_case(
        request_for(initialize),
        chunked_response("application/json", response_of_size(id_for(initialize), g_limit + 1), 100),
        g_limit));
}

TEST(HttpClientSseLimits, SseEventAtTheLimitIsAcceptedAndOneByteOverFails) {
    const bool initialize = false;
    const auto at_limit = response_of_size(id_for(initialize), g_limit);
    expect_accepted(run_case(request_for(initialize),
                             sized_response("text/event-stream", event_of(at_limit)), g_limit),
                    at_limit);
    expect_too_large(
        run_case(request_for(initialize),
                 sized_response("text/event-stream",
                                event_of(response_of_size(id_for(initialize), g_limit + 1))),
                 g_limit));
}

TEST(HttpClientSseLimits, SseEventOfSeveralLinesCountsTheJoinedData) {
    const bool initialize = false;
    const std::string head = R"({"jsonrpc":"2.0","id":)" + std::to_string(id_for(initialize)) + ",";
    const std::string rest_start = R"("result":{"p":")";
    const std::string tail = R"("}})";
    // The data lines of the event are the head and the rest, which join with a newline between
    // them, where JSON allows white space.
    const auto rest_of_size = [&](std::size_t joined_size) {
        return rest_start +
               std::string(joined_size - head.size() - 1 - rest_start.size() - tail.size(), 'x') + tail;
    };
    const auto event_of_size = [&](std::size_t joined_size) {
        return "data: " + head + "\ndata: " + rest_of_size(joined_size) + "\n\n";
    };
    const auto joined_at_limit = head + "\n" + rest_of_size(g_limit);
    ASSERT_EQ(joined_at_limit.size(), g_limit);
    expect_accepted(run_case(request_for(initialize),
                             sized_response("text/event-stream", event_of_size(g_limit)), g_limit),
                    joined_at_limit);
    expect_too_large(run_case(request_for(initialize),
                              sized_response("text/event-stream", event_of_size(g_limit + 1)),
                              g_limit));
}

TEST(HttpClientSseLimits, SseLineAtTheLimitIsAcceptedAndOneByteOverFails) {
    const bool initialize = false;
    const auto message = response_of_size(id_for(initialize), 100);
    auto stream = [&](std::size_t comment_size) {
        return ": " + std::string(comment_size - 2, 'c') + "\n" + event_of(message);
    };
    expect_accepted(run_case(request_for(initialize),
                             sized_response("text/event-stream", stream(g_limit)), g_limit),
                    message);
    expect_too_large(run_case(request_for(initialize),
                              sized_response("text/event-stream", stream(g_limit + 1)), g_limit));
}

// An answer to the initialize exchange is read whole, so the limit holds its body, framing
// included.
TEST(HttpClientSseLimits, InitializeAnswerIsHeldToTheLimitWhole) {
    const auto event_of_body_size = [](std::size_t size) {
        return event_of(response_of_size(1, size - 8));
    };
    const auto at_limit = event_of_body_size(g_limit);
    ASSERT_EQ(at_limit.size(), g_limit);
    expect_accepted(run_case(g_initialize, sized_response("text/event-stream", at_limit), g_limit),
                    response_of_size(1, g_limit - 8));
    expect_too_large(run_case(
        g_initialize, sized_response("text/event-stream", event_of_body_size(g_limit + 1)), g_limit));
    expect_accepted(
        run_case(g_initialize, chunked_response("text/event-stream", at_limit, 100), g_limit),
        response_of_size(1, g_limit - 8));
}

TEST_P(HttpClientLimits, ALimitBelowTheDefaultStopsAMessageTheDefaultAccepts) {
    const bool initialize = GetParam();
    const auto message = response_of_size(id_for(initialize), 2 * g_limit);
    const auto raw = sized_response("application/json", message);
    expect_accepted(run_case(request_for(initialize), raw, std::nullopt), message);
    expect_too_large(run_case(request_for(initialize), raw, g_limit));
    const auto stream = sized_response("text/event-stream", event_of(message));
    expect_accepted(run_case(request_for(initialize), stream, std::nullopt), message);
    expect_too_large(run_case(request_for(initialize), stream, g_limit));
}

TEST_P(HttpClientLimits, ALimitAboveTheDefaultAcceptsALargerMessage) {
    const bool initialize = GetParam();
    constexpr std::size_t mebibyte = 1024 * 1024;
    const auto message = response_of_size(id_for(initialize), 17 * mebibyte);
    expect_accepted(
        run_case(request_for(initialize), sized_response("application/json", message), 18 * mebibyte),
        message);
}

TEST_P(HttpClientLimits, TheDefaultIs16Mebibytes) {
    const bool initialize = GetParam();
    constexpr std::size_t sixteen_mebibytes = std::size_t{16} * 1024 * 1024;
    const auto at_default = response_of_size(id_for(initialize), sixteen_mebibytes);
    expect_accepted(
        run_case(request_for(initialize), sized_response("application/json", at_default), std::nullopt),
        at_default);
    expect_too_large(run_case(
        request_for(initialize),
        sized_response("application/json", response_of_size(id_for(initialize), sixteen_mebibytes + 1)),
        std::nullopt));
}

TEST_P(HttpClientLimits, ALimitOfZeroAcceptsNoMessage) {
    const bool initialize = GetParam();
    expect_too_large(
        run_case(request_for(initialize),
                 sized_response("application/json", response_of_size(id_for(initialize), 100)), 0));
}

INSTANTIATE_TEST_SUITE_P(Paths, HttpClientLimits, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "InitializeExchange" : "SessionRead";
                         });

}  // namespace transport_http_client_limits_test
