#include "../detail/memory_transport_read.hpp"
#include "../detail/spawn_awaitable.hpp"
#include "../detail/strand_executor.hpp"

#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
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
    using Body = std::variant<std::string, nlohmann::json>;

    /// A message, and the id as JSON text of the request it was written as related to, if any.
    struct Message {
        // NOLINTNEXTLINE(google-explicit-constructor)
        Message(Body content, std::shared_ptr<const std::string> related = nullptr)
            : body(std::move(content)), related_request(std::move(related)) {}
        Body body;
        std::shared_ptr<const std::string> related_request;
    };

    struct ReadWaiter {
        explicit ReadWaiter(const boost::asio::any_io_executor& executor) : signal(executor) {
            signal.expires_at(std::chrono::steady_clock::time_point::max());
        }

        void wake() { signal.cancel(); }

        boost::asio::steady_timer signal;
        std::optional<Message> message;
    };

    explicit MemoryTransportState(const boost::asio::any_io_executor& executor)
        : strand(mcp::detail::make_strand_executor(executor)) {}

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
        if (auto* raw = std::get_if<std::string>(&message.body)) {
            return std::move(*raw);
        }
        return std::get<nlohmann::json>(message.body).dump();
    }

    static nlohmann::json as_json(Message message) {
        if (auto* json = std::get_if<nlohmann::json>(&message.body)) {
            return std::move(*json);
        }
        return nlohmann::json::parse(std::get<std::string>(message.body));
    }

    /// @return The next message, or std::nullopt once the transport is closed.
    static Task<std::optional<Message>> next_message_or_closed(
        std::shared_ptr<MemoryTransportState> state) {
        if (state->closed.load(std::memory_order_acquire)) {
            co_return std::nullopt;
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
            co_return std::nullopt;
        }
        co_return std::move(*waiter->message);
    }

    static Task<Message> next_message(std::shared_ptr<MemoryTransportState> state) {
        auto message = co_await next_message_or_closed(std::move(state));
        if (!message) {
            throw std::runtime_error("transport closed");
        }
        co_return std::move(*message);
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

    /// Queues @p message for the peer. Runs on the peer's strand, which owns its queue and waiters.
    template <typename MessageValue>
    static void deliver_now(const MemoryTransportState& sender, MemoryTransportState& peer,
                            MessageValue message) {
        if (sender.closed.load(std::memory_order_acquire) ||
            peer.closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("transport closed");
        }

        peer.deliver_message(Message(std::move(message)));
    }

    template <typename MessageValue>
    static Task<void> deliver(std::shared_ptr<MemoryTransportState> sender,
                              std::shared_ptr<MemoryTransportState> peer, MessageValue message) {
        deliver_now(*sender, *peer, std::move(message));
        co_return;
    }

    static Task<void> write(std::shared_ptr<MemoryTransportState> state, Message message) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("transport closed");
        }

        auto peer = state->lock_peer();
        if (!peer) {
            throw std::runtime_error("peer transport not set");
        }

        // A writer whose own executor is the peer's strand runs inside it already, which is all a
        // delivery needs, so it delivers here instead of spawning a coroutine onto that strand.
        const auto caller = co_await boost::asio::this_coro::executor;
        if (caller == peer->strand) {
            deliver_now(*state, *peer, std::move(message));
            co_return;
        }

        co_await mcp::detail::spawn_awaitable(peer->strand, deliver(state, peer, std::move(message)));
    }

    boost::asio::any_io_executor strand;
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

Task<std::optional<std::string>> read_message_until_closed(
    std::shared_ptr<detail::MemoryTransportState> state) {
    auto message = co_await detail::MemoryTransportState::next_message_or_closed(std::move(state));
    if (!message) {
        co_return std::nullopt;
    }
    co_return detail::MemoryTransportState::as_string(std::move(*message));
}

Task<std::optional<nlohmann::json>> read_json_until_closed(
    std::shared_ptr<detail::MemoryTransportState> state) {
    auto message = co_await detail::MemoryTransportState::next_message_or_closed(std::move(state));
    if (!message) {
        co_return std::nullopt;
    }
    co_return detail::MemoryTransportState::as_json(std::move(*message));
}

Task<std::optional<detail::MemoryTransportAccess::RelatedMessage>> read_related_until_closed(
    std::shared_ptr<detail::MemoryTransportState> state) {
    auto message = co_await detail::MemoryTransportState::next_message_or_closed(std::move(state));
    if (!message) {
        co_return std::nullopt;
    }
    auto related_request = std::move(message->related_request);
    co_return detail::MemoryTransportAccess::RelatedMessage{
        detail::MemoryTransportState::as_string(std::move(*message)), std::move(related_request)};
}

// A read touches the queue and the waiters, which belong to the endpoint's strand. A reader whose
// own executor is that strand runs on it already, so it reads in place; any other is spawned onto
// the strand, where it also stays after each suspension.
Task<std::optional<std::string>> read_message_until_closed_on_strand(
    std::shared_ptr<detail::MemoryTransportState> state) {
    const auto caller = co_await boost::asio::this_coro::executor;
    if (caller == state->strand) {
        co_return co_await mcp::read_message_until_closed(std::move(state));
    }
    co_return co_await mcp::detail::spawn_awaitable(state->strand,
                                                    mcp::read_message_until_closed(state));
}

Task<std::optional<nlohmann::json>> read_json_until_closed_on_strand(
    std::shared_ptr<detail::MemoryTransportState> state) {
    const auto caller = co_await boost::asio::this_coro::executor;
    if (caller == state->strand) {
        co_return co_await mcp::read_json_until_closed(std::move(state));
    }
    co_return co_await mcp::detail::spawn_awaitable(state->strand, mcp::read_json_until_closed(state));
}

}  // namespace

Task<std::optional<detail::MemoryTransportAccess::RelatedMessage>>
detail::MemoryTransportAccess::read_related_until_closed(MemoryTransport& transport) {
    auto state = transport.state_;
    return mcp::detail::spawn_awaitable(state->strand, mcp::read_related_until_closed(state));
}

Task<std::optional<std::string>> detail::MemoryTransportAccess::read_message_until_closed(
    MemoryTransport& transport) {
    return mcp::read_message_until_closed_on_strand(transport.state_);
}

Task<std::optional<nlohmann::json>> detail::MemoryTransportAccess::read_json_until_closed(
    MemoryTransport& transport) {
    return mcp::read_json_until_closed_on_strand(transport.state_);
}

MemoryTransport::MemoryTransport(const boost::asio::any_io_executor& executor)
    : state_(std::make_shared<detail::MemoryTransportState>(executor)) {}

MemoryTransport::~MemoryTransport() = default;

Task<std::string> MemoryTransport::read_message() {
    auto state = state_;
    return mcp::detail::spawn_awaitable(state->strand, mcp::read_message(state));
}

Task<nlohmann::json> MemoryTransport::read_json() {
    auto state = state_;
    return mcp::detail::spawn_awaitable(state->strand, mcp::read_json(state));
}

// A write reads only the closed flag and the peer pointer before it hands the message to the
// peer's strand, so it runs on the caller's executor instead of first hopping to this strand.
Task<void> MemoryTransport::write_json(nlohmann::json message) {
    return detail::MemoryTransportState::write(
        state_, detail::MemoryTransportState::Message(std::move(message)));
}

Task<void> MemoryTransport::write_message(std::string_view message) {
    return detail::MemoryTransportState::write(
        state_, detail::MemoryTransportState::Message(std::string(message)));
}

Task<void> MemoryTransport::write_related_message(std::string_view message,
                                                  std::string_view related_request) {
    return detail::MemoryTransportState::write(
        state_, detail::MemoryTransportState::Message(
                    std::string(message), std::make_shared<const std::string>(related_request)));
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
