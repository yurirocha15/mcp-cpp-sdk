#include "mcp/detail/transport/session_pipe.hpp"

#include <mcp/detail/json_dump.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <utility>

namespace mcp::detail {

SessionPipe::SessionPipe(const boost::asio::any_io_executor& strand, Router router,
                         CloseHandler on_close)
    : strand_(strand), signal_(strand), router_(std::move(router)), on_close_(std::move(on_close)) {}

void SessionPipe::deliver(nlohmann::json message) {
    std::lock_guard lock(mutex_);
    if (closed_.load(std::memory_order_acquire)) {
        throw TransportError(TransportError::Kind::eClosed, "transport closed");
    }
    incoming_.push_back(std::move(message));
    wake_reader();
}

void SessionPipe::wake_reader() {
    if (!reader_waiting_) {
        return;
    }
    reader_waiting_ = false;
    // The timer belongs to the strand the read loop waits on.
    boost::asio::post(strand_, [self = shared_from_this()] { self->signal_.cancel(); });
}

Task<std::optional<nlohmann::json>> SessionPipe::next_message() {
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (!incoming_.empty()) {
                std::optional<nlohmann::json> message(std::move(incoming_.front()));
                incoming_.pop_front();
                co_return message;
            }
            if (closed_.load(std::memory_order_acquire)) {
                co_return std::nullopt;
            }
            reader_waiting_ = true;
            signal_.expires_at(std::chrono::steady_clock::time_point::max());
        }
        // A wake is posted to this strand, so it runs once this wait has started.
        boost::system::error_code ignored;
        co_await signal_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
    }
}

Task<std::string> SessionPipe::read_message() {
    auto message = co_await next_message();
    if (!message) {
        throw TransportError(TransportError::Kind::eClosed, "transport closed");
    }
    co_return mcp::detail::dump_json(*message);
}

Task<void> SessionPipe::write_message(std::string_view message) {
    if (closed_.load(std::memory_order_acquire)) {
        throw TransportError(TransportError::Kind::eClosed, "transport closed");
    }
    router_(std::string(message), nullptr);
    co_return;
}

Task<void> SessionPipe::write_related_message(std::string_view message,
                                              std::string_view related_request) {
    if (closed_.load(std::memory_order_acquire)) {
        throw TransportError(TransportError::Kind::eClosed, "transport closed");
    }
    const std::string related(related_request);
    router_(std::string(message), &related);
    co_return;
}

void SessionPipe::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        wake_reader();
    }
    if (on_close_) {
        on_close_();
    }
}

}  // namespace mcp::detail
