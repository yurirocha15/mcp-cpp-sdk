#pragma once

#include "mcp/detail/net/connection.hpp"
#include "mcp/detail/transport/incoming_request.hpp"
#include "mcp/detail/transport/session_pipe.hpp"

#include <mcp/core/async.hpp>
#include <mcp/transport/sse_legacy.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {
class Server;
}  // namespace mcp

namespace mcp::detail {

/// One session of the HTTP+SSE transport: the Server that serves it, the messages it wrote that no
/// event stream has carried yet, and the principal that opened it.
class LegacySseSession {
   public:
    /// @param max_messages How many messages may wait for the stream; a session past it ends.
    /// @param max_bytes How many bytes of messages may wait for the stream; one message is always
    ///        accepted, however large.
    /// @param stream_executor The strand of the connection that writes the stream, which the
    ///        session wakes through signal().
    LegacySseSession(std::string session_id, std::string session_principal,
                     const boost::asio::any_io_executor& stream_executor, std::size_t max_messages,
                     std::size_t max_bytes);

    /// The opaque id the client names in the query of its POSTs.
    const std::string id;
    /// The principal of the request that opened the stream. Only it may POST to the session.
    const std::string principal;

    /// What the session's Server reads the client's messages from and writes its own to.
    std::shared_ptr<SessionPipe> pipe;
    /// The strand of its own the session's Server runs on.
    boost::asio::any_io_executor server_strand;

    /// Queues a message the server wrote for the stream, which it wakes. The session ends when the
    /// queue is past its limits.
    void push(std::string message);

    /// Ends the session: the stream writes what is queued, then finishes.
    void end();

    /// The timer the connection serving the stream waits on; push() and end() cancel it.
    [[nodiscard]] const std::shared_ptr<boost::asio::steady_timer>& signal() const noexcept {
        return signal_;
    }

    struct Batch {
        std::vector<std::string> messages;
        bool ended{false};
    };
    /// The queued messages, which the caller now owns, and whether the session ended.
    Batch take();

   private:
    void wake() const;

    const std::size_t max_messages_;
    const std::size_t max_bytes_;
    std::mutex mutex_;
    std::deque<std::string> queue_;
    std::size_t queued_bytes_{0};
    bool ended_{false};
    const std::shared_ptr<boost::asio::steady_timer> signal_;
};

/// The sessions of the HTTP+SSE transport of one StreamableHttpSessionManager.
class LegacySseSessions {
   public:
    explicit LegacySseSessions(LegacySseOptions options);

    [[nodiscard]] const LegacySseOptions& options() const noexcept { return options_; }

    /// What a new session is held to.
    struct Limits {
        std::size_t max_sessions{0};
        std::size_t max_messages{0};
        std::size_t max_bytes{0};
    };

    /// Opens a session whose Server will run on a strand of @p executor of its own; start() runs
    /// it. The stream is written by a connection on @p stream_executor. Null when the table holds
    /// as many sessions as @p limits allow, or once closed.
    std::shared_ptr<LegacySseSession> open(const boost::asio::any_io_executor& executor,
                                           const boost::asio::any_io_executor& stream_executor,
                                           std::string principal, const Limits& limits);

    /// Runs @p server for @p session, which open() returned.
    static void start(const std::shared_ptr<LegacySseSession>& session, std::unique_ptr<Server> server);

    /// The session named @p id, or null.
    [[nodiscard]] std::shared_ptr<LegacySseSession> find(const std::string& id) const;

    /// Ends the session named @p id and forgets it.
    void remove(const std::string& id);

    /// Ends every session; none opens afterwards.
    void close();

    [[nodiscard]] std::size_t size() const;

    /// The id the query of @p target names as `sessionId`, if it names one.
    [[nodiscard]] static std::optional<std::string> session_id_of(std::string_view target);

    /// Writes the event stream of @p session to @p stream: the `endpoint` event, then the
    /// session's messages as `message` events, with a comment every @p keepalive. Returns when the
    /// session ends or a write fails, and forgets the session. The connection does not carry
    /// another request afterwards.
    Task<void> serve(Connection& stream, const IncomingRequest& request,
                     std::shared_ptr<LegacySseSession> session, std::chrono::milliseconds keepalive,
                     const std::atomic<bool>& closed);

   private:
    LegacySseOptions options_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<LegacySseSession>> sessions_;
    bool closed_{false};
};

}  // namespace mcp::detail
