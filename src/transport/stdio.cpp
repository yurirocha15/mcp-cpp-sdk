#include <mcp/transport/stdio.hpp>

#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace mcp {

struct StdioTransport::Impl {
    struct SharedState {
        explicit SharedState(boost::asio::strand<boost::asio::any_io_executor>& strand)
            : timer(strand) {}

        boost::asio::steady_timer timer;
        std::queue<std::string> queue;
        std::atomic<bool> closed{false};
    };

    Impl(const boost::asio::any_io_executor& executor, std::istream& input, std::ostream& output)
        : input(input),
          output(output),
          strand(boost::asio::make_strand(executor)),
          state(std::make_shared<SharedState>(strand)) {
        state->timer.expires_at(std::chrono::steady_clock::time_point::max());
    }

    ~Impl() {
        state->closed.store(true, std::memory_order_release);
        if (reader_thread.joinable()) {
            reader_thread.join();
        }
    }

    void ensure_reader_started() {
        if (reader_started) {
            return;
        }
        reader_started = true;
        auto shared_state = state;
        reader_thread = std::thread([&in = input, shared_state]() {
            std::string line;
            while (std::getline(in, line)) {
                if (shared_state->closed.load(std::memory_order_acquire)) {
                    break;
                }

                boost::asio::post(shared_state->timer.get_executor(),
                                  [shared_state, m = std::move(line)]() mutable {
                                      shared_state->queue.push(std::move(m));
                                      shared_state->timer.cancel();
                                  });
            }

            boost::asio::post(shared_state->timer.get_executor(), [shared_state]() {
                shared_state->closed.store(true, std::memory_order_release);
                shared_state->timer.cancel();
            });
        });
    }

    std::istream& input;
    std::ostream& output;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    std::shared_ptr<SharedState> state;
    bool reader_started{false};
    std::thread reader_thread;
};

StdioTransport::StdioTransport(const boost::asio::any_io_executor& executor, std::istream& input,
                               std::ostream& output)
    : impl_(std::make_unique<Impl>(executor, input, output)) {}

StdioTransport::~StdioTransport() = default;

Task<std::string> StdioTransport::read_message() {
    impl_->ensure_reader_started();
    auto& state = *impl_->state;
    for (;;) {
        if (!state.queue.empty()) {
            auto message = std::move(state.queue.front());
            state.queue.pop();
            co_return message;
        }

        if (state.closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("StdioTransport is closed");
        }

        state.timer.expires_at(std::chrono::steady_clock::time_point::max());
        try {
            co_await state.timer.async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (err.code() != boost::asio::error::operation_aborted) {
                throw;
            }
        }
    }
}

Task<void> StdioTransport::write_message(std::string_view message) {
    std::string msg(message);
    co_await boost::asio::post(impl_->strand, boost::asio::use_awaitable);
    impl_->output << msg << '\n';
    impl_->output.flush();
    co_return;
}

void StdioTransport::close() {
    impl_->state->closed.store(true, std::memory_order_release);
    impl_->state->timer.cancel();
}

}  // namespace mcp
