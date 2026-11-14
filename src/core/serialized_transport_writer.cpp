#include <mcp/detail/serialized_transport_writer.hpp>

#include "../detail/strand_executor.hpp"

#include <mcp/transport/transport.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mcp::detail {

namespace {

/// What a queued message counts in queued_bytes().
std::size_t queued_cost(const std::string& message) {
    return message.size() + g_queued_message_overhead;
}

}  // namespace

struct SerializedTransportWriterState {
    struct Operation {
        Operation(const boost::asio::any_io_executor& executor,
                  std::shared_ptr<const std::string> owned_message,
                  std::shared_ptr<const std::atomic<bool>> cancellation)
            : message(std::move(owned_message)),
              cancel_before_start(std::move(cancellation)),
              completion(executor) {
            completion.expires_at(std::chrono::steady_clock::time_point::max());
        }

        /// Null for a marker that completes once everything queued before it is written.
        std::shared_ptr<const std::string> message;
        std::shared_ptr<const std::atomic<bool>> cancel_before_start;
        /// The id of the client request the message belongs to, as JSON text, or null.
        std::shared_ptr<const std::string> related_request;
        boost::asio::steady_timer completion;
        std::exception_ptr error;
        bool completed{false};
    };

    SerializedTransportWriterState(std::shared_ptr<ITransport> owned_transport,
                                   const boost::asio::any_io_executor& executor)
        : transport(std::move(owned_transport)), strand(mcp::detail::make_strand_executor(executor)) {}

    static void complete(const std::shared_ptr<Operation>& operation,
                         std::exception_ptr error = nullptr) {
        operation->error = std::move(error);
        operation->completed = true;

        operation->completion.expires_at(std::chrono::steady_clock::time_point::min());
    }

    static Task<void> drain(std::shared_ptr<SerializedTransportWriterState> state) {
        while (!state->queue.empty()) {
            auto operation = state->queue.front();
            if (!operation->message) {
                state->queue.pop_front();
                complete(operation);
                continue;
            }
            if (operation->cancel_before_start &&
                operation->cancel_before_start->load(std::memory_order_acquire)) {
                state->queue.pop_front();
                state->queued_bytes.fetch_sub(queued_cost(*operation->message),
                                              std::memory_order_relaxed);
                complete(operation, std::make_exception_ptr(boost::system::system_error(
                                        boost::asio::error::operation_aborted)));
                continue;
            }
            // A failed write fails only its own call. HttpClientTransport answers a refused request
            // or a dropped connection with an error and opens a new connection for the next one,
            // so failing every later call as well would end a client over one request. A transport
            // that cannot recover fails each later call on its own.
            std::exception_ptr error;
            try {
                if (operation->related_request) {
                    co_await state->transport->write_related_message(*operation->message,
                                                                     *operation->related_request);
                } else {
                    co_await state->transport->write_message(*operation->message);
                }
            } catch (...) {
                error = std::current_exception();
            }

            state->queue.pop_front();
            state->queued_bytes.fetch_sub(queued_cost(*operation->message), std::memory_order_relaxed);
            complete(operation, std::move(error));
        }

        state->draining = false;
    }

    /// Queues @p operation, whose bytes are already counted, and starts draining. On the strand.
    static void push(const std::shared_ptr<SerializedTransportWriterState>& state,
                     std::shared_ptr<Operation> operation) {
        state->queue.push_back(std::move(operation));
        if (!state->draining) {
            state->draining = true;
            boost::asio::co_spawn(state->strand, drain(state), boost::asio::detached);
        }
    }

    static Task<void> enqueue_and_wait(std::shared_ptr<SerializedTransportWriterState> state,
                                       std::shared_ptr<const std::string> message,
                                       std::shared_ptr<const std::atomic<bool>> cancel_before_start,
                                       std::shared_ptr<const std::string> related_request = nullptr) {
        auto operation = std::make_shared<Operation>(state->strand, std::move(message),
                                                     std::move(cancel_before_start));
        operation->related_request = std::move(related_request);
        if (operation->message) {
            state->queued_bytes.fetch_add(queued_cost(*operation->message), std::memory_order_relaxed);
        }
        push(state, operation);

        while (!operation->completed) {
            boost::system::error_code ignored;
            co_await operation->completion.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }

        if (operation->error) {
            std::rethrow_exception(operation->error);
        }
    }

    static Task<void> write_beside_queue(std::shared_ptr<SerializedTransportWriterState> state,
                                         std::shared_ptr<const std::string> message) {
        co_await state->transport->write_message(*message);
    }

    /// Writes @p message at once when the caller runs on the strand and nothing is queued, posted
    /// or being written, so that no write overtakes another; otherwise queues it and waits.
    static Task<void> write_or_enqueue(std::shared_ptr<SerializedTransportWriterState> state,
                                       std::shared_ptr<const std::string> message,
                                       std::shared_ptr<const std::atomic<bool>> cancel_before_start) {
        const auto caller = co_await boost::asio::this_coro::executor;
        if (caller != state->strand) {
            co_await boost::asio::co_spawn(
                state->strand,
                enqueue_and_wait(state, std::move(message), std::move(cancel_before_start)),
                boost::asio::use_awaitable);
            co_return;
        }
        if (state->posted.load(std::memory_order_acquire) != 0) {
            // A message posted before this call is still on its way to the strand; this one
            // follows it there.
            co_await boost::asio::post(state->strand, boost::asio::use_awaitable);
        }
        if (state->draining) {
            co_await enqueue_and_wait(state, std::move(message), std::move(cancel_before_start));
            co_return;
        }
        if (cancel_before_start && cancel_before_start->load(std::memory_order_acquire)) {
            throw boost::system::system_error(boost::asio::error::operation_aborted);
        }
        // Messages written meanwhile queue behind this one, as behind a drain.
        state->draining = true;
        const auto bytes = queued_cost(*message);
        state->queued_bytes.fetch_add(bytes, std::memory_order_relaxed);
        std::exception_ptr error;
        try {
            co_await state->transport->write_message(*message);
        } catch (...) {
            error = std::current_exception();
        }
        state->queued_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        if (state->queue.empty()) {
            state->draining = false;
        } else {
            boost::asio::co_spawn(state->strand, drain(state), boost::asio::detached);
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::shared_ptr<ITransport> transport;
    boost::asio::any_io_executor strand;
    std::deque<std::shared_ptr<Operation>> queue;
    bool draining{false};
    /// Messages post_message() has counted but not yet queued.
    std::atomic<std::size_t> posted{0};
    /// Bytes of the messages queued or being written.
    std::atomic<std::size_t> queued_bytes{0};
};

SerializedTransportWriter::SerializedTransportWriter(std::shared_ptr<ITransport> transport,
                                                     const boost::asio::any_io_executor& executor)
    : state_(std::make_shared<SerializedTransportWriterState>(std::move(transport), executor)) {
    if (!state_->transport) {
        throw std::invalid_argument("SerializedTransportWriter requires a transport");
    }
}

std::size_t SerializedTransportWriter::queued_bytes() const noexcept {
    return state_ ? state_->queued_bytes.load(std::memory_order_relaxed) : 0;
}

Task<void> SerializedTransportWriter::write_message(std::string_view message) const {
    if (!state_) {
        throw std::logic_error("Cannot use a moved-from SerializedTransportWriter");
    }

    // This function deliberately is not a coroutine. Copying here makes the
    // string_view safe before the returned awaitable can suspend, while the
    // shared allocation avoids keeping an SSO string in a GCC 11 coroutine frame.
    auto owned_message = std::make_shared<const std::string>(message);
    return SerializedTransportWriterState::write_or_enqueue(state_, std::move(owned_message), nullptr);
}

Task<void> SerializedTransportWriter::write_message(std::shared_ptr<const std::string> message) const {
    return write_message(std::move(message), nullptr);
}

Task<void> SerializedTransportWriter::write_related(
    std::shared_ptr<const std::string> message,
    std::shared_ptr<const std::string> related_request) const {
    if (!state_) {
        throw std::logic_error("Cannot use a moved-from SerializedTransportWriter");
    }
    if (!message || !related_request) {
        throw std::invalid_argument("SerializedTransportWriter message must not be null");
    }
    auto state = state_;
    return boost::asio::co_spawn(state->strand,
                                 SerializedTransportWriterState::enqueue_and_wait(
                                     state, std::move(message), nullptr, std::move(related_request)),
                                 boost::asio::use_awaitable);
}

Task<void> SerializedTransportWriter::write_overlapping(
    std::shared_ptr<const std::string> message) const {
    if (!state_) {
        throw std::logic_error("Cannot use a moved-from SerializedTransportWriter");
    }

    auto state = state_;
    auto executor = state->strand;
    return boost::asio::co_spawn(
        executor,
        SerializedTransportWriterState::write_beside_queue(std::move(state), std::move(message)),
        boost::asio::use_awaitable);
}

Task<void> SerializedTransportWriter::written() const {
    if (!state_) {
        throw std::logic_error("Cannot use a moved-from SerializedTransportWriter");
    }
    auto state = state_;
    return boost::asio::co_spawn(
        state->strand, SerializedTransportWriterState::enqueue_and_wait(state, nullptr, nullptr),
        boost::asio::use_awaitable);
}

void SerializedTransportWriter::post_message(std::shared_ptr<const std::string> message) const {
    if (!state_) {
        throw std::logic_error("Cannot use a moved-from SerializedTransportWriter");
    }
    if (!message) {
        throw std::invalid_argument("SerializedTransportWriter message must not be null");
    }
    auto state = state_;
    state->queued_bytes.fetch_add(queued_cost(*message), std::memory_order_relaxed);
    state->posted.fetch_add(1, std::memory_order_release);
    boost::asio::post(state->strand, [state, message = std::move(message)]() mutable {
        using State = SerializedTransportWriterState;
        State::push(state,
                    std::make_shared<State::Operation>(state->strand, std::move(message), nullptr));
        state->posted.fetch_sub(1, std::memory_order_release);
    });
}

Task<void> SerializedTransportWriter::write_message(
    std::shared_ptr<const std::string> message,
    std::shared_ptr<const std::atomic<bool>> cancel_before_start) const {
    if (!state_) {
        throw std::logic_error("Cannot use a moved-from SerializedTransportWriter");
    }
    if (!message) {
        throw std::invalid_argument("SerializedTransportWriter message must not be null");
    }
    return SerializedTransportWriterState::write_or_enqueue(state_, std::move(message),
                                                            std::move(cancel_before_start));
}

}  // namespace mcp::detail
