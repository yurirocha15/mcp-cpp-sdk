#include "restarting_stdio_transport.hpp"

#include "../detail/constants.hpp"
#include "../detail/jsonrpc_envelope.hpp"
#include "../detail/spawn_awaitable.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace mcp::detail {

struct RestartingStdioTransport::State {
    State(const Executor& owner, StdioProcessOptions process_options, StdioRestartPolicy restart_policy)
        : executor(owner),
          strand(boost::asio::make_strand(owner.get())),
          process(std::move(process_options)),
          policy(restart_policy),
          ready_wake(strand),
          delay(strand) {
        ready_wake.expires_at(boost::asio::steady_timer::time_point::max());
    }

    /// Starts the process of the next generation.
    void launch() {
        current = std::make_shared<StdioClientTransport>(executor, process);
        ++generation;
        renamed.clear();
    }

    /// An id as JSON writes it, so that ids the server and the client spell differently compare
    /// equal; empty for anything but a string or an integer.
    static std::string canonical_id(std::string_view raw) {
        if (raw.empty()) {
            return {};
        }
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        if (parsed.is_discarded() || !(parsed.is_string() || parsed.is_number_integer())) {
            return {};
        }
        return parsed.dump();
    }

    /// The id a notifications/cancelled names, or empty.
    static std::string cancelled_id(std::string_view message) {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return {};
        }
        const auto params = parsed.find("params");
        if (params == parsed.end() || !params->is_object()) {
            return {};
        }
        const auto id = params->find("requestId");
        if (id == params->end() || !(id->is_string() || id->is_number_integer())) {
            return {};
        }
        return id->dump();
    }

    /// The answer a request gets when the process it went to ended before answering it.
    static std::string closed_answer(std::string_view id) {
        std::string answer = R"({"jsonrpc":"2.0","id":)";
        answer += id;
        answer += R"(,"error":{"code":)";
        answer += std::to_string(mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
        answer +=
            R"(,"message":"The server process ended before answering; it is being started again"}})";
        return answer;
    }

    /// Whether a process that ended as @p exit says is started again. A process the transport
    /// had to end, because it broke the connection without exiting, counts as an abnormal end.
    [[nodiscard]] bool restart_wanted(const ProcessExit& exit) const {
        return policy.restart_after_clean_exit || exit.requested || exit.signaled || exit.code != 0;
    }

    /// @p message with its `id` replaced by @p id, or, with @p in_params, its `params.requestId`.
    static std::string with_id(std::string_view message, const nlohmann::json& id,
                               bool in_params = false) {
        auto parsed = nlohmann::json::parse(message, nullptr, false);
        if (!parsed.is_object()) {
            return std::string(message);
        }
        if (in_params) {
            parsed["params"]["requestId"] = id;
        } else {
            parsed["id"] = id;
        }
        return parsed.dump();
    }

    /// Bookkeeping for a message read from the current process. A request whose id is that of an
    /// earlier process's request the client has yet to answer gets another id toward the client,
    /// so that the answers to the two are told apart; note_write() gives the answer its id back.
    void note_read(std::string& message) {
        const auto scanned = scan_jsonrpc_envelope(message);
        if (!scanned) {
            return;
        }
        const auto id = canonical_id(scanned->id);
        if (id.empty()) {
            if (scanned->method == R"("notifications/cancelled")") {
                if (const auto found = renamed.find(cancelled_id(message)); found != renamed.end()) {
                    message = with_id(message, nlohmann::json::parse(found->second), true);
                }
            }
            return;
        }
        if (!scanned->method.empty()) {
            const auto earlier = incoming.find(id);
            if (earlier == incoming.end() || earlier->second.generation == generation) {
                incoming.insert_or_assign(id, Incoming{generation, {}});
                return;
            }
            const nlohmann::json alias =
                std::string(mcp::detail::constants::stdio_restart::g_RENAMED_REQUEST_PREFIX) +
                std::to_string(++renames);
            auto alias_id = alias.dump();
            message = with_id(message, alias);
            renamed.insert_or_assign(id, alias_id);
            incoming.insert_or_assign(std::move(alias_id), Incoming{generation, id});
            return;
        }
        if (const auto found = outstanding.find(id); found != outstanding.end()) {
            outstanding.erase(found);
        }
        if (initialize_request && id == initialize_id && !scanned->has_result) {
            // A server that refuses initialize is not a legacy one, and the next process is not
            // initialized either.
            initialize_request.reset();
            initialized_notification.reset();
        }
    }

    /// Bookkeeping for a message the client writes. Returns false for an answer to a request of a
    /// process that has ended, which is not written; gives an answer to a renamed request its id.
    bool note_write(std::string& message) {
        const auto scanned = scan_jsonrpc_envelope(message);
        if (!scanned) {
            return true;
        }
        const auto id = canonical_id(scanned->id);
        if (scanned->method.empty()) {
            const auto found = incoming.find(id);
            if (id.empty() || found == incoming.end()) {
                return true;
            }
            const bool same_process = found->second.generation == generation;
            const auto original = std::move(found->second.original);
            incoming.erase(found);
            if (!original.empty()) {
                renamed.erase(original);
                if (same_process) {
                    message = with_id(message, nlohmann::json::parse(original));
                }
            }
            return same_process;
        }
        if (id.empty()) {
            if (scanned->method == R"("notifications/initialized")") {
                initialized_notification = std::string(message);
            } else if (scanned->method == R"("notifications/cancelled")") {
                // A cancelled request may never be answered; it no longer waits.
                if (const auto found = outstanding.find(cancelled_id(message));
                    found != outstanding.end()) {
                    outstanding.erase(found);
                }
            }
            return true;
        }
        outstanding.insert(id);
        if (scanned->method == R"("initialize")") {
            initialize_request = std::string(message);
            initialize_id = id;
        }
        return true;
    }

    /// Undoes note_write() for a request the transport refused to write.
    void forget_request(std::string_view message) {
        const auto scanned = scan_jsonrpc_envelope(message);
        if (scanned && !scanned->method.empty()) {
            if (const auto found = outstanding.find(canonical_id(scanned->id));
                found != outstanding.end()) {
                outstanding.erase(found);
            }
        }
    }

    /// Whether @p message answers the replayed initialize.
    /// @throws std::runtime_error When it refuses it.
    [[nodiscard]] bool is_initialize_answer(std::string_view message) const {
        const auto scanned = scan_jsonrpc_envelope(message);
        if (!scanned || !scanned->method.empty() || canonical_id(scanned->id) != initialize_id) {
            return false;
        }
        if (!scanned->has_result) {
            throw std::runtime_error("The restarted server refused initialize");
        }
        return true;
    }

    /// Waits until a process is ready for the client's messages, or the transport is closed.
    static Task<void> wait_until_ready(std::shared_ptr<State> self) {
        while (!self->ready && !self->closed.load(std::memory_order_acquire)) {
            boost::system::error_code ignored;
            co_await self->ready_wake.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }
    }

    static Task<void> write(std::shared_ptr<State> self, std::shared_ptr<std::string> message) {
        co_await wait_until_ready(self);
        if (self->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("transport closed");
        }
        if (!self->note_write(*message)) {
            co_return;
        }
        auto transport = self->current;
        try {
            co_await transport->write_message(*message);
        } catch (const std::invalid_argument&) {
            self->forget_request(*message);
            throw;
        } catch (...) {
            // The process has gone. The read side sees the end, answers this message if it was a
            // request, and starts the next process; without one to come, the failure is the
            // client's.
            if (self->closed.load(std::memory_order_acquire) ||
                self->restarts >= self->policy.max_restarts) {
                throw;
            }
        }
    }

    /// Handles the end of @p transport's process, reported by @p failure: answers the requests it
    /// left waiting and makes the next start due, or rethrows @p failure when no start follows.
    static Task<void> process_ended(std::shared_ptr<State> self,
                                    std::shared_ptr<StdioClientTransport> transport,
                                    std::exception_ptr failure) {
        const auto exit = co_await transport->shutdown();
        if (self->closed.load(std::memory_order_acquire) || !self->restart_wanted(exit) ||
            self->restarts >= self->policy.max_restarts) {
            std::rethrow_exception(failure);
        }
        self->ready = false;
        self->restart_due = true;
        self->ended_with = failure;
        for (const auto& id : self->outstanding) {
            self->queued.push_back(closed_answer(id));
        }
        self->outstanding.clear();
    }

    /// Sends a legacy session's initialize request and initialized notification to a new
    /// process. Messages that arrive before the answer to initialize are kept for the client; that
    /// answer is not passed on.
    static Task<void> replay_initialize(std::shared_ptr<State> self) {
        if (!self->initialize_request) {
            co_return;
        }
        auto transport = self->current;
        co_await transport->write_message(*self->initialize_request);
        for (;;) {
            auto message = co_await transport->read_message();
            if (self->is_initialize_answer(message)) {
                break;
            }
            self->note_read(message);
            self->queued.push_back(std::move(message));
        }
        if (self->initialized_notification) {
            co_await transport->write_message(*self->initialized_notification);
        }
    }

    /// Starts processes, each after the policy's delay, until one is ready or the policy allows no
    /// more. Rethrows what ended the last process when none starts.
    static Task<void> start_again(std::shared_ptr<State> self) {
        auto failure = self->ended_with;
        while (self->restarts < self->policy.max_restarts &&
               !self->closed.load(std::memory_order_acquire)) {
            ++self->restarts;
            self->delay.expires_after(self->policy.delay);
            boost::system::error_code ignored;
            co_await self->delay.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
            if (self->closed.load(std::memory_order_acquire)) {
                break;
            }
            bool started = false;
            try {
                self->launch();
                co_await replay_initialize(self);
                started = true;
            } catch (...) {
                failure = std::current_exception();
            }
            if (started) {
                self->restart_due = false;
                self->ready = true;
                self->ready_wake.cancel();
                co_return;
            }
            // A process that started but could not be initialized does not keep running.
            auto transport = self->current;
            static_cast<void>(co_await transport->shutdown());
        }
        self->restart_due = false;
        std::rethrow_exception(failure);
    }

    static Task<std::string> read(std::shared_ptr<State> self) {
        for (;;) {
            if (!self->queued.empty()) {
                auto message = std::move(self->queued.front());
                self->queued.pop_front();
                co_return message;
            }
            if (self->closed.load(std::memory_order_acquire)) {
                throw std::runtime_error("transport closed");
            }
            if (self->restart_due) {
                co_await start_again(self);
                continue;
            }
            auto transport = self->current;
            std::exception_ptr failure;
            try {
                auto message = co_await transport->read_message();
                self->note_read(message);
                co_return message;
            } catch (...) {
                failure = std::current_exception();
            }
            co_await process_ended(self, std::move(transport), failure);
        }
    }

    Executor executor;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    StdioProcessOptions process;
    StdioRestartPolicy policy;
    std::shared_ptr<StdioClientTransport> current;
    std::uint64_t generation = 0;
    std::size_t restarts = 0;
    std::atomic<bool> closed{false};
    /// False from the end of a process until the next one is ready for the client's messages.
    bool ready = true;
    /// A process has ended, and the read side has yet to start the next one.
    bool restart_due = false;
    /// The read failure that reported the end, rethrown when no restart follows.
    std::exception_ptr ended_with;
    /// The ids of the client's requests that the current process has not answered.
    std::set<std::string, std::less<>> outstanding;
    /// A server's request that the client has not answered.
    struct Incoming {
        /// The generation of the process that sent it.
        std::uint64_t generation = 0;
        /// The id the process gave it, when the client knows it by another; empty otherwise.
        std::string original;
    };
    /// The server's requests that the client has not answered, by the id the client knows.
    std::map<std::string, Incoming, std::less<>> incoming;
    /// The current process's renamed requests: the id it gave each, to the id the client knows.
    std::map<std::string, std::string, std::less<>> renamed;
    std::uint64_t renames = 0;
    std::optional<std::string> initialize_request;
    std::string initialize_id;
    std::optional<std::string> initialized_notification;
    /// Messages for the client before the next read: the answers made for an ended process, and
    /// what arrived while initialize was sent again.
    std::deque<std::string> queued;
    boost::asio::steady_timer ready_wake;
    boost::asio::steady_timer delay;
};

RestartingStdioTransport::RestartingStdioTransport(const Executor& executor,
                                                   StdioProcessOptions process,
                                                   StdioRestartPolicy policy)
    : state_(std::make_shared<State>(executor, std::move(process), policy)) {
    state_->launch();
}

RestartingStdioTransport::~RestartingStdioTransport() { close(); }

Task<std::string> RestartingStdioTransport::read_message() {
    return detail::spawn_awaitable(state_->strand, State::read(state_));
}

Task<void> RestartingStdioTransport::write_message(std::string_view message) {
    return detail::spawn_awaitable(state_->strand,
                                   State::write(state_, std::make_shared<std::string>(message)));
}

void RestartingStdioTransport::close() {
    if (state_->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    boost::asio::post(state_->strand, [self = state_] {
        self->ready_wake.cancel();
        self->delay.cancel();
        if (self->current) {
            self->current->close();
        }
    });
}

}  // namespace mcp::detail
