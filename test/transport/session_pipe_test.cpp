#include "mcp/detail/transport/session_pipe.hpp"
#include "mcp/core/async.hpp"
#include "mcp/detail/async/strand_executor.hpp"
#include "mcp/transport/transport.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace session_pipe_test {

namespace {

using nlohmann::json;

struct Routed {
    std::string message;
    std::optional<std::string> related_request;
};

class SessionPipeTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_;
    boost::asio::any_io_executor strand_{mcp::detail::make_strand_executor(io_.get_executor())};
    std::vector<Routed> routed_;
    int closes_{0};
    std::shared_ptr<mcp::detail::SessionPipe> pipe_{std::make_shared<mcp::detail::SessionPipe>(
        strand_,
        [this](std::string message, const std::string* related_request) {
            routed_.push_back({std::move(message), related_request != nullptr
                                                       ? std::optional<std::string>(*related_request)
                                                       : std::nullopt});
        },
        [this] { ++closes_; })};

    /// Reads from the pipe on its strand until it ends, collecting what it reads.
    void read_all(std::vector<json>& read) {
        mcp_test::spawn_detached(strand_, [this, &read]() -> mcp::Task<void> {
            while (auto message = co_await pipe_->next_message()) {
                read.push_back(std::move(*message));
            }
        });
    }
};

}  // namespace

TEST_F(SessionPipeTest, MessagesDeliveredBeforeTheReaderWaitsAreReadInOrder) {
    pipe_->deliver(json{{"n", 1}});
    pipe_->deliver(json{{"n", 2}});
    pipe_->close();
    std::vector<json> read;
    read_all(read);
    io_.run();
    ASSERT_EQ(read.size(), 2U);
    EXPECT_EQ(read[0]["n"], 1);
    EXPECT_EQ(read[1]["n"], 2);
}

TEST_F(SessionPipeTest, DeliveryWakesAWaitingReader) {
    std::vector<json> read;
    read_all(read);
    mcp_test::spawn_detached(io_.get_executor(), [this, &read]() -> mcp::Task<void> {
        // The reader is waiting by the time each message comes.
        boost::asio::steady_timer pause(io_.get_executor());
        for (int n = 1; n <= 3; ++n) {
            pause.expires_after(std::chrono::milliseconds(5));
            co_await pause.async_wait(boost::asio::use_awaitable);
            EXPECT_EQ(read.size(), static_cast<std::size_t>(n - 1));
            pipe_->deliver(json{{"n", n}});
        }
        pause.expires_after(std::chrono::milliseconds(5));
        co_await pause.async_wait(boost::asio::use_awaitable);
        pipe_->close();
    });
    io_.run();
    ASSERT_EQ(read.size(), 3U);
    EXPECT_EQ(read[2]["n"], 3);
}

TEST_F(SessionPipeTest, CloseEndsTheReaderAfterWhatWasDeliveredAndRefusesMore) {
    pipe_->deliver(json{{"n", 1}});
    pipe_->close();
    try {
        pipe_->deliver(json{{"n", 2}});
        ADD_FAILURE() << "deliver() accepted a message after close()";
    } catch (const mcp::TransportError& error) {
        EXPECT_EQ(error.kind(), mcp::TransportError::Kind::eClosed);
    }
    std::vector<json> read;
    read_all(read);
    io_.run();
    ASSERT_EQ(read.size(), 1U);
    EXPECT_EQ(read[0]["n"], 1);
}

TEST_F(SessionPipeTest, WritesReachTheRouterInOrderWithTheirRequest) {
    bool refused_after_close = false;
    mcp_test::spawn_detached(strand_, [this, &refused_after_close]() -> mcp::Task<void> {
        co_await pipe_->write_message(R"({"method":"notifications/message"})");
        co_await pipe_->write_related_message(R"({"method":"notifications/progress"})", "7");
        co_await pipe_->write_message(R"({"id":7,"result":{}})");
        pipe_->close();
        try {
            co_await pipe_->write_message(R"({"method":"late"})");
        } catch (const mcp::TransportError& error) {
            refused_after_close = error.kind() == mcp::TransportError::Kind::eClosed;
        }
    });
    io_.run();
    ASSERT_EQ(routed_.size(), 3U);
    EXPECT_EQ(routed_[0].message, R"({"method":"notifications/message"})");
    EXPECT_FALSE(routed_[0].related_request.has_value());
    EXPECT_EQ(routed_[1].related_request, "7");
    EXPECT_EQ(routed_[2].message, R"({"id":7,"result":{}})");
    EXPECT_TRUE(refused_after_close);
}

TEST_F(SessionPipeTest, EitherSideClosingCallsTheHandlerOnce) {
    pipe_->close();
    pipe_->close();
    EXPECT_EQ(closes_, 1);
}

}  // namespace session_pipe_test
