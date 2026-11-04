#pragma once

#include <mcp/core/core.hpp>
#include <mcp/transport/http_types.hpp>

#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace mcp::detail {

/// The connection limit to use for @p configured, which is zero for the default: three quarters of
/// the descriptor limit, so the process keeps descriptors for everything else it does.
inline std::size_t resolved_max_connections(std::size_t configured) {
    if (configured != 0) {
        return configured;
    }
    constexpr std::size_t fallback = 4096;
#if !defined(_WIN32)
    rlimit descriptors{};
    if (::getrlimit(RLIMIT_NOFILE, &descriptors) == 0 && descriptors.rlim_cur != RLIM_INFINITY) {
        const auto limit = static_cast<std::size_t>(descriptors.rlim_cur);
        return limit / 4 * 3 > 0 ? limit / 4 * 3 : 1;
    }
#endif
    return fallback;
}

/// What the connections of one listener hold together.
struct HttpListenerBudget {
    std::atomic<std::size_t> connections{0};
    std::atomic<std::size_t> requests{0};
    std::atomic<std::size_t> bytes{0};
};

/// A share of a budget kept in an atomic counter, given back when the lease ends.
class BudgetLease {
   public:
    BudgetLease(std::atomic<std::size_t>& counter, std::size_t limit) noexcept
        : counter_(&counter), limit_(limit) {}
    ~BudgetLease() { release(); }

    BudgetLease(const BudgetLease&) = delete;
    BudgetLease& operator=(const BudgetLease&) = delete;
    BudgetLease(BudgetLease&&) = delete;
    BudgetLease& operator=(BudgetLease&&) = delete;

    /// Holds @p amount in all, taking what is missing from the budget. False, holding what it held
    /// before, when that would take the budget past its limit.
    [[nodiscard]] bool grow_to(std::size_t amount) noexcept {
        if (amount <= held_) {
            return true;
        }
        const auto extra = amount - held_;
        auto current = counter_->load(std::memory_order_relaxed);
        do {
            if (current > limit_ || extra > limit_ - current) {
                return false;
            }
        } while (!counter_->compare_exchange_weak(current, current + extra, std::memory_order_relaxed));
        held_ = amount;
        return true;
    }

    void release() noexcept {
        if (held_ != 0) {
            counter_->fetch_sub(held_, std::memory_order_relaxed);
            held_ = 0;
        }
    }

   private:
    std::atomic<std::size_t>* counter_;
    std::size_t limit_;
    std::size_t held_{0};
};

/// A string body that grows as the body arrives. Beast's string_body reserves the declared
/// Content-Length before the first byte of the body, so a peer could hold that memory by sending
/// headers alone; this one holds only what was sent.
struct ArrivingStringBody {
    using value_type = std::string;

    static std::uint64_t size(const value_type& body) { return body.size(); }

    class reader {
       public:
        template <bool isRequest, class Fields>
        reader(boost::beast::http::header<isRequest, Fields>& /*header*/, value_type& body)
            : body_(body) {}

        void init(const boost::optional<std::uint64_t>& /*length*/, boost::beast::error_code& error) {
            error = {};
        }

        template <class ConstBufferSequence>
        std::size_t put(const ConstBufferSequence& buffers, boost::beast::error_code& error) {
            const auto extra = boost::asio::buffer_size(buffers);
            if (extra > body_.max_size() - body_.size()) {
                error = boost::beast::http::error::buffer_overflow;
                return 0;
            }
            for (auto it = boost::asio::buffer_sequence_begin(buffers);
                 it != boost::asio::buffer_sequence_end(buffers); ++it) {
                const boost::asio::const_buffer buffer = *it;
                body_.append(static_cast<const char*>(buffer.data()), buffer.size());
            }
            error = {};
            return extra;
        }

        void finish(boost::beast::error_code& error) { error = {}; }

       private:
        value_type& body_;
    };
};

using RequestParser = boost::beast::http::request_parser<ArrivingStringBody>;

/// The request @p parser has read, moved out of it.
inline StringRequest take_request(RequestParser& parser) {
    auto request = parser.release();
    return StringRequest(std::move(request.base()), std::move(request.body()));
}

/// Starts the deadline for reading one request, or clears it when @p limits sets none.
inline void arm_request_read_deadline(boost::beast::tcp_stream& stream,
                                      const HttpConnectionLimits& limits) {
    if (limits.request_read_timeout > std::chrono::milliseconds::zero()) {
        stream.expires_after(limits.request_read_timeout);
    } else {
        stream.expires_never();
    }
}

/// Writes @p response within the time @p limits gives reading a request, so a peer that does not
/// take its answer cannot keep what its request holds. False when the peer is gone or too slow, and
/// the connection is to end.
inline Task<bool> write_response_within_limits(boost::beast::tcp_stream& stream,
                                               StringResponse& response,
                                               const HttpConnectionLimits& limits) {
    arm_request_read_deadline(stream, limits);
    try {
        co_await boost::beast::http::async_write(stream, response, boost::asio::use_awaitable);
    } catch (const boost::system::system_error&) {
        co_return false;
    }
    stream.expires_never();
    co_return true;
}

/// Writes @p response and ends the exchange on this connection, ignoring a peer that is gone.
inline Task<void> write_final_response(boost::beast::tcp_stream& stream, StringResponse response) {
    response.keep_alive(false);
    response.prepare_payload();
    try {
        co_await boost::beast::http::async_write(stream, response, boost::asio::use_awaitable);
    } catch (const boost::system::system_error&) {
        // The peer may already have left; there is nobody left to tell.
    }
}

inline StringResponse make_limit_response(const StringRequest& request,
                                          boost::beast::http::status status,
                                          const std::string& message) {
    StringResponse response{status, request.version()};
    response.set(boost::beast::http::field::server, "mcp-cpp-sdk");
    response.set(boost::beast::http::field::content_type, "application/json");
    response.body() = std::string(R"({"error":")") + message + R"("})";
    return response;
}

/// What a server checks on a request's headers alone, before its body is read: an answer that
/// refuses it, or nothing to go on.
using RefuseBeforeBody = std::function<Task<std::optional<StringResponse>>(const StringRequest&)>;

/// Reads one request into @p parser within @p limits. The headers are read first and given to
/// @p refuse_before_body, so a request that is refused never has its body read or held; then the
/// body is charged to @p bytes as it arrives.
/// Returns false when the connection is to end, after writing whatever answer is due.
inline Task<bool> read_request_within_limits(boost::beast::tcp_stream& stream,
                                             boost::beast::flat_buffer& buffer, RequestParser& parser,
                                             HttpConnectionLimits limits, std::size_t body_limit,
                                             BudgetLease& bytes,
                                             const RefuseBeforeBody& refuse_before_body) {
    namespace http = boost::beast::http;
    const auto ends_quietly = [](const boost::system::error_code& code) {
        return code == boost::asio::error::eof || code == boost::asio::error::connection_reset ||
               code == boost::asio::error::operation_aborted || code == boost::beast::error::timeout ||
               code == http::error::end_of_stream;
    };
    arm_request_read_deadline(stream, limits);
    bool declared_too_large = false;
    try {
        co_await http::async_read_header(stream, buffer, parser, boost::asio::use_awaitable);
    } catch (const boost::system::system_error& error) {
        if (ends_quietly(error.code())) {
            co_return false;
        }
        // A Content-Length past the limit is refused as soon as the headers are read.
        if (error.code() != http::error::body_limit) {
            throw;
        }
        declared_too_large = true;
    }
    const StringRequest head{parser.get().base()};
    if (declared_too_large) {
        stream.expires_never();
        auto too_large_response =
            make_limit_response(head, http::status::payload_too_large, "Request body too large");
        co_await write_final_response(stream, std::move(too_large_response));
        co_return false;
    }
    if (auto refusal = co_await refuse_before_body(head)) {
        stream.expires_never();
        // The body was not read, so the next request cannot be found on this connection.
        co_await write_final_response(stream, std::move(*refusal));
        co_return false;
    }
    // The body is charged as it arrives, so a request holds only what its peer has sent.
    bool too_large = false;
    bool busy = false;
    try {
        while (!parser.is_done()) {
            co_await http::async_read_some(stream, buffer, parser, boost::asio::use_awaitable);
            if (!bytes.grow_to(parser.get().body().size())) {
                busy = true;
                break;
            }
        }
    } catch (const boost::system::system_error& error) {
        if (ends_quietly(error.code())) {
            co_return false;
        }
        if (error.code() != http::error::body_limit) {
            throw;
        }
        too_large = true;
    }
    stream.expires_never();
    if (busy) {
        // Named: GCC 11 mishandles a temporary in a co_await operand.
        auto busy_response =
            make_limit_response(head, http::status::service_unavailable, "Server busy");
        co_await write_final_response(stream, std::move(busy_response));
        co_return false;
    }
    if (too_large) {
        // The parser cannot resynchronize after a truncated body, so the connection ends here.
        auto too_large_response =
            make_limit_response(head, http::status::payload_too_large, "Request body too large");
        co_await write_final_response(stream, std::move(too_large_response));
        co_return false;
    }
    co_return true;
}

}  // namespace mcp::detail
