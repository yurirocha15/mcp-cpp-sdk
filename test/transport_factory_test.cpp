#include "mcp/transport_factory.hpp"

#include <gtest/gtest.h>

#include "mcp/runtime.hpp"

namespace {

TEST(TransportFactoryTest, FactoryCreateStdioReturnsNonNull) {
    mcp::Runtime runtime;
    mcp::TransportFactory factory(runtime);
    auto transport = factory.create_stdio();
    ASSERT_NE(transport, nullptr);
}

TEST(TransportFactoryTest, FactoryCreateHttpClientReturnsNonNull) {
    mcp::Runtime runtime;
    mcp::TransportFactory factory(runtime);
    auto transport = factory.create_http_client("http://localhost:9999/mcp");
    ASSERT_NE(transport, nullptr);
}

TEST(TransportFactoryTest, FactoryCreateHttpClientThrowsOnInvalidUrl) {
    mcp::Runtime runtime;
    mcp::TransportFactory factory(runtime);
    EXPECT_THROW(factory.create_http_client("ftp://bad"), std::invalid_argument);
}

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
