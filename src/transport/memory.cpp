#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace mcp {

namespace detail {

struct MemoryTransportState {
    using Message = std::variant<std::string, nlohmann::json>;

    struct ReadWaiter {
        explicit ReadWaiter(const boost::asio::any_io_executor& executor) : signal(executor) {
            signal.expires_at(std::chrono::steady_clock::time_point::max());
        }

        void wake() {
            boost::system::error_code ignored;
            signal.cancel(ignored);
        }

        boost::asio::steady_timer signal;
        std::optional<Message> message;
    };

    explicit MemoryTransportState(const boost::asio::any_io_executor& executor)
        : strand(boost::asio::make_strand(executor)) {}

    ~MemoryTransportState() { close_endpoint(lock_peer()); }

    std::shared_ptr<MemoryTransportState> lock_peer() const {
        std::lock_guard lock(peer_mutex);
        return peer.lock();
    }

    void set_peer(const std::shared_ptr<MemoryTransportState>& new_peer) {
        std::lock_guard lock(peer_mutex);
        peer = new_peer;
    }

    static void close_endpoint(const std::shared_ptr<MemoryTransportState>& state) {
        if (!state || state->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        boost::asio::post(state->strand, [state]() {
            for (auto& weak_waiter : state->read_waiters) {
                if (auto waiter = weak_waiter.lock()) {
                    waiter->wake();
                }
            }
            state->read_waiters.clear();
        });
    }

    static std::string as_string(Message message) {
        if (auto* raw = std::get_if<std::string>(&message)) {
            return std::move(*raw);
        }
        return std::get<nlohmann::json>(message).dump();
    }

    static nlohmann::json as_json(Message message) {
        if (auto* json = std::get_if<nlohmann::json>(&message)) {
            return std::move(*json);
        }
        return nlohmann::json::parse(std::get<std::string>(message));
    }

    static Task<Message> next_message(std::shared_ptr<MemoryTransportState> state) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("transport closed");
        }
        if (!state->incoming.empty()) {
            auto message = std::move(state->incoming.front());
            state->incoming.pop();
            co_return message;
        }

        auto waiter = std::make_shared<ReadWaiter>(state->strand);
        state->read_waiters.emplace_back(waiter);
        while (!waiter->message && !state->closed.load(std::memory_order_acquire)) {
            boost::system::error_code error;
            co_await waiter->signal.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error && !waiter->message && !state->closed.load(std::memory_order_acquire)) {
                throw boost::system::system_error(error);
            }
        }

        if (state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("transport closed");
        }
        co_return std::move(*waiter->message);
    }

    void deliver_message(Message message) {
        while (!read_waiters.empty()) {
            auto waiter = read_waiters.front().lock();
            read_waiters.pop_front();
            if (!waiter) {
                continue;
            }

            waiter->message.emplace(std::move(message));
            waiter->wake();
            return;
        }
        incoming.emplace(std::move(message));
    }

    template <typename MessageValue>
    static Task<void> deliver(std::shared_ptr<MemoryTransportState> sender,
                              std::shared_ptr<MemoryTransportState> peer, MessageValue message) {
        if (sender->closed.load(std::memory_order_acquire) ||
            peer->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("transport closed");
        }

        peer->deliver_message(Message(std::move(message)));
        co_return;
    }

    template <typename MessageValue>
    static Task<void> write(std::shared_ptr<MemoryTransportState> state, MessageValue message) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("transport closed");
        }

        auto peer = state->lock_peer();
        if (!peer) {
            throw std::runtime_error("peer transport not set");
        }

        co_await boost::asio::co_spawn(peer->strand, deliver(state, peer, std::move(message)),
                                       boost::asio::use_awaitable);
    }

    boost::asio::strand<boost::asio::any_io_executor> strand;
    std::queue<Message> incoming;
    std::deque<std::weak_ptr<ReadWaiter>> read_waiters;
    std::atomic<bool> closed{false};

   private:
    mutable std::mutex peer_mutex;
    std::weak_ptr<MemoryTransportState> peer;
};

}  // namespace detail

namespace {

Task<std::string> read_message(std::shared_ptr<detail::MemoryTransportState> state) {
    co_return detail::MemoryTransportState::as_string(
        co_await detail::MemoryTransportState::next_message(std::move(state)));
}

Task<nlohmann::json> read_json(std::shared_ptr<detail::MemoryTransportState> state) {
    co_return detail::MemoryTransportState::as_json(
        co_await detail::MemoryTransportState::next_message(std::move(state)));
}

}  // namespace

MemoryTransport::MemoryTransport(const boost::asio::any_io_executor& executor)
    : state_(std::make_shared<detail::MemoryTransportState>(executor)) {}

MemoryTransport::~MemoryTransport() = default;

Task<std::string> MemoryTransport::read_message() {
    auto state = state_;
    return boost::asio::co_spawn(state->strand, mcp::read_message(state), boost::asio::use_awaitable);
}

Task<nlohmann::json> MemoryTransport::read_json() {
    auto state = state_;
    return boost::asio::co_spawn(state->strand, mcp::read_json(state), boost::asio::use_awaitable);
}

Task<void> MemoryTransport::write_json(nlohmann::json message) {
    auto state = state_;
    return boost::asio::co_spawn(state->strand,
                                 detail::MemoryTransportState::write(state, std::move(message)),
                                 boost::asio::use_awaitable);
}

Task<void> MemoryTransport::write_message(std::string_view message) {
    auto state = state_;
    return boost::asio::co_spawn(state->strand,
                                 detail::MemoryTransportState::write(state, std::string(message)),
                                 boost::asio::use_awaitable);
}

void MemoryTransport::close() {
    auto state = state_;
    auto peer = state->lock_peer();
    detail::MemoryTransportState::close_endpoint(state);
    detail::MemoryTransportState::close_endpoint(peer);
}

void MemoryTransport::set_peer(const std::shared_ptr<MemoryTransport>& peer) {
    state_->set_peer(peer ? peer->state_ : nullptr);
}

std::pair<std::shared_ptr<ITransport>, std::shared_ptr<ITransport>> create_memory_transport_pair(
    const boost::asio::any_io_executor& executor) {
    auto transport_a = std::make_shared<MemoryTransport>(executor);
    auto transport_b = std::make_shared<MemoryTransport>(executor);
    transport_a->set_peer(transport_b);
    transport_b->set_peer(transport_a);
    return {transport_a, transport_b};
}

}  // namespace mcp
