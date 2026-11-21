#pragma once

#include "../detail/http_limits.hpp"

#include <mcp/core/async.hpp>
#include <mcp/server/request_context.hpp>
#include <mcp/transport/http_beast.hpp>
#include <mcp/transport/http_types.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp::detail {

using StreamConnection = boost::beast::tcp_stream;

/// What the streams of one HTTP server transport have done, as StreamStats reports it.
struct StreamCounters {
    std::atomic<std::uint64_t> streams_opened{0};
    std::atomic<std::uint64_t> disconnect_cancels{0};
    std::atomic<std::uint64_t> half_close_probes{0};
    std::atomic<std::uint64_t> dropped_notifications{0};

    [[nodiscard]] StreamStats snapshot() const {
        return {streams_opened.load(std::memory_order_relaxed),
                disconnect_cancels.load(std::memory_order_relaxed),
                half_close_probes.load(std::memory_order_relaxed),
                dropped_notifications.load(std::memory_order_relaxed)};
    }
};

inline void check_stream_options(const StreamOptions& options) {
    if (options.keepalive_interval.count() <= 0 || options.disconnect_probe_interval.count() <= 0) {
        throw std::invalid_argument("Stream intervals must be positive");
    }
    if (options.max_queued_notifications == 0) {
        throw std::invalid_argument("Stream notification queue limit must be at least 1");
    }
    if (options.max_queued_bytes == 0) {
        throw std::invalid_argument("Stream notification byte limit must be at least 1");
    }
}

/// The answer to a 2026-07-28 request while it is produced: the notifications its handler sends
/// and, once the server has answered, the response. Filled from the dispatch executor and read on
/// the connection's strand, which owns `signal`.
struct ModernExchange : std::enable_shared_from_this<ModernExchange> {
    /// @p budget, when not null, is the connection memory budget, of which the bytes of queued
    /// notifications take a share up to @p budget_limit.
    ModernExchange(boost::asio::any_io_executor strand_arg, const StreamOptions& options,
                   bool streams_arg, std::atomic<std::size_t>* budget, std::size_t budget_limit);

    /// Wakes the connection; posted because `signal` and `woken` belong to the strand.
    void wake();

    /// The connection may be busy writing rather than waiting, when cancelling the timer does
    /// nothing; `woken` keeps the wake for its next wait.
    void wake_on_strand();

    /// Queues a notification for the stream, or returns false when it would take the queue past
    /// its count or byte limit, or past the memory budget. Once the queue has ended the
    /// notification is discarded and true is returned.
    bool push_notification(std::string message);

    /// Gives back the bytes of notifications taken from the queue once they are written.
    void release_queued(std::size_t bytes);

    /// Ends the queue, giving back what it holds of the memory budget. The request may still be
    /// running, and what it sends afterwards is discarded. Called before the budget goes away.
    void end_queue();

    /// Cancels the request and ends the queue, for an exchange that is not served.
    void abandon();

    /// Cancels the request and wakes a handler that waits for its cancellation.
    void cancel();

    /// Registers the function to call after the request is cancelled. The caller reads
    /// `cancelled` once registered, since a cancellation before it did not call the function.
    void set_on_cancel(std::function<void()> hook);

    void finish(std::string response_wire);

    boost::asio::any_io_executor strand;
    boost::asio::steady_timer signal;
    bool woken{false};
    const std::size_t max_queued;
    const std::size_t max_queued_bytes;
    /// Whether the answer may become an SSE stream: the client accepts one over HTTP/1.1.
    const bool streams;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);

    std::mutex mutex;
    /// Empty, it holds no memory, as for most requests, which send no notification. Guarded by
    /// mutex.
    std::vector<std::string> notifications;
    std::size_t queued_bytes{0};              // guarded by mutex
    bool queue_ended{false};                  // guarded by mutex
    std::optional<BudgetLease> budget_lease;  // guarded by mutex
    std::optional<std::string> response;      // guarded by mutex
    std::function<void()> on_cancel;          // guarded by mutex
};

/// Serves a request of 2026-07-28 and returns its serialized response.
using ModernDispatch = std::function<Task<std::string>(nlohmann::json, RequestContext)>;

/// A call to a dispatch function that does not own it, so a caller with a lambda at hand allocates
/// nothing. The function must outlive the call that takes the reference.
class ModernDispatchRef {
   public:
    template <typename Function>
    ModernDispatchRef(const Function& function)  // NOLINT(google-explicit-constructor)
        : function_(&function),
          call_([](const void* target, nlohmann::json json, RequestContext context) {
              return (*static_cast<const Function*>(target))(std::move(json), std::move(context));
          }) {}

    Task<std::string> operator()(nlohmann::json json, RequestContext context) const {
        return call_(function_, std::move(json), std::move(context));
    }

   private:
    const void* function_;
    Task<std::string> (*call_)(const void*, nlohmann::json, RequestContext);
};

/// What serving the exchanges of a transport needs from it.
struct ModernServing {
    const std::atomic<bool>& closed;
    const StreamOptions& options;
    StreamCounters& counters;
    std::atomic<std::uint64_t>& header_mismatch_rejections;
    /// Bounds how long the peer may take to read what is written to it.
    const HttpConnectionLimits& limits;
};

/// What starting an exchange needs besides the request.
struct ModernStart {
    /// The connection's strand, which owns the exchange's wake-up timer.
    boost::asio::any_io_executor strand;
    /// Where the server answers.
    boost::asio::any_io_executor dispatch_executor;
    const StreamOptions& options;
    std::shared_ptr<StreamCounters> counters;
    bool json_only{false};
    /// The connection memory budget the bytes of queued notifications are charged to, and its limit.
    std::atomic<std::size_t>* budget{nullptr};
    std::size_t budget_limit{0};
    /// Whether the request's `Mcp-Param-*` headers go to the server to check.
    bool check_headers{true};
};

/// Dispatches the 2026-07-28 request @p request_json, sent in @p request by @p principal, on the
/// executor of @p start, and returns the exchange the connection writes the answer from. The
/// answer becomes an SSE stream only if the client accepts one, the transport is not JSON-only and
/// a notification arrives before the response.
std::shared_ptr<ModernExchange> start_modern_exchange(const StringRequest& request,
                                                      nlohmann::json request_json,
                                                      std::string principal, const ModernStart& start,
                                                      ModernDispatchRef dispatch);

/// Writes the answer to a 2026-07-28 request as it is produced. Plain JSON, with the status the
/// specification gives its error, unless a notification arrives first and the client accepts SSE:
/// then an SSE stream carrying the notifications and finally the response. Meanwhile one read on
/// the connection watches the client. Bytes it reads are the client's next request and stay in
/// @p buffer; a reset, or a failed write, means the client has gone and cancels the request. A
/// half-close is not a disconnect, since the client may still be reading: the stream is opened and
/// probed until a write fails or the answer is complete. Without a stream the socket is checked for
/// a reset at the same interval. A write the peer does not take within the request timeout of the
/// connection limits ends the connection.
///
/// @p client_finished_sending is set once the watch has read the client's end of the stream.
///
/// @return Whether the connection may carry another request.
Task<bool> serve_modern_exchange(StreamConnection& stream, boost::beast::flat_buffer& buffer,
                                 const StringRequest& request, std::shared_ptr<ModernExchange> exchange,
                                 bool& client_finished_sending, const ModernServing& serving,
                                 std::vector<char>& response_head);

}  // namespace mcp::detail
