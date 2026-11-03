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

/// Starts the deadline for reading one request, or clears it when @p limits sets none.
inline void arm_request_read_deadline(boost::beast::tcp_stream& stream,
                                      const HttpConnectionLimits& limits) {
    if (limits.request_read_timeout > std::chrono::milliseconds::zero()) {
        stream.expires_after(limits.request_read_timeout);
    } else {
        stream.expires_never();
    }
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
/// body is charged to @p bytes by its declared length, or by @p body_limit when it declares none.
/// Returns false when the connection is to end, after writing whatever answer is due.
inline Task<bool> read_request_within_limits(
    boost::beast::tcp_stream& stream, boost::beast::flat_buffer& buffer,
    boost::beast::http::request_parser<boost::beast::http::string_body>& parser,
    HttpConnectionLimits limits, std::size_t body_limit, BudgetLease& bytes,
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
    const auto declared = parser.content_length();
    const std::size_t body_bytes =
        declared ? static_cast<std::size_t>(std::min<std::uint64_t>(*declared, body_limit))
                 : body_limit;
    if (!bytes.grow_to(body_bytes)) {
        stream.expires_never();
        // Named: GCC 11 mishandles a temporary in a co_await operand.
        auto busy = make_limit_response(head, http::status::service_unavailable, "Server busy");
        co_await write_final_response(stream, std::move(busy));
        co_return false;
    }
    bool too_large = false;
    try {
        if (!parser.is_done()) {
            co_await http::async_read(stream, buffer, parser, boost::asio::use_awaitable);
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
