#include "mcp/transport_factory.hpp"

#include <gtest/gtest.h>

#include "mcp/runtime.hpp"

namespace {

TEST(TransportFactoryTest, MakeStdioTransportReturnsNonNull) {
    mcp::Runtime runtime;
    auto transport = mcp::make_stdio_transport(runtime);
    ASSERT_NE(transport, nullptr);
}

TEST(TransportFactoryTest, MakeHttpClientTransportReturnsNonNull) {
    mcp::Runtime runtime;
    auto transport = mcp::make_http_client_transport(runtime, "http://localhost:9999/mcp");
    ASSERT_NE(transport, nullptr);
}

TEST(TransportFactoryTest, MakeHttpClientTransportThrowsOnInvalidUrl) {
    mcp::Runtime runtime;
    EXPECT_THROW(mcp::make_http_client_transport(runtime, "ftp://bad"), std::invalid_argument);
}

}  // namespace
