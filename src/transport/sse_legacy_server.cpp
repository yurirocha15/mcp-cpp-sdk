#include "sse_legacy_server.hpp"

#include "mcp/detail/async/strand_executor.hpp"
#include "mcp/detail/constants.hpp"
#include "mcp/detail/text/secure_random.hpp"

#include <mcp/server/server.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace mcp::detail {

namespace http = boost::beast::http;

namespace {

namespace sse_constants = mcp::detail::constants::sse_legacy;

/// Appends one event of @p name to @p out. A line break in @p data starts another `data` line, so
/// the event stays one event.
void append_event(std::string& out, std::string_view name, std::string_view data) {
    out += "event: ";
    out += name;
    out += '\n';
    std::size_t position = 0;
    for (;;) {
        const auto end = data.find_first_of("\r\n", position);
        out += "data: ";
        out += data.substr(position, end == std::string_view::npos ? end : end - position);
        out += '\n';
        if (end == std::string_view::npos) {
            break;
        }
        position = end + 1;
        if (data[end] == '\r' && position < data.size() && data[position] == '\n') {
            ++position;
        }
    }
    out += '\n';
}

/// What one stream holds, on the heap and not in the coroutine's frame: GCC 11 mishandles a
/// std::string that lives across a suspension point there.
struct StreamState {
    http::response<http::empty_body> header;
    std::optional<http::response_serializer<http::empty_body>> serializer;
    std::string text;
};

}  // namespace

LegacySseSession::LegacySseSession(std::string session_id, std::string session_principal,
                                   const boost::asio::any_io_executor& stream_executor,
                                   std::size_t max_messages, std::size_t max_bytes)
    : id(std::move(session_id)),
      principal(std::move(session_principal)),
      max_messages_(max_messages),
      max_bytes_(max_bytes),
      signal_(std::make_shared<boost::asio::steady_timer>(stream_executor)) {
    signal_->expires_at(std::chrono::steady_clock::time_point::max());
}

void LegacySseSession::wake() const {
    boost::asio::dispatch(signal_->get_executor(), [signal = signal_] { (void)signal->cancel(); });
}

void LegacySseSession::push(std::string message) {
    {
        std::scoped_lock lock(mutex_);
        if (ended_) {
            return;
        }
        if (queue_.size() >= max_messages_ ||
            (!queue_.empty() && queued_bytes_ + message.size() > max_bytes_)) {
            // A client that does not read its stream cannot be left to make the server hold what
            // it sends; the stream ends and the client opens another.
            ended_ = true;
        } else {
            queued_bytes_ += message.size();
            queue_.push_back(std::move(message));
        }
    }
    wake();
}

void LegacySseSession::end() {
    {
        std::scoped_lock lock(mutex_);
        ended_ = true;
    }
    wake();
}

LegacySseSession::Batch LegacySseSession::take() {
    Batch batch;
    std::scoped_lock lock(mutex_);
    batch.messages.reserve(queue_.size());
    for (auto& message : queue_) {
        batch.messages.push_back(std::move(message));
    }
    queue_.clear();
    queued_bytes_ = 0;
    batch.ended = ended_;
    return batch;
}

LegacySseSessions::LegacySseSessions(LegacySseOptions options) : options_(std::move(options)) {}

std::shared_ptr<LegacySseSession> LegacySseSessions::open(
    const boost::asio::any_io_executor& executor, const boost::asio::any_io_executor& stream_executor,
    std::string principal, const Limits& limits) {
    auto session =
        std::make_shared<LegacySseSession>(generate_secure_session_id(), std::move(principal),
                                           stream_executor, limits.max_messages, limits.max_bytes);
    // The server and its endpoint share a strand of the session's own, as a session of the
    // Streamable HTTP transport has.
    session->server_strand = make_strand_executor(executor);
    // The pipe holds the session weakly, so a session that was removed routes nothing more.
    const std::weak_ptr<LegacySseSession> weak = session;
    session->pipe = std::make_shared<SessionPipe>(
        session->server_strand,
        [weak](std::string message, const std::string* /*related_request*/) {
            if (auto alive = weak.lock()) {
                alive->push(std::move(message));
            }
        },
        [weak] {
            if (auto alive = weak.lock()) {
                alive->end();
            }
        });
    {
        std::unique_lock lock(mutex_);
        if (closed_ || sessions_.size() >= limits.max_sessions) {
            lock.unlock();
            session->pipe->close();
            return nullptr;
        }
        sessions_.emplace(session->id, session);
    }
    return session;
}

void LegacySseSessions::start(const std::shared_ptr<LegacySseSession>& session,
                              std::unique_ptr<Server> server) {
    boost::asio::co_spawn(
        session->signal()->get_executor(),
        [srv = std::move(server), transport = std::shared_ptr<ITransport>(session->pipe),
         exec = session->server_strand]() mutable -> boost::asio::awaitable<void> {
            co_await srv->run(std::move(transport), exec);
        },
        boost::asio::detached);
}

std::shared_ptr<LegacySseSession> LegacySseSessions::find(const std::string& id) const {
    std::shared_lock lock(mutex_);
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : found->second;
}

void LegacySseSessions::remove(const std::string& id) {
    std::shared_ptr<LegacySseSession> session;
    {
        std::unique_lock lock(mutex_);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            return;
        }
        session = std::move(found->second);
        sessions_.erase(found);
    }
    session->end();
    session->pipe->close();
}

void LegacySseSessions::close() {
    std::unordered_map<std::string, std::shared_ptr<LegacySseSession>> sessions;
    {
        std::unique_lock lock(mutex_);
        closed_ = true;
        sessions.swap(sessions_);
    }
    for (const auto& entry : sessions) {
        entry.second->end();
        entry.second->pipe->close();
    }
}

std::size_t LegacySseSessions::size() const {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

std::optional<std::string> LegacySseSessions::session_id_of(std::string_view target) {
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    auto query = target.substr(question + 1);
    query = query.substr(0, query.find('#'));
    constexpr auto name = sse_constants::g_SESSION_ID_PARAMETER;
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto pair = query.substr(0, ampersand);
        if (pair.size() > name.size() + 1 && pair.starts_with(name) && pair[name.size()] == '=') {
            return std::string(pair.substr(name.size() + 1));
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        query.remove_prefix(ampersand + 1);
    }
    return std::nullopt;
}

namespace {

/// Forgets the session when the stream ends, however it ends.
class SessionRemoval {
   public:
    SessionRemoval(LegacySseSessions& sessions, std::shared_ptr<LegacySseSession> session)
        : sessions_(sessions), session_(std::move(session)) {}
    SessionRemoval(const SessionRemoval&) = delete;
    SessionRemoval& operator=(const SessionRemoval&) = delete;
    ~SessionRemoval() {
        try {
            sessions_.remove(session_->id);
        } catch (const std::exception&) {
            // A destructor does not throw.
        }
    }

   private:
    LegacySseSessions& sessions_;
    std::shared_ptr<LegacySseSession> session_;
};

}  // namespace

Task<void> LegacySseSessions::serve(Connection& stream, const IncomingRequest& request,
                                    std::shared_ptr<LegacySseSession> session,
                                    std::chrono::milliseconds keepalive,
                                    const std::atomic<bool>& closed) {
    const SessionRemoval removal(*this, session);
    auto state = std::make_shared<StreamState>();
    state->header = http::response<http::empty_body>{http::status::ok, request.version()};
    state->header.set(http::field::server, "mcp-cpp-sdk");
    state->header.set(http::field::content_type,
                      std::string(sse_constants::g_CONTENT_TYPE_EVENT_STREAM));
    state->header.set(http::field::cache_control, "no-cache");
    state->header.set("X-Accel-Buffering", "no");
    // The stream is the connection's last exchange.
    state->header.keep_alive(false);
    state->header.chunked(true);
    state->serializer.emplace(state->header);

    boost::system::error_code error;
    co_await http::async_write_header(stream, *state->serializer,
                                      boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error) {
        co_return;
    }

    append_event(state->text, sse_constants::g_EVENT_ENDPOINT,
                 options_.message_path + "?" + std::string(sse_constants::g_SESSION_ID_PARAMETER) +
                     "=" + session->id);
    co_await boost::asio::async_write(stream, http::make_chunk(boost::asio::buffer(state->text)),
                                      boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error) {
        co_return;
    }

    for (;;) {
        auto batch = session->take();
        if (!batch.messages.empty()) {
            state->text.clear();
            for (const auto& message : batch.messages) {
                append_event(state->text, sse_constants::g_EVENT_MESSAGE, message);
            }
            co_await boost::asio::async_write(
                stream, http::make_chunk(boost::asio::buffer(state->text)),
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error) {
                co_return;
            }
            // More may have come while the write was suspended.
            continue;
        }
        if (batch.ended) {
            co_await boost::asio::async_write(
                stream, http::make_chunk_last(),
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            co_return;
        }
        if (closed.load(std::memory_order_acquire)) {
            co_return;
        }

        // A comment now and then keeps intermediaries from closing an idle stream and finds a
        // client that left.
        session->signal()->expires_after(keepalive);
        boost::system::error_code wait_error;
        co_await session->signal()->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
        if (!wait_error) {
            state->text = ": keep-alive\n\n";
            co_await boost::asio::async_write(
                stream, http::make_chunk(boost::asio::buffer(state->text)),
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error) {
                co_return;
            }
        }
    }
}

}  // namespace mcp::detail
