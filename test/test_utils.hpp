#pragma once

#include "mcp/server.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// ScriptedTransport — mock ITransport that queues scripted messages and
// captures outgoing writes.  Suspends via a strand-wrapped timer when the
// incoming queue is empty; wakes on enqueue_message() or close().
// ---------------------------------------------------------------------------
class ScriptedTransport final : public mcp::ITransport {
   public:
    explicit ScriptedTransport(const boost::asio::any_io_executor& executor)
        : strand_(boost::asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            if (!incoming_.empty()) {
                auto msg = std::move(incoming_.front());
                incoming_.pop();
                co_return msg;
            }
            timer_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await timer_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        written_.emplace_back(message);
        if (on_write_) {
            on_write_(written_.back());
        }
    }

    void close() override {
        closed_ = true;
        timer_.cancel();
    }

    void enqueue_message(std::string msg) {
        boost::asio::post(strand_, [this, m = std::move(msg)]() mutable {
            incoming_.push(std::move(m));
            timer_.cancel();
        });
    }

    void set_on_write(std::function<void(std::string_view)> callback) {
        on_write_ = std::move(callback);
    }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }
    [[nodiscard]] bool is_closed() const { return closed_; }
    [[nodiscard]] const boost::asio::strand<boost::asio::any_io_executor>& strand() const {
        return strand_;
    }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------
// JSON-RPC message helpers
// ---------------------------------------------------------------------------
inline nlohmann::json make_initialize_request(std::string_view id) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

inline nlohmann::json make_shutdown_request(std::string_view id) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "shutdown"}};
}

inline nlohmann::json make_tool_call_request(std::string_view id, const std::string& tool_name,
                                             nlohmann::json arguments = nlohmann::json::object()) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "tools/call"},
            {"params", {{"name", tool_name}, {"arguments", std::move(arguments)}}}};
}

inline nlohmann::json make_result_response(std::string_view id, nlohmann::json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

inline nlohmann::json make_error_response(std::string_view id, int code, std::string_view message) {
    return {
        {"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::string(message)}}}};
}
