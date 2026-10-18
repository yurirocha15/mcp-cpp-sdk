#include <mcp/detail/serialized_transport_writer.hpp>

#include <mcp/transport/transport.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
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

        std::shared_ptr<const std::string> message;
        std::shared_ptr<const std::atomic<bool>> cancel_before_start;
        boost::asio::steady_timer completion;
        std::exception_ptr error;
        bool completed{false};
    };

    SerializedTransportWriterState(std::shared_ptr<ITransport> owned_transport,
                                   const boost::asio::any_io_executor& executor)
        : transport(std::move(owned_transport)), strand(boost::asio::make_strand(executor)) {}

    static void complete(const std::shared_ptr<Operation>& operation,
                         std::exception_ptr error = nullptr) {
        operation->error = std::move(error);
        operation->completed = true;

        boost::system::error_code ignored;
        operation->completion.expires_at(std::chrono::steady_clock::time_point::min(), ignored);
    }

    static Task<void> drain(std::shared_ptr<SerializedTransportWriterState> state) {
        while (!state->queue.empty()) {
            auto operation = state->queue.front();
            if (operation->cancel_before_start &&
                operation->cancel_before_start->load(std::memory_order_acquire)) {
                state->queue.pop_front();
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
                co_await state->transport->write_message(*operation->message);
            } catch (...) {
                error = std::current_exception();
            }

            state->queue.pop_front();
            complete(operation, std::move(error));
        }

        state->draining = false;
    }

    static Task<void> enqueue_and_wait(std::shared_ptr<SerializedTransportWriterState> state,
                                       std::shared_ptr<const std::string> message,
                                       std::shared_ptr<const std::atomic<bool>> cancel_before_start) {
        auto operation = std::make_shared<Operation>(state->strand, std::move(message),
                                                     std::move(cancel_before_start));
        state->queue.push_back(operation);

        if (!state->draining) {
            state->draining = true;
            boost::asio::co_spawn(state->strand, drain(state), boost::asio::detached);
        }

        while (!operation->completed) {
            boost::system::error_code ignored;
            co_await operation->completion.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }

        if (operation->error) {
            std::rethrow_exception(operation->error);
        }
    }

    std::shared_ptr<ITransport> transport;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    std::deque<std::shared_ptr<Operation>> queue;
    bool draining{false};
};

SerializedTransportWriter::SerializedTransportWriter(std::shared_ptr<ITransport> transport,
                                                     const boost::asio::any_io_executor& executor)
    : state_(std::make_shared<SerializedTransportWriterState>(std::move(transport), executor)) {
    if (!state_->transport) {
        throw std::invalid_argument("SerializedTransportWriter requires a transport");
    }
}

Task<void> SerializedTransportWriter::write_message(std::string_view message) const {
    if (!state_) {
        throw std::logic_error("Cannot use a moved-from SerializedTransportWriter");
    }

    // This function deliberately is not a coroutine. Copying here makes the
    // string_view safe before the returned awaitable can suspend, while the
    // shared allocation avoids keeping an SSO string in a GCC 11 coroutine frame.
    auto owned_message = std::make_shared<const std::string>(message);
    auto state = state_;
    return boost::asio::co_spawn(
        state->strand,
        SerializedTransportWriterState::enqueue_and_wait(state, std::move(owned_message), nullptr),
        boost::asio::use_awaitable);
}

Task<void> SerializedTransportWriter::write_message(std::shared_ptr<const std::string> message) const {
    return write_message(std::move(message), nullptr);
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
    auto state = state_;
    return boost::asio::co_spawn(state->strand,
                                 SerializedTransportWriterState::enqueue_and_wait(
                                     state, std::move(message), std::move(cancel_before_start)),
                                 boost::asio::use_awaitable);
}

}  // namespace mcp::detail
