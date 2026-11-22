#pragma once

#include "hang_guard.hpp"
#include "spawn.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace mcp_test {

/// Sends one request straight to the server's dispatcher and returns the response.
inline nlohmann::json dispatch_direct(mcp::Server& server, nlohmann::json request) {
    boost::asio::io_context io_ctx;
    nlohmann::json response;
    mcp_test::spawn_then(io_ctx, server.dispatch_request_direct(std::move(request)),
                         [&response](std::exception_ptr error, std::string wire) {
                             EXPECT_EQ(error, nullptr);
                             if (!error) {
                                 response = nlohmann::json::parse(wire);
                             }
                         });
    io_ctx.run();
    return response;
}

/// Runs a 2026-07-28 client, connected with @p capabilities, against @p server over a memory
/// transport and calls @p body with the client and what connect() learned. Fails the test when
/// @p body does not finish within the hang guard.
inline void run_modern_client(
    mcp::Server& server, const mcp::ClientCapabilities& capabilities,
    std::function<mcp::Task<void>(mcp::Client&, const mcp::ConnectResult&)> body) {
    boost::asio::io_context io_ctx;
    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx.get_executor());
    boost::asio::steady_timer guard(io_ctx, g_hang_guard);
    bool timed_out = false;
    guard.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            timed_out = true;
            io_ctx.stop();
        }
    });
    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        try {
            co_await server.run(server_transport, io_ctx.get_executor());
        } catch (const std::exception&) {
        }
    });
    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        mcp::ClientOptions options;
        options.protocol = mcp::ClientProtocol::pinned("2026-07-28");
        mcp::Client client(client_transport, io_ctx.get_executor(), options);
        try {
            // Named: GCC 11 mishandles a braced temporary in a co_await operand.
            mcp::Implementation identity{"extension-test-client", "1.0"};
            const auto connected = co_await client.connect(identity, capabilities);
            co_await body(client, connected);
        } catch (const std::exception& error) {
            ADD_FAILURE() << "unexpected exception: " << error.what();
        }
        client.close();
        guard.cancel();
    });
    io_ctx.run();
    EXPECT_FALSE(timed_out) << "the test did not finish";
}

}  // namespace mcp_test
