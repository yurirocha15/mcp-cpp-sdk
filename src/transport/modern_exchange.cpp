#include "modern_exchange.hpp"

#include "../detail/beast_string.hpp"
#include "../detail/constants.hpp"
#include "../detail/http_request_metadata.hpp"
#include "../detail/socket_probe.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/detail/json_dump.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>

#include <exception>
#include <string_view>
#include <utility>

namespace mcp::detail {

namespace http = boost::beast::http;

ModernExchange::ModernExchange(boost::asio::any_io_executor strand_arg, const StreamOptions& options,
                               bool streams_arg, std::atomic<std::size_t>* budget,
                               std::size_t budget_limit)
    : strand(std::move(strand_arg)),
      signal(strand),
      max_queued(options.max_queued_notifications),
      max_queued_bytes(options.max_queued_bytes),
      streams(streams_arg) {
    signal.expires_at(std::chrono::steady_clock::time_point::max());
    if (budget != nullptr) {
        budget_lease.emplace(*budget, budget_limit);
    }
}

void ModernExchange::wake() {
    boost::asio::post(strand, [self = shared_from_this()] { self->wake_on_strand(); });
}

void ModernExchange::wake_on_strand() {
    woken = true;
    signal.cancel();
}

bool ModernExchange::push_notification(std::string message) {
    {
        std::lock_guard lock(mutex);
        if (queue_ended) {
            return true;
        }
        const auto size = message.size();
        if (notifications.size() >= max_queued || size > max_queued_bytes ||
            queued_bytes > max_queued_bytes - size ||
            (budget_lease && !budget_lease->grow_to(queued_bytes + size))) {
            return false;
        }
        queued_bytes += size;
        notifications.push_back(std::move(message));
    }
    wake();
    return true;
}

void ModernExchange::release_queued(std::size_t bytes) {
    std::lock_guard lock(mutex);
    if (queue_ended) {
        return;
    }
    queued_bytes -= bytes;
    if (budget_lease) {
        budget_lease->shrink_to(queued_bytes);
    }
}

void ModernExchange::end_queue() {
    std::vector<std::string> discarded;
    {
        std::lock_guard lock(mutex);
        queue_ended = true;
        queued_bytes = 0;
        discarded.swap(notifications);
        budget_lease.reset();
    }
}

void ModernExchange::abandon() {
    cancel();
    end_queue();
}

void ModernExchange::cancel() {
    cancelled->store(true, std::memory_order_relaxed);
    std::function<void()> hook;
    {
        std::lock_guard lock(mutex);
        hook = on_cancel;
    }
    if (hook) {
        hook();
    }
}

void ModernExchange::set_on_cancel(std::function<void()> hook) {
    std::lock_guard lock(mutex);
    on_cancel = std::move(hook);
}

void ModernExchange::finish(std::string response_wire) {
    {
        std::lock_guard lock(mutex);
        response = std::move(response_wire);
    }
    wake();
}

namespace {

Task<void> notification_queued() { co_return; }

StringResponse json_response(const StringRequest& request, http::status status, std::string body) {
    StringResponse response{status, request.version()};
    response.set(http::field::server, "mcp-cpp-sdk");
    response.set(http::field::content_type, "application/json");
    response.keep_alive(request.keep_alive());
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

/// Writes @p text as one chunk within the time the limits give the peer to take it.
Task<bool> write_chunk(StreamConnection& stream, std::string_view text,
                       const HttpConnectionLimits& limits) {
    arm_request_read_deadline(stream, limits);
    boost::system::error_code error;
    co_await boost::asio::async_write(stream, http::make_chunk(boost::asio::buffer(text)),
                                      boost::asio::redirect_error(boost::asio::use_awaitable, error));
    stream.expires_never();
    co_return !error;
}

Task<bool> write_last_chunk(StreamConnection& stream, const HttpConnectionLimits& limits) {
    arm_request_read_deadline(stream, limits);
    boost::system::error_code error;
    co_await boost::asio::async_write(stream, http::make_chunk_last(),
                                      boost::asio::redirect_error(boost::asio::use_awaitable, error));
    stream.expires_never();
    co_return !error;
}

Task<bool> open_stream(StreamConnection& stream, const StringRequest& request,
                       const HttpConnectionLimits& limits) {
    http::response<http::empty_body> header{http::status::ok, request.version()};
    header.set(http::field::server, "mcp-cpp-sdk");
    header.set(http::field::content_type, "text/event-stream");
    header.set(http::field::cache_control, "no-cache");
    header.set("X-Accel-Buffering", "no");
    header.keep_alive(request.keep_alive());
    header.chunked(true);
    http::response_serializer<http::empty_body> serializer{header};
    arm_request_read_deadline(stream, limits);
    boost::system::error_code error;
    co_await http::async_write_header(stream, serializer,
                                      boost::asio::redirect_error(boost::asio::use_awaitable, error));
    stream.expires_never();
    co_return !error;
}

/// Opens the SSE stream unless it is open, and counts it; returns false when the client is gone.
Task<bool> ensure_stream_open(StreamConnection& stream, const StringRequest& request,
                              const HttpConnectionLimits& limits, StreamCounters& counters,
                              bool& stream_open) {
    if (!stream_open) {
        // Awaited first: with the co_await in the condition, GCC 12 builds a coroutine that
        // crashes when it resumes.
        const bool opened = co_await open_stream(stream, request, limits);
        if (!opened) {
            co_return false;
        }
        stream_open = true;
        counters.streams_opened.fetch_add(1, std::memory_order_relaxed);
    }
    co_return true;
}

/// Waits for a wake for at most @p timeout; returns true when the time ran out.
Task<bool> wait_for_signal(ModernExchange& exchange, std::optional<std::chrono::milliseconds> timeout) {
    if (exchange.woken) {
        exchange.woken = false;
        co_return false;
    }
    if (timeout) {
        exchange.signal.expires_after(*timeout);
    } else {
        exchange.signal.expires_at(std::chrono::steady_clock::time_point::max());
    }
    boost::system::error_code error;
    co_await exchange.signal.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
    exchange.woken = false;
    co_return !error;
}

/// Whether the client reset a connection whose sending side it had already closed. epoll ends the
/// wait for a socket error on a reset, but kqueue and the Windows reactor end it only for
/// out-of-band data, so the socket is asked instead: for an error it has recorded, and with a
/// peek, which past the client's end of stream reads end of file until a reset.
bool peer_reset(StreamConnection& stream) { return probe_peer(stream.socket()) == PeerState::eReset; }

}  // namespace

std::shared_ptr<ModernExchange> start_modern_exchange(const StringRequest& request,
                                                      nlohmann::json request_json,
                                                      std::string principal, const ModernStart& start,
                                                      ModernDispatchRef dispatch) {
    const auto accept_it = request.find(http::field::accept);
    const bool streams =
        !start.json_only && request.version() >= 11 && accept_it != request.end() &&
        to_std_string_view(accept_it->value()).find("text/event-stream") != std::string_view::npos;
    auto exchange = std::make_shared<ModernExchange>(start.strand, start.options, streams, start.budget,
                                                     start.budget_limit);
    const bool listening = request_json.at("method") == "subscriptions/listen";

    RequestContext request_context;
    request_context.cancelled = exchange->cancelled;
    request_context.principal = std::move(principal);
    if (start.check_headers) {
        request_context.param_headers = mcp_param_headers(request);
    }
    request_context.watch_cancel = [weak = std::weak_ptr(exchange)](std::function<void()> hook) {
        if (auto target = weak.lock()) {
            target->set_on_cancel(std::move(hook));
        }
    };
    if (streams) {
        // A listen stream that loses a notification ends, so the client reconnects and lists again
        // rather than carry on unaware of the change.
        request_context.notifications = [weak = std::weak_ptr(exchange), counters = start.counters,
                                         listening](std::string_view message) {
            if (auto target = weak.lock(); target && !target->push_notification(std::string(message))) {
                counters->dropped_notifications.fetch_add(1, std::memory_order_relaxed);
                if (listening) {
                    target->cancel();
                }
            }
            return notification_queued();
        };
    }

    // The server answers on the dispatch executor while the connection watches the client, so a
    // client that goes away cancels the request.
    auto request_id = request_json.at("id");
    auto answering = dispatch(std::move(request_json), std::move(request_context));
    boost::asio::co_spawn(
        start.dispatch_executor, std::move(answering),
        [exchange, request_id = std::move(request_id)](std::exception_ptr error, std::string wire) {
            if (error) {
                wire = mcp::detail::dump_json(
                    nlohmann::json{{"jsonrpc", "2.0"},
                                   {"id", request_id},
                                   {"error",
                                    {{"code", mcp::constants::jsonrpc::g_INTERNAL_ERROR},
                                     {"message", "Internal error"}}}});
            }
            exchange->finish(std::move(wire));
        });
    return exchange;
}

Task<bool> serve_modern_exchange(StreamConnection& stream, boost::beast::flat_buffer& buffer,
                                 const StringRequest& request, std::shared_ptr<ModernExchange> exchange,
                                 bool& client_finished_sending, const ModernServing& serving,
                                 std::vector<char>& response_head) {
    const auto& options = serving.options;
    const auto& limits = serving.limits;
    // Whatever way the request ends, the queue gives back its share of the memory budget.
    struct QueueEnd {
        ModernExchange& exchange;
        ~QueueEnd() { exchange.end_queue(); }
    };
    const QueueEnd queue_end{*exchange};
    struct Watch {
        bool armed{false};
        bool completed{false};
        boost::system::error_code error;
    };
    auto watch = std::make_shared<Watch>();
    const auto arm_watch = [&stream, &buffer, &watch, &exchange] {
        watch->armed = true;
        watch->completed = false;
        stream.async_read_some(
            buffer.prepare(mcp::detail::constants::modern_exchange::g_WATCH_READ_SIZE),
            boost::asio::bind_executor(
                exchange->strand,
                [watch, exchange, &buffer](const boost::system::error_code& error, std::size_t bytes) {
                    if (!error) {
                        buffer.commit(bytes);
                    }
                    watch->armed = false;
                    watch->completed = true;
                    watch->error = error;
                    exchange->wake_on_strand();
                }));
    };

    // Once the client's end of the stream is read there is nothing more to read, but a reset
    // or this side closing the socket still ends the wait, and so wakes the loop.
    const auto arm_error_wait = [&stream, &watch, &exchange] {
        watch->armed = true;
        watch->completed = false;
        stream.socket().async_wait(
            boost::asio::ip::tcp::socket::wait_error,
            boost::asio::bind_executor(
                exchange->strand, [watch, exchange](const boost::system::error_code& error) {
                    watch->armed = false;
                    watch->completed = true;
                    watch->error = error ? error : boost::asio::error::connection_reset;
                    exchange->wake_on_strand();
                }));
    };

    bool stream_open = false;
    bool half_closed = client_finished_sending;
    // The client sent more than the server will hold, so the read that watches it has stopped.
    bool read_ahead_full = false;
    bool client_gone = false;
    if (half_closed) {
        arm_error_wait();
    } else {
        arm_watch();
    }

    for (;;) {
        std::vector<std::string> notifications;
        std::optional<std::string> response;
        std::size_t queued_bytes = 0;
        {
            std::lock_guard lock(exchange->mutex);
            notifications.swap(exchange->notifications);
            response.swap(exchange->response);
            queued_bytes = exchange->queued_bytes;
        }

        if (serving.closed.load(std::memory_order_acquire)) {
            client_gone = true;
            break;
        }

        if (!notifications.empty() && exchange->streams) {
            if (!co_await ensure_stream_open(stream, request, limits, serving.counters, stream_open)) {
                client_gone = true;
                break;
            }
            std::string events;
            for (const auto& notification : notifications) {
                events += "data: ";
                events += notification;
                events += "\n\n";
            }
            const bool written = co_await write_chunk(stream, events, limits);
            if (!written) {
                client_gone = true;
                break;
            }
            exchange->release_queued(queued_bytes);
        }

        if (response) {
            if (watch->armed) {
                boost::system::error_code ignored;
                (void)stream.socket().cancel(ignored);
                while (watch->armed) {
                    co_await wait_for_signal(*exchange, std::nullopt);
                }
            }
            if (watch->completed && watch->error == boost::asio::error::eof) {
                half_closed = true;
                client_finished_sending = true;
            }
            const auto error_code = serialized_response_error_code(*response);
            if (error_code == mcp::constants::jsonrpc::g_HEADER_MISMATCH) {
                serving.header_mismatch_rejections.fetch_add(1, std::memory_order_relaxed);
            }
            if (stream_open) {
                std::string final_event = "data: " + *response + "\n\n";
                const bool written = co_await write_chunk(stream, final_event, limits);
                bool ended = false;
                if (written) {
                    ended = co_await write_last_chunk(stream, limits);
                }
                co_return written && ended && request.keep_alive() &&
                    (!half_closed || buffer.size() != 0);
            }
            const auto status = error_code ? http_status_for_error(*error_code) : http::status::ok;
            auto plain = json_response(request, status, std::move(*response));
            const bool written =
                co_await write_response_within_limits(stream, plain, limits, response_head);
            co_return written&& plain.keep_alive() && (!half_closed || buffer.size() != 0);
        }

        if (watch->completed) {
            watch->completed = false;
            if (!watch->error) {
                if (buffer.size() < mcp::detail::constants::modern_exchange::g_MAX_READ_AHEAD) {
                    arm_watch();
                } else {
                    read_ahead_full = true;
                    arm_error_wait();
                }
            } else if (watch->error == boost::asio::error::eof) {
                half_closed = true;
                client_finished_sending = true;
                arm_error_wait();
            } else {
                client_gone = true;
                break;
            }
        }

        // A stream's keep-alives find a client that left; anything else has to ask the socket.
        const bool probe_socket = half_closed || (read_ahead_full && !stream_open);
        std::optional<std::chrono::milliseconds> timeout;
        if (probe_socket) {
            timeout = options.disconnect_probe_interval;
        } else if (stream_open) {
            timeout = options.keepalive_interval;
        }
        const bool timed_out = co_await wait_for_signal(*exchange, timeout);
        if (!timed_out) {
            continue;
        }
        if (half_closed && exchange->streams) {
            // A client that closed cleanly cannot be told from one that only half-closed until a
            // write fails, so the answer becomes a stream that can be probed.
            if (!co_await ensure_stream_open(stream, request, limits, serving.counters, stream_open)) {
                client_gone = true;
                break;
            }
            serving.counters.half_close_probes.fetch_add(1, std::memory_order_relaxed);
            const bool probed = co_await write_chunk(stream, ": probe\n\n", limits);
            if (!probed) {
                client_gone = true;
                break;
            }
        } else if (probe_socket) {
            if (peer_reset(stream)) {
                client_gone = true;
                break;
            }
        } else if (stream_open) {
            const bool kept_alive = co_await write_chunk(stream, ": keep-alive\n\n", limits);
            if (!kept_alive) {
                client_gone = true;
                break;
            }
        }
    }

    if (client_gone) {
        exchange->cancel();
        serving.counters.disconnect_cancels.fetch_add(1, std::memory_order_relaxed);
    }
    if (watch->armed) {
        boost::system::error_code ignored;
        (void)stream.socket().cancel(ignored);
        while (watch->armed) {
            co_await wait_for_signal(*exchange, std::nullopt);
        }
    }
    co_return false;
}

}  // namespace mcp::detail
