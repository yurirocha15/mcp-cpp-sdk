#pragma once

#include <mcp/core/async.hpp>
#include <mcp/transport/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::detail {

/// What a StreamableHttpSessionManager session and its Server exchange. The manager hands the
/// server each client message as the JSON it parsed, and the server's read loop takes them in the
/// order given; each message the server writes goes straight to the manager's router, in the
/// order written. Neither direction runs a coroutine or allocates a waiter per message.
class SessionPipe final : public ITransport, public std::enable_shared_from_this<SessionPipe> {
   public:
    /// Takes each message the server writes, with the id of the client request it was written as
    /// related to, as JSON text, or null.
    using Router = std::function<void(std::string message, const std::string* related_request)>;
    /// Called once, when either side closes the pipe.
    using CloseHandler = std::function<void()>;

    /// The server's read loop runs on @p strand.
    SessionPipe(const boost::asio::any_io_executor& strand, Router router, CloseHandler on_close);
    ~SessionPipe() override = default;
    SessionPipe(const SessionPipe&) = delete;
    SessionPipe& operator=(const SessionPipe&) = delete;
    SessionPipe(SessionPipe&&) = delete;
    SessionPipe& operator=(SessionPipe&&) = delete;

    /// Queues @p message for the server. Throws TransportError (eClosed) once the pipe is closed.
    void deliver(nlohmann::json message);

    /// The next message for the server, or nothing once the pipe is closed and every message
    /// delivered before has been taken. Runs on the strand given to the constructor.
    Task<std::optional<nlohmann::json>> next_message();

    Task<std::string> read_message() override;
    Task<void> write_message(std::string_view message) override;
    Task<void> write_related_message(std::string_view message,
                                     std::string_view related_request) override;
    void close() override;

   private:
    /// Wakes the read loop when it waits. Called with mutex_ held.
    void wake_reader();

    boost::asio::any_io_executor strand_;
    boost::asio::steady_timer signal_;  // used on strand_
    Router router_;
    CloseHandler on_close_;
    std::atomic<bool> closed_{false};
    std::mutex mutex_;
    std::deque<nlohmann::json> incoming_;  // guarded by mutex_
    bool reader_waiting_{false};           // guarded by mutex_
};

}  // namespace mcp::detail
