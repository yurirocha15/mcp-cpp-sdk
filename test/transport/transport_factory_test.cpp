#include "mcp/transport/transport_factory.hpp"

#include <gtest/gtest.h>

#include "mcp/core/runtime.hpp"

namespace transport_factory_test {

namespace {

// The legacy free functions are deprecated; this one test proves they still work.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

TEST(DeprecatedTransportFunctionsTest, StillCreateTransports) {
    mcp::Runtime runtime;
    EXPECT_NE(mcp::make_stdio_transport(runtime), nullptr);
    EXPECT_NE(mcp::make_http_client_transport(runtime, "http://localhost:9999/mcp"), nullptr);
    EXPECT_THROW((void)mcp::make_http_client_transport(runtime, "ftp://bad"), std::invalid_argument);
}

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

}  // namespace

}  // namespace transport_factory_test
