#include "mcp/detail/net/network_failure.hpp"

#include <gtest/gtest.h>

#include "support/network_error.hpp"

#include <boost/asio/error.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <stdexcept>
#include <string>

namespace network_failure_test {

using Kind = mcp::TransportError::Kind;
using mcp::detail::network_failure;

TEST(NetworkFailure, ACancelledOperationIsAClosedTransport) {
    const auto error = network_failure(boost::asio::error::operation_aborted, Kind::eIo, "T");
    EXPECT_TRUE(
        mcp_test::is_network_error(error, Kind::eClosed, boost::asio::error::operation_aborted));
    EXPECT_STREQ(error.what(), "T is closed");
}

TEST(NetworkFailure, APeerThatClosedOrResetIsAClosedTransport) {
    for (const boost::system::error_code code :
         {boost::system::error_code(boost::asio::error::eof),
          boost::system::error_code(boost::asio::error::connection_reset),
          boost::system::error_code(boost::asio::error::broken_pipe)}) {
        const auto error = network_failure(code, Kind::eIo, "T");
        EXPECT_TRUE(mcp_test::is_network_error(error, Kind::eClosed, code)) << error.what();
        EXPECT_EQ(std::string(error.what()), "T lost its connection: " + code.message());
    }
}

TEST(NetworkFailure, AnyErrorWhileConnectingIsAConnectFailure) {
    for (const boost::system::error_code code :
         {boost::system::error_code(boost::asio::error::connection_refused),
          boost::system::error_code(boost::asio::error::eof),
          boost::system::error_code(boost::beast::error::timeout)}) {
        const auto error = network_failure(code, Kind::eConnect, "T");
        EXPECT_TRUE(mcp_test::is_network_error(error, Kind::eConnect, code)) << error.what();
        EXPECT_EQ(std::string(error.what()), "T could not connect: " + code.message());
    }
}

TEST(NetworkFailure, AnotherErrorOnAnOpenConnectionIsAnIoFailure) {
    const boost::system::error_code code = boost::beast::error::timeout;
    const auto error = network_failure(code, Kind::eIo, "T");
    EXPECT_TRUE(mcp_test::is_network_error(error, Kind::eIo, code));
    EXPECT_EQ(std::string(error.what()), "T failed to read or write: " + code.message());
}

TEST(NetworkFailure, AMessageOverTheLimitIsAProtocolError) {
    for (const boost::system::error_code code :
         {boost::system::error_code(boost::beast::websocket::error::message_too_big),
          boost::system::error_code(boost::beast::http::error::body_limit)}) {
        const auto error = network_failure(code, Kind::eIo, "T");
        EXPECT_TRUE(mcp_test::is_network_error(error, Kind::eProtocol, code)) << error.what();
        EXPECT_EQ(std::string(error.what()),
                  "T received a message larger than its limit: " + code.message());
    }
}

TEST(NetworkFailure, OnlyANetworkErrorIsTurnedIntoATransportError) {
    const auto converted = mcp::detail::as_transport_failure(
        std::make_exception_ptr(boost::system::system_error(boost::asio::error::connection_reset)),
        "T");
    try {
        std::rethrow_exception(converted);
    } catch (const mcp::TransportError& error) {
        EXPECT_TRUE(
            mcp_test::is_network_error(error, Kind::eClosed, boost::asio::error::connection_reset));
    }

    const auto other = std::make_exception_ptr(std::invalid_argument("bad call"));
    EXPECT_EQ(mcp::detail::as_transport_failure(other, "T"), other);
    EXPECT_EQ(mcp::detail::as_transport_failure(nullptr, "T"), nullptr);
}

}  // namespace network_failure_test
