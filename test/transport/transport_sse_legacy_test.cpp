#include "support/hang_guard.hpp"
#include "support/spawn.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/core/constants.hpp"
#include "mcp/core/deprecation.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/sse_legacy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace transport_sse_legacy_test {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using json = nlohmann::json;
using Headers = std::vector<std::pair<std::string, std::string>>;

namespace {

/// Sets an environment variable for one test and restores the previous value.
class ScopedEnvironmentVariable {
   public:
    ScopedEnvironmentVariable(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) {
            previous_ = previous;
        }
        set(value);
    }
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;
    ~ScopedEnvironmentVariable() { set(previous_ ? previous_->c_str() : ""); }

   private:
    void set(const char* value) const {
#if defined(_WIN32)
        _putenv_s(name_, value);
#else
        if (*value == '\0') {
            ::unsetenv(name_);
        } else {
            ::setenv(name_, value, 1);
        }
#endif
    }

    const char* name_;
    std::optional<std::string> previous_;
};

// ---------------------------------------------------------------------------
// An io_context that runs on a thread of its own
// ---------------------------------------------------------------------------

class IoThread {
   public:
    IoThread() : guard_(asio::make_work_guard(io_)), thread_([this] { io_.run(); }) {}
    IoThread(const IoThread&) = delete;
    IoThread& operator=(const IoThread&) = delete;
    ~IoThread() {
        guard_.reset();
        io_.stop();
        thread_.join();
    }

    asio::io_context& context() { return io_; }
    asio::io_context::executor_type executor() { return io_.get_executor(); }

   private:
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::thread thread_;
};

/// Runs @p function on @p io and waits for it; what it throws is thrown here.
void run(IoThread& io, mcp_test::TaskFunction function) {
    auto future = mcp_test::spawn_future(io.context(), std::move(function));
    if (future.wait_for(g_hang_guard) != std::future_status::ready) {
        throw std::runtime_error("The coroutine did not finish");
    }
    future.get();
}

std::string read_one(IoThread& io, mcp::ITransport& transport) {
    auto future = mcp_test::spawn_future(io.context(), transport.read_message());
    if (future.wait_for(g_hang_guard) != std::future_status::ready) {
        throw std::runtime_error("The read did not finish");
    }
    return future.get();
}

void write_one(IoThread& io, mcp::ITransport& transport, const std::string& message) {
    run(io, [&transport, &message]() -> mcp::Task<void> { co_await transport.write_message(message); });
}

// ---------------------------------------------------------------------------
// A blocking HTTP client for the tests that look at what a server sends
// ---------------------------------------------------------------------------

/// Closes a socket when the test waits too long on it, so a hang fails instead of stalling.
class Watchdog {
   public:
    explicit Watchdog(std::function<void()> on_expiry)
        : thread_([this, expiry = std::move(on_expiry)] {
              std::unique_lock lock(mutex_);
              if (!condition_.wait_for(lock, g_hang_guard, [this] { return done_; })) {
                  expiry();
              }
          }) {}
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    ~Watchdog() {
        {
            std::scoped_lock lock(mutex_);
            done_ = true;
        }
        condition_.notify_all();
        thread_.join();
    }

   private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_{false};
    std::thread thread_;
};

struct RawResponse {
    unsigned int status{0};
    std::string body;
    std::string content_type;
};

asio::ip::tcp::socket connect_to(asio::io_context& io, unsigned short port) {
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    return socket;
}

/// One request, and the whole answer. Only for answers that end: not for an event stream.
RawResponse raw_request(unsigned short port, http::verb method, const std::string& target,
                        const std::string& body = {}, const Headers& headers = {}) {
    asio::io_context io;
    auto socket = connect_to(io, port);
    const Watchdog watchdog([&socket] {
        boost::system::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    });
    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    request.set(http::field::connection, "close");
    for (const auto& [name, value] : headers) {
        request.set(name, value);
    }
    if (!body.empty()) {
        request.set(http::field::content_type, "application/json");
        request.body() = body;
    }
    request.prepare_payload();
    http::write(socket, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return {response.result_int(), response.body(), std::string(response[http::field::content_type])};
}

struct SseEvent {
    std::string name;
    std::string data;
};

/// A GET whose answer is an event stream, read event by event.
class SseStream {
   public:
    SseStream(unsigned short port, const Headers& headers = {}, const std::string& target = "/sse")
        : socket_(connect_to(io_, port)), watchdog_([this] { shutdown(); }) {
        http::request<http::empty_body> request{http::verb::get, target, 11};
        request.set(http::field::host, "127.0.0.1:" + std::to_string(port));
        request.set(http::field::accept, "text/event-stream");
        for (const auto& [name, value] : headers) {
            request.set(name, value);
        }
        http::write(socket_, request);
        parser_.body_limit(std::numeric_limits<std::uint64_t>::max());
        http::read_header(socket_, buffer_, parser_);
        status_ = parser_.get().result_int();
        content_type_ = std::string(parser_.get()[http::field::content_type]);
    }

    [[nodiscard]] unsigned int status() const { return status_; }
    [[nodiscard]] const std::string& content_type() const { return content_type_; }

    /// The next event, skipping comments, or nothing once the stream ended.
    std::optional<SseEvent> next() {
        for (;;) {
            if (const auto end = text_.find("\n\n"); end != std::string::npos) {
                const auto block = text_.substr(0, end);
                text_.erase(0, end + 2);
                SseEvent event;
                std::istringstream lines(block);
                std::string line;
                bool any = false;
                while (std::getline(lines, line)) {
                    if (line.rfind("event: ", 0) == 0) {
                        event.name = line.substr(7);
                        any = true;
                    } else if (line.rfind("data: ", 0) == 0) {
                        event.data += (event.data.empty() ? "" : "\n") + line.substr(6);
                        any = true;
                    }
                }
                if (any) {
                    return event;
                }
                continue;
            }
            if (ended_ || parser_.is_done()) {
                return std::nullopt;
            }
            std::array<char, 4096> chunk{};
            parser_.get().body().data = chunk.data();
            parser_.get().body().size = chunk.size();
            boost::system::error_code error;
            http::read_some(socket_, buffer_, parser_, error);
            if (error && error != http::error::need_buffer) {
                ended_ = true;
            }
            text_.append(chunk.data(), chunk.size() - parser_.get().body().size);
        }
    }

    /// The target the endpoint event named.
    std::string endpoint() {
        const auto event = next();
        EXPECT_TRUE(event.has_value());
        EXPECT_EQ(event ? event->name : "", "endpoint");
        return event ? event->data : std::string();
    }

    void close() {
        boost::system::error_code ignored;
        socket_.close(ignored);
    }

   private:
    void shutdown() {
        boost::system::error_code ignored;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    }

    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    Watchdog watchdog_;
    beast::flat_buffer buffer_;
    http::response_parser<http::buffer_body> parser_;
    unsigned int status_{0};
    std::string content_type_;
    std::string text_;
    bool ended_{false};
};

// ---------------------------------------------------------------------------
// A peer that plays the server of the HTTP+SSE transport as a script says
// ---------------------------------------------------------------------------

class ScriptedPeer {
   public:
    struct Request {
        std::string method;
        std::string target;
        std::string authorization;
        std::string accept;
        std::string body;
    };

    ScriptedPeer() : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}) {}
    ScriptedPeer(const ScriptedPeer&) = delete;
    ScriptedPeer& operator=(const ScriptedPeer&) = delete;
    ~ScriptedPeer() { stop(); }

    /// The first event of the stream: `endpoint` with @p data, unless an event name is given.
    std::string first_event_name{"endpoint"};
    std::string first_event_data{"/messages?sessionId=scripted"};
    unsigned int get_status{200};
    unsigned int post_status{202};

    void start() {
        acceptor_thread_ = std::thread([this] {
            for (;;) {
                auto socket = std::make_shared<asio::ip::tcp::socket>(io_);
                boost::system::error_code error;
                acceptor_.accept(*socket, error);
                if (error || stopping_.load()) {
                    return;
                }
                std::scoped_lock lock(mutex_);
                sockets_.push_back(socket);
                connection_threads_.emplace_back([this, socket] { serve(socket); });
            }
        });
    }

    [[nodiscard]] unsigned short port() const { return acceptor_.local_endpoint().port(); }
    [[nodiscard]] std::string url(const std::string& path = "/sse") const {
        return "http://127.0.0.1:" + std::to_string(port()) + path;
    }

    [[nodiscard]] std::vector<Request> requests() const {
        std::scoped_lock lock(mutex_);
        return requests_;
    }

    [[nodiscard]] bool wait_for_requests(std::size_t count) const {
        return eventually([&] { return requests().size() >= count; });
    }

    /// Sends an event on the open stream.
    void send(const std::string& name, const std::string& data) {
        write_chunk("event: " + name + "\ndata: " + data + "\n\n");
    }

    /// Ends the stream the way a server that closes it does. The socket closes in stop(), after
    /// the thread that reads it has returned.
    void end_stream() {
        std::shared_ptr<asio::ip::tcp::socket> stream;
        {
            std::scoped_lock lock(mutex_);
            stream = std::move(stream_);
        }
        if (stream) {
            boost::system::error_code ignored;
            asio::write(*stream, asio::buffer(std::string_view("0\r\n\r\n")), ignored);
            stream->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        }
    }

    void stop() {
        boost::system::error_code ignored;
        if (acceptor_thread_.joinable()) {
            // Closing the acceptor does not end an accept that is blocked, so a connection does.
            stopping_ = true;
            asio::ip::tcp::socket waker(io_);
            waker.connect({asio::ip::make_address("127.0.0.1"), port()}, ignored);
            acceptor_thread_.join();
        }
        acceptor_.close(ignored);
        std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets;
        std::vector<std::thread> threads;
        {
            std::scoped_lock lock(mutex_);
            sockets = sockets_;
            threads.swap(connection_threads_);
        }
        // shutdown() ends a read that a connection thread is blocked in; a socket closes only
        // after the threads that use it have returned.
        for (const auto& socket : sockets) {
            socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        }
        for (auto& thread : threads) {
            thread.join();
        }
        for (const auto& socket : sockets) {
            socket->close(ignored);
        }
    }

   private:
    void write_chunk(const std::string& text) {
        std::shared_ptr<asio::ip::tcp::socket> stream;
        {
            std::scoped_lock lock(mutex_);
            stream = stream_;
        }
        if (!stream) {
            return;
        }
        std::ostringstream chunk;
        chunk << std::hex << text.size() << "\r\n" << text << "\r\n";
        boost::system::error_code ignored;
        asio::write(*stream, asio::buffer(chunk.str()), ignored);
    }

    void serve(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        beast::flat_buffer buffer;
        for (;;) {
            http::request<http::string_body> request;
            boost::system::error_code error;
            http::read(*socket, buffer, request, error);
            if (error) {
                return;
            }
            {
                std::scoped_lock lock(mutex_);
                requests_.push_back({std::string(request.method_string()),
                                     std::string(request.target()),
                                     std::string(request[http::field::authorization]),
                                     std::string(request[http::field::accept]), request.body()});
            }
            if (request.method() == http::verb::get) {
                if (get_status != 200) {
                    http::response<http::string_body> refusal{static_cast<http::status>(get_status),
                                                              11};
                    refusal.set(http::field::www_authenticate, "Bearer realm=\"scripted\"");
                    refusal.body() = "{}";
                    refusal.prepare_payload();
                    http::write(*socket, refusal, error);
                    return;
                }
                const std::string head =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n";
                asio::write(*socket, asio::buffer(head), error);
                {
                    std::scoped_lock lock(mutex_);
                    stream_ = socket;
                }
                send(first_event_name, first_event_data);
                // Waits for the client to leave, or for the test to end the stream.
                std::array<char, 64> sink{};
                while (!error) {
                    socket->read_some(asio::buffer(sink), error);
                }
                return;
            }
            http::response<http::empty_body> answer{static_cast<http::status>(post_status), 11};
            answer.keep_alive(true);
            answer.prepare_payload();
            http::write(*socket, answer, error);
            if (error) {
                return;
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread acceptor_thread_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets_;
    std::vector<std::thread> connection_threads_;
    std::vector<Request> requests_;
    std::shared_ptr<asio::ip::tcp::socket> stream_;
};

// ---------------------------------------------------------------------------
// Our own server
// ---------------------------------------------------------------------------

std::unique_ptr<mcp::Server> make_echo_server(const mcp::Executor&) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server =
        std::make_unique<mcp::Server>(mcp::Implementation{"legacy-sse-test", "1.0.0"}, std::move(caps));
    server->add_tool<json, mcp::CallToolResult>(
        "echo", "Echoes input",
        json{{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}},
        [](json params) -> mcp::Task<mcp::CallToolResult> {
            co_return mcp::make_tool_text_result(params.value("message", "empty"));
        });
    return server;
}

/// A StreamableHttpSessionManager that serves the HTTP+SSE transport on a port the system chose.
class ManagerHarness {
   public:
    using Configure = std::function<void(mcp::StreamableHttpSessionManager&)>;

    explicit ManagerHarness(const Configure& configure = {},
                            mcp::StreamableHttpSessionManager::ServerFactory factory = make_echo_server)
        : manager_(io_.executor(), "127.0.0.1", 0, std::move(factory)) {
        manager_.set_legacy_sse({});
        if (configure) {
            configure(manager_);
        }
        mcp_test::spawn_detached(io_.context(), manager_.listen());
    }
    ManagerHarness(const ManagerHarness&) = delete;
    ManagerHarness& operator=(const ManagerHarness&) = delete;
    ~ManagerHarness() { manager_.close(); }

    mcp::StreamableHttpSessionManager& manager() { return manager_; }
    [[nodiscard]] unsigned short port() const { return manager_.port(); }

   private:
    IoThread io_;
    mcp::StreamableHttpSessionManager manager_;
};

/// A Server served by run_http() with the HTTP+SSE transport on.
class ServedHarness {
   public:
    explicit ServedHarness(std::function<void(mcp::Server&)> setup = {})
        : server_("served-sse", "1.0.0") {
        server_.add_tool<json, mcp::CallToolResult>(
            "echo", "Echoes input",
            json{{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}},
            [](json params) -> mcp::Task<mcp::CallToolResult> {
                co_return mcp::make_tool_text_result(params.value("message", "empty"));
            });
        if (setup) {
            setup(server_);
        }
        thread_ = std::thread([this] {
            server_.run_http("127.0.0.1", 0,
                             [](mcp::StreamableHttpSessionManager& http) { http.set_legacy_sse({}); });
        });
        EXPECT_TRUE(eventually([this] { return server_.bound_port() != 0; }));
    }
    ServedHarness(const ServedHarness&) = delete;
    ServedHarness& operator=(const ServedHarness&) = delete;
    ~ServedHarness() {
        server_.stop();
        thread_.join();
    }

    [[nodiscard]] unsigned short port() const { return server_.bound_port(); }
    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port()) + "/sse";
    }

   private:
    mcp::Server server_;
    std::thread thread_;
};

json initialize_request(int id, const std::string& version = "2025-11-25") {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", version},
              {"clientInfo", {{"name", "raw-client"}, {"version", "1.0.0"}}},
              {"capabilities", json::object()}}}};
}

struct Recorded {
    std::mutex mutex;
    std::vector<mcp::DeprecatedFeature> features;

    mcp::DeprecationHandler handler() {
        return [this](const mcp::DeprecationNotice& notice) {
            std::scoped_lock lock(mutex);
            features.push_back(notice.feature);
        };
    }

    std::vector<mcp::DeprecatedFeature> snapshot() {
        std::scoped_lock lock(mutex);
        return features;
    }
};

std::string message_text(const mcp::CallToolResult& result) {
    return json(result).at("content").at(0).at("text").get<std::string>();
}

}  // namespace

/// Without a handler, a notice goes to standard error once per client and per server; the tests
/// that look at the notices install handlers, and the others keep the output quiet.
class LegacySseTransportTest : public ::testing::Test {
   protected:
    const ScopedEnvironmentVariable silence_{"MCP_SILENCE_DEPRECATION_NOTICES", "1"};
};

// ===========================================================================
// The client against a scripted peer
// ===========================================================================

TEST_F(LegacySseTransportTest, ClientOpensTheStreamPostsToTheEndpointAndReadsMessageEvents) {
    IoThread io;
    ScriptedPeer peer;
    peer.start();
    auto transport = std::make_shared<mcp::LegacySseClientTransport>(io.executor(), peer.url());

    write_one(io, *transport, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    peer.send("message", R"({"jsonrpc":"2.0","id":1,"result":{}})");
    EXPECT_EQ(read_one(io, *transport), R"({"jsonrpc":"2.0","id":1,"result":{}})");

    ASSERT_TRUE(peer.wait_for_requests(2));
    const auto requests = peer.requests();
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[0].target, "/sse");
    EXPECT_EQ(requests[0].accept, "text/event-stream");
    EXPECT_EQ(requests[1].method, "POST");
    EXPECT_EQ(requests[1].target, "/messages?sessionId=scripted");
    EXPECT_EQ(requests[1].body, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");

    // Events of other names are not messages.
    peer.send("ping", "ignored");
    peer.send("message", "second");
    EXPECT_EQ(read_one(io, *transport), "second");
    transport->close();
}

TEST_F(LegacySseTransportTest, ClientResolvesTheEndpointAgainstTheStreamUrl) {
    struct Case {
        std::string stream_path;
        std::string endpoint;
        std::string expected_target;
    };
    for (const auto& item : std::vector<Case>{
             {"/sse", "/post?x=1", "/post?x=1"},
             {"/app/sse", "post?x=1", "/app/post?x=1"},
             {"/app/v1/sse", "../post", "/app/post"},
             {"/app/sse?token=a", "?sessionId=b", "/app/sse?sessionId=b"},
             {"/sse", "/post#fragment", "/post"},
         }) {
        IoThread io;
        ScriptedPeer peer;
        peer.first_event_data = item.endpoint;
        peer.start();
        mcp::LegacySseClientTransport transport(io.executor(), peer.url(item.stream_path));
        write_one(io, transport, "{}");
        ASSERT_TRUE(peer.wait_for_requests(2)) << item.endpoint;
        EXPECT_EQ(peer.requests()[1].target, item.expected_target) << item.endpoint;
    }
}

TEST_F(LegacySseTransportTest, ClientAcceptsAnAbsoluteEndpointOfTheSameOrigin) {
    IoThread io;
    ScriptedPeer peer;
    peer.first_event_data = "http://127.0.0.1:" + std::to_string(peer.port()) + "/absolute?id=1";
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    write_one(io, transport, "{}");
    ASSERT_TRUE(peer.wait_for_requests(2));
    EXPECT_EQ(peer.requests()[1].target, "/absolute?id=1");
}

TEST_F(LegacySseTransportTest, ClientRefusesAnEndpointOfAnotherOrigin) {
    // Each endpoint is made from the port of the peer that sends it.
    const std::vector<std::function<std::string(unsigned short)>> endpoints{
        [](unsigned short) { return std::string("http://evil.example/messages"); },
        [](unsigned short port) {
            return "http://127.0.0.1:" + std::to_string(port + 1) + "/messages";
        },
        [](unsigned short port) { return "https://127.0.0.1:" + std::to_string(port) + "/messages"; },
        [](unsigned short) { return std::string("//evil.example/messages"); },
        [](unsigned short port) { return "ftp://127.0.0.1:" + std::to_string(port) + "/messages"; },
        [](unsigned short port) {
            return "http://user@127.0.0.1:" + std::to_string(port) + "/messages";
        },
        [](unsigned short) { return std::string("   "); },
    };
    for (const auto& make_endpoint : endpoints) {
        IoThread io;
        ScriptedPeer peer;
        const auto endpoint = make_endpoint(peer.port());
        peer.first_event_data = endpoint;
        peer.start();
        mcp::LegacySseClientTransport transport(io.executor(), peer.url());
        try {
            write_one(io, transport, "{}");
            ADD_FAILURE() << "accepted " << endpoint;
        } catch (const mcp::TransportError& error) {
            EXPECT_EQ(error.kind(), mcp::TransportError::Kind::eProtocol) << endpoint;
        }
        // Nothing was posted: the stream's GET is the only request.
        EXPECT_EQ(peer.requests().size(), 1U) << endpoint;
    }
}

TEST_F(LegacySseTransportTest, ClientDoesNotSendARequestOfRevision20260728) {
    IoThread io;
    ScriptedPeer peer;
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    try {
        write_one(io, transport, R"({"jsonrpc":"2.0","id":1,"method":"server/discover"})");
        FAIL() << "the probe was sent";
    } catch (const mcp::TransportError& error) {
        EXPECT_EQ(error.kind(), mcp::TransportError::Kind::eProtocol);
    }
    EXPECT_TRUE(peer.requests().empty());
    write_one(io, transport, R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
    ASSERT_TRUE(peer.wait_for_requests(2));
}

TEST_F(LegacySseTransportTest, ClientRefusesAStreamWhoseFirstEventIsNotTheEndpoint) {
    IoThread io;
    ScriptedPeer peer;
    peer.first_event_name = "message";
    peer.first_event_data = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    try {
        write_one(io, transport, "{}");
        FAIL() << "the write was accepted";
    } catch (const mcp::TransportError& error) {
        EXPECT_EQ(error.kind(), mcp::TransportError::Kind::eProtocol);
    }
    EXPECT_EQ(peer.requests().size(), 1U);
}

TEST_F(LegacySseTransportTest, TheStreamEndingClosesTheTransport) {
    IoThread io;
    ScriptedPeer peer;
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    write_one(io, transport, "{}");
    peer.send("message", "last");
    peer.end_stream();

    // What arrived before the end is still read; then the transport reports it is closed.
    EXPECT_EQ(read_one(io, transport), "last");
    try {
        static_cast<void>(read_one(io, transport));
        FAIL() << "a read succeeded on a transport whose stream ended";
    } catch (const mcp::TransportError& error) {
        EXPECT_EQ(error.kind(), mcp::TransportError::Kind::eClosed);
    }
    try {
        write_one(io, transport, "{}");
        FAIL() << "a write succeeded on a transport whose stream ended";
    } catch (const mcp::TransportError& error) {
        EXPECT_EQ(error.kind(), mcp::TransportError::Kind::eClosed);
    }
}

TEST_F(LegacySseTransportTest, ClientSendsTheBearerTokenOnTheStreamAndOnEveryPost) {
    IoThread io;
    ScriptedPeer peer;
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    transport.set_bearer_token_provider([] { return std::string("secret"); });
    write_one(io, transport, "{}");
    write_one(io, transport, "{}");
    ASSERT_TRUE(peer.wait_for_requests(3));
    for (const auto& request : peer.requests()) {
        EXPECT_EQ(request.authorization, "Bearer secret") << request.method;
    }
}

TEST_F(LegacySseTransportTest, AnErrorStatusOfTheStreamIsReportedWithItsStatus) {
    IoThread io;
    ScriptedPeer peer;
    peer.get_status = 401;
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    try {
        write_one(io, transport, "{}");
        FAIL() << "the write was accepted";
    } catch (const mcp::HttpStatusError& error) {
        EXPECT_EQ(error.status(), 401U);
        EXPECT_EQ(error.authenticate_challenge(), "Bearer realm=\"scripted\"");
    }
}

TEST_F(LegacySseTransportTest, AnErrorStatusOfAPostIsReportedWithItsStatus) {
    IoThread io;
    ScriptedPeer peer;
    peer.post_status = 500;
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    try {
        write_one(io, transport, "{}");
        FAIL() << "the write was accepted";
    } catch (const mcp::HttpStatusError& error) {
        EXPECT_EQ(error.status(), 500U);
    }
}

TEST_F(LegacySseTransportTest, ClientRefusesBadUrlsAndSettingsThatDoNotApply) {
    IoThread io;
    EXPECT_THROW(mcp::LegacySseClientTransport(io.executor(), "ftp://127.0.0.1/sse"),
                 std::invalid_argument);
    EXPECT_THROW(mcp::LegacySseClientTransport(io.executor(), "http://user:pw@127.0.0.1/sse"),
                 std::invalid_argument);
    mcp::LegacySseClientTransport plain(io.executor(), "http://127.0.0.1:9/sse");
    EXPECT_THROW(plain.set_server_name("name"), std::invalid_argument);
    EXPECT_THROW(plain.set_tls(std::shared_ptr<const mcp::TlsClientContext>{}), std::invalid_argument);
}

TEST_F(LegacySseTransportTest, ClosingTheTransportFailsALaterWrite) {
    IoThread io;
    ScriptedPeer peer;
    peer.start();
    mcp::LegacySseClientTransport transport(io.executor(), peer.url());
    write_one(io, transport, "{}");
    transport.close();
    try {
        write_one(io, transport, "{}");
        FAIL() << "a write succeeded on a closed transport";
    } catch (const mcp::TransportError& error) {
        EXPECT_EQ(error.kind(), mcp::TransportError::Kind::eClosed);
    }
}

// ===========================================================================
// The client and the server together
// ===========================================================================

TEST_F(LegacySseTransportTest, ClientAndServerExchangeMessagesEndToEnd) {
    ManagerHarness harness;
    mcp::Client client(
        mcp::Endpoint::legacy_sse("http://127.0.0.1:" + std::to_string(harness.port()) + "/sse"));
    const auto connected = client.sync().connect("legacy-sse-client", "1.0.0");
    EXPECT_EQ(connected.protocolVersion, "2025-11-25");
    const auto result = client.sync().call_tool("echo", json{{"message", "hello"}});
    EXPECT_EQ(message_text(result), "hello");
    EXPECT_EQ(harness.manager().session_count(), 1U);
}

TEST_F(LegacySseTransportTest, ClientAndServerUseConfiguredPaths) {
    ManagerHarness harness([](mcp::StreamableHttpSessionManager& manager) {
        manager.set_legacy_sse({.sse_path = "/events", .message_path = "/inbox"});
    });
    SseStream stream(harness.port(), {}, "/events");
    EXPECT_EQ(stream.status(), 200U);
    EXPECT_EQ(stream.content_type(), "text/event-stream");
    const auto endpoint = stream.endpoint();
    EXPECT_EQ(endpoint.rfind("/inbox?sessionId=", 0), 0U) << endpoint;
    // The default path is not a stream: the Streamable HTTP endpoint answers it.
    EXPECT_NE(raw_request(harness.port(), http::verb::get, "/sse").content_type, "text/event-stream");

    mcp::Client client(
        mcp::Endpoint::legacy_sse("http://127.0.0.1:" + std::to_string(harness.port()) + "/events"));
    client.sync().connect("legacy-sse-client", "1.0.0");
    EXPECT_EQ(message_text(client.sync().call_tool("echo", json{{"message", "paths"}})), "paths");
}

TEST_F(LegacySseTransportTest, TheSessionIdIsOpaqueAndDiffersPerStream) {
    ManagerHarness harness;
    SseStream first(harness.port());
    SseStream second(harness.port());
    const auto first_endpoint = first.endpoint();
    const auto second_endpoint = second.endpoint();
    EXPECT_NE(first_endpoint, second_endpoint);
    const auto id = first_endpoint.substr(first_endpoint.find('=') + 1);
    EXPECT_GE(id.size(), 16U);
    EXPECT_EQ(id.find_first_not_of("0123456789abcdef"), std::string::npos) << id;
}

TEST_F(LegacySseTransportTest, InitializeNegotiatesAtMost20251125) {
    ManagerHarness harness;
    SseStream stream(harness.port());
    const auto endpoint = stream.endpoint();
    const auto posted = raw_request(harness.port(), http::verb::post, endpoint,
                                    initialize_request(1, "2026-07-28").dump());
    EXPECT_EQ(posted.status, 202U);
    const auto event = stream.next();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->name, "message");
    const auto response = json::parse(event->data);
    EXPECT_EQ(response.at("id"), 1);
    EXPECT_EQ(response.at("result").at("protocolVersion"), "2025-11-25");
}

TEST_F(LegacySseTransportTest, ARequestOfRevision20260728IsRefused) {
    ManagerHarness harness;
    SseStream stream(harness.port());
    const auto endpoint = stream.endpoint();
    const json request = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/list"}};
    for (const Headers& headers : {Headers{{"MCP-Protocol-Version", "2026-07-28"}}, Headers{}}) {
        const auto body = headers.empty()
                              ? json{{"jsonrpc", "2.0"}, {"id", 8}, {"method", "server/discover"}}
                              : request;
        const auto refused =
            raw_request(harness.port(), http::verb::post, endpoint, body.dump(), headers);
        EXPECT_EQ(refused.status, 400U) << refused.body;
        EXPECT_EQ(json::parse(refused.body).at("error").at("code"),
                  mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION);
    }
}

TEST_F(LegacySseTransportTest, OriginAndHostAreCheckedLikeTheStreamableEndpoint) {
    ManagerHarness harness([](mcp::StreamableHttpSessionManager& manager) {
        manager.set_allowed_origins({"http://app.example"});
    });
    const auto refused =
        raw_request(harness.port(), http::verb::get, "/sse", {}, {{"Origin", "http://evil.example"}});
    EXPECT_EQ(refused.status, 403U);

    const auto wrong_host =
        raw_request(harness.port(), http::verb::get, "/sse", {}, {{"Host", "evil.example"}});
    EXPECT_EQ(wrong_host.status, 403U);

    SseStream allowed(harness.port(), {{"Origin", "http://app.example"}});
    EXPECT_EQ(allowed.status(), 200U);
    const auto endpoint = allowed.endpoint();

    const json ping = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}};
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint, ping.dump(),
                          {{"Origin", "http://evil.example"}})
                  .status,
              403U);
    EXPECT_EQ(
        raw_request(harness.port(), http::verb::post, endpoint, ping.dump(), {{"Host", "evil.example"}})
            .status,
        403U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint, ping.dump(),
                          {{"Origin", "http://app.example"}})
                  .status,
              202U);
}

TEST_F(LegacySseTransportTest, TheBearerTokenIsCheckedOnTheStreamAndOnPosts) {
    ManagerHarness harness([](mcp::StreamableHttpSessionManager& manager) {
        manager.set_bearer_token_validator(
            [](std::string_view token) -> mcp::TokenValidation { return {token == "right", "alice"}; });
    });
    const auto without = raw_request(harness.port(), http::verb::get, "/sse");
    EXPECT_EQ(without.status, 401U);
    const auto wrong =
        raw_request(harness.port(), http::verb::get, "/sse", {}, {{"Authorization", "Bearer wrong"}});
    EXPECT_EQ(wrong.status, 401U);

    SseStream stream(harness.port(), {{"Authorization", "Bearer right"}});
    EXPECT_EQ(stream.status(), 200U);
    const auto endpoint = stream.endpoint();
    const json ping = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}};
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint, ping.dump()).status, 401U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint, ping.dump(),
                          {{"Authorization", "Bearer right"}})
                  .status,
              202U);
}

TEST_F(LegacySseTransportTest, ASessionServesOnlyThePrincipalThatOpenedIt) {
    ManagerHarness harness([](mcp::StreamableHttpSessionManager& manager) {
        manager.set_bearer_token_validator([](std::string_view token) -> mcp::TokenValidation {
            if (token == "alice-token") {
                return {true, "alice"};
            }
            if (token == "bob-token") {
                return {true, "bob"};
            }
            return {false};
        });
    });
    SseStream alice(harness.port(), {{"Authorization", "Bearer alice-token"}});
    const auto endpoint = alice.endpoint();
    const json ping = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}};

    const auto foreign = raw_request(harness.port(), http::verb::post, endpoint, ping.dump(),
                                     {{"Authorization", "Bearer bob-token"}});
    const auto guessed = raw_request(harness.port(), http::verb::post,
                                     "/messages?sessionId=0123456789abcdef0123456789abcdef",
                                     ping.dump(), {{"Authorization", "Bearer bob-token"}});
    EXPECT_EQ(foreign.status, 404U);
    EXPECT_EQ(guessed.status, 404U);
    // The two refusals are the same answer, so the id does not show whether it exists.
    EXPECT_EQ(foreign.body, guessed.body);
    EXPECT_EQ(foreign.content_type, guessed.content_type);

    EXPECT_EQ(raw_request(harness.port(), http::verb::post, "/messages", ping.dump(),
                          {{"Authorization", "Bearer alice-token"}})
                  .status,
              400U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint, ping.dump(),
                          {{"Authorization", "Bearer alice-token"}})
                  .status,
              202U);
    const auto event = alice.next();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(json::parse(event->data).at("id"), 1);
}

TEST_F(LegacySseTransportTest, AnUnknownSessionIsRefusedWithoutAValidator) {
    ManagerHarness harness;
    const json ping = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}};
    EXPECT_EQ(raw_request(harness.port(), http::verb::post,
                          "/messages?sessionId=ffffffffffffffffffffffffffffffff", ping.dump())
                  .status,
              404U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::get, "/messages").status, 405U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, "/sse", ping.dump()).status, 405U);
}

TEST_F(LegacySseTransportTest, MalformedPostsAreRefused) {
    ManagerHarness harness;
    SseStream stream(harness.port());
    const auto endpoint = stream.endpoint();
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint, "not json").status, 400U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint, "[1]").status, 400U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::post, endpoint,
                          R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})")
                  .status,
              400U);
}

TEST_F(LegacySseTransportTest, SessionLimitsApplyToTheStreams) {
    ManagerHarness harness([](mcp::StreamableHttpSessionManager& manager) {
        mcp::SessionLimits limits;
        limits.max_sessions = 1;
        manager.set_session_limits(limits);
    });
    SseStream first(harness.port());
    EXPECT_EQ(first.status(), 200U);
    EXPECT_EQ(harness.manager().session_count(), 1U);
    EXPECT_EQ(raw_request(harness.port(), http::verb::get, "/sse").status, 503U);
}

// SessionLimits::max_sessions bounds the sessions of both transports together, whichever opens
// first.
TEST_F(LegacySseTransportTest, SessionLimitsCountTheSessionsOfBothTransports) {
    const auto limit_to_one = [](mcp::StreamableHttpSessionManager& manager) {
        mcp::SessionLimits limits;
        limits.max_sessions = 1;
        manager.set_session_limits(limits);
    };
    const Headers streamable{{"Accept", "application/json, text/event-stream"}};
    {
        ManagerHarness harness(limit_to_one);
        SseStream stream(harness.port());
        EXPECT_EQ(stream.status(), 200U);
        EXPECT_EQ(raw_request(harness.port(), http::verb::post, "/mcp", initialize_request(1).dump(),
                              streamable)
                      .status,
                  503U);
        EXPECT_EQ(harness.manager().session_count(), 1U);
    }
    {
        ManagerHarness harness(limit_to_one);
        EXPECT_EQ(raw_request(harness.port(), http::verb::post, "/mcp", initialize_request(1).dump(),
                              streamable)
                      .status,
                  200U);
        EXPECT_EQ(raw_request(harness.port(), http::verb::get, "/sse").status, 503U);
        EXPECT_EQ(harness.manager().session_count(), 1U);
    }
}

TEST_F(LegacySseTransportTest, ASessionEndsWhenItsClientLeaves) {
    ManagerHarness harness([](mcp::StreamableHttpSessionManager& manager) {
        mcp::StreamOptions options;
        options.keepalive_interval = std::chrono::milliseconds(20);
        manager.set_stream_options(options);
    });
    {
        SseStream stream(harness.port());
        static_cast<void>(stream.endpoint());
        EXPECT_EQ(harness.manager().session_count(), 1U);
        stream.close();
    }
    // The next comment the server writes finds the connection closed.
    EXPECT_TRUE(eventually([&] { return harness.manager().session_count() == 0; }));
}

TEST_F(LegacySseTransportTest, ClosingTheManagerEndsTheStreams) {
    ManagerHarness harness;
    SseStream stream(harness.port());
    static_cast<void>(stream.endpoint());
    harness.manager().close();
    EXPECT_FALSE(stream.next().has_value());
    EXPECT_EQ(harness.manager().session_count(), 0U);
}

TEST_F(LegacySseTransportTest, SetLegacySseValidatesItsArgumentsAndTheManagersState) {
    IoThread io;
    mcp::StreamableHttpSessionManager manager(io.executor(), "127.0.0.1", 0, make_echo_server);
    EXPECT_THROW(manager.set_legacy_sse({.sse_path = "sse", .message_path = "/messages"}),
                 std::invalid_argument);
    EXPECT_THROW(manager.set_legacy_sse({.sse_path = "/sse?x=1", .message_path = "/messages"}),
                 std::invalid_argument);
    EXPECT_THROW(manager.set_legacy_sse({.sse_path = "/same", .message_path = "/same"}),
                 std::invalid_argument);
    EXPECT_NO_THROW(manager.set_legacy_sse({}));
    manager.close();
    EXPECT_THROW(manager.set_legacy_sse({}), std::logic_error);
}

TEST_F(LegacySseTransportTest, WithoutSetLegacySseTheEndpointIsNotServed) {
    IoThread io;
    mcp::StreamableHttpSessionManager manager(io.executor(), "127.0.0.1", 0, make_echo_server);
    mcp_test::spawn_detached(io.context(), manager.listen());
    // Not served as a stream: Streamable HTTP has no session to resume here.
    EXPECT_EQ(raw_request(manager.port(), http::verb::get, "/sse").status, 400U);
    manager.close();
}

// ===========================================================================
// The deprecation notice
// ===========================================================================

TEST_F(LegacySseTransportTest, ReportsTheDeprecationOnceOnEachSide) {
    Recorded server_side;
    ManagerHarness harness({}, [&server_side](const mcp::Executor&) -> std::unique_ptr<mcp::Server> {
        auto server = make_echo_server({});
        server->set_deprecation_handler(server_side.handler());
        return server;
    });

    Recorded client_side;
    mcp::ClientOptions options;
    options.deprecation_handler = client_side.handler();
    mcp::Client client(
        mcp::Endpoint::legacy_sse("http://127.0.0.1:" + std::to_string(harness.port()) + "/sse"),
        options);
    client.sync().connect("legacy-sse-client", "1.0.0");
    EXPECT_EQ(message_text(client.sync().call_tool("echo", json{{"message", "once"}})), "once");
    client.sync().list_tools();

    const std::vector<mcp::DeprecatedFeature> expected{mcp::DeprecatedFeature::eHttpSse};
    EXPECT_EQ(client_side.snapshot(), expected);
    EXPECT_EQ(server_side.snapshot(), expected);

    const auto& notice = mcp::deprecation_notice(mcp::DeprecatedFeature::eHttpSse);
    EXPECT_EQ(notice.proposal, "SEP-2596");
    EXPECT_EQ(notice.deprecated_in, "2025-03-26");
    EXPECT_EQ(notice.earliest_removal, "three months after SEP-2596 reaches Final");
    EXPECT_EQ(notice.migration, "Use Streamable HTTP.");
}

TEST_F(LegacySseTransportTest, ServersOfRunHttpShareOneReport) {
    Recorded server_side;
    ServedHarness harness(
        [&server_side](mcp::Server& server) { server.set_deprecation_handler(server_side.handler()); });
    for (int index = 0; index < 2; ++index) {
        mcp::Client client(mcp::Endpoint::legacy_sse(harness.url()));
        client.sync().connect("legacy-sse-client", "1.0.0");
        EXPECT_EQ(message_text(client.sync().call_tool("echo", json{{"message", "served"}})), "served");
    }
    const std::vector<mcp::DeprecatedFeature> expected{mcp::DeprecatedFeature::eHttpSse};
    EXPECT_EQ(server_side.snapshot(), expected);
}

}  // namespace transport_sse_legacy_test
