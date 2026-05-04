Testing MCP Applications
========================

This guide shows how to test MCP servers and clients that you build using the mcp-cpp-sdk. The SDK provides specialized tools and patterns to make testing asynchronous, bidirectional communication straightforward and deterministic.

Overview
--------

When testing your MCP applications, you should focus on three levels of verification:

1.  **Unit Tests**: Testing individual tool handlers, resource providers, or middleware in isolation.
2.  **Integration Tests**: Testing the interaction between your Server and a Client using in-memory communication.
3.  **End-to-End (E2E) Tests**: Testing the full stack, including real I/O (Stdio, HTTP) and external dependencies.

This guide focuses primarily on Integration and E2E testing using the built-in :cpp:class:`mcp::MemoryTransport`.

MemoryTransport for Testing
---------------------------

The :cpp:class:`mcp::MemoryTransport` is an in-process, bidirectional transport that allows a Server and a Client to communicate without any network or process overhead. It's the recommended way to write fast, reliable integration tests.

Key features of MemoryTransport:

- **In-Process**: No sockets or pipes required.
- **Asynchronous**: Fully supports C++20 coroutines.
- **Deterministic**: Messages are delivered in order via a shared event loop.
- **Thread-Safe**: Uses `boost::asio::strand` for internal synchronization.

Using create_memory_transport_pair()
------------------------------------

To use MemoryTransport, you should always use the helper function :cpp:func:`mcp::create_memory_transport_pair`. This function creates two connected transport instances where messages written to one are immediately available to the other.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :start-after: // Create bidirectional memory transport pair
   :end-before: std::cout << "[Main] Created MemoryTransport pair

Testing Patterns
----------------

Integration testing typically follows a "loopback" pattern where both the server and client run in the same process, often within the same `io_context`. This approach is highly efficient because it eliminates the latency of external networks or process communication while still exercising the full MCP protocol logic.

Loopback Test Pattern
~~~~~~~~~~~~~~~~~~~~~

A typical loopback test follows these steps:

1.  Create an `asio::io_context` to manage the event loop.
2.  Create a MemoryTransport pair using :cpp:func:`mcp::create_memory_transport_pair`.
3.  Initialize a :cpp:class:`mcp::Server` with the desired tools, resources, and capabilities.
4.  Initialize a :cpp:class:`mcp::Client` using one half of the transport pair.
5.  Spawn a coroutine using `asio::co_spawn` to run the server's event loop in the background.
6.  Spawn a separate coroutine to execute client requests, using `co_await` to wait for responses, and then use your testing framework (like GoogleTest) to verify the results.
7.  Run the `io_context` until all work is complete or a timeout occurs.

Here is a simplified example of this pattern from our integration test suite:

.. literalinclude:: ../../test/e2e_integration_test.cpp
   :language: cpp
   :lines: 42-75

Writing Scripted Tests
----------------------

Scripted tests are a way to verify a complex sequence of interactions between a client and a server. Since the SDK is built on coroutines, you can write tests that look like synchronous code but execute asynchronously. This makes the tests easy to read and maintain.

When writing scripted tests, consider the following:

- **State Management**: Ensure that your server is fully initialized before the client starts sending requests. A common pattern is to include a small delay or use a signal to indicate server readiness.
- **Concurrent Operations**: You can test how the server handles multiple simultaneous requests by spawning multiple client coroutines.
- **Protocol Violations**: You can intentionally send malformed messages to verify that your server responds with the appropriate MCP error codes.

.. code-block:: cpp

    boost::asio::co_spawn(io_ctx, [&]() -> mcp::Task<void> {
        // 1. Connect and verify version handshake
        auto init_res = co_await client->connect(client_info, client_caps);
        EXPECT_EQ(init_res.serverInfo.version, "1.0");

        // 2. Call a tool and verify the content
        auto result = co_await client->call_tool("echo", {{"message", "test"}});
        EXPECT_EQ(result.content[0]["text"], "test");

        // 3. Verify server-side state if accessible
        EXPECT_TRUE(server->has_tool("echo"));

        // 4. Gracefully close the connection
        client->close();
    }, boost::asio::detached);

E2E Testing Patterns
--------------------

For full E2E tests, you can use real transports like :cpp:class:`mcp::StdioTransport` or :cpp:class:`mcp::HttpClientTransport`. These tests are essential for verifying that your server correctly handles serialization, platform-specific I/O, and real-world networking conditions.

Key considerations for E2E tests:

- **Environment Setup**: Ensure that any external servers or tools your MCP server relies on are available and in a known state.
- **Cleanup**: Use teardown logic to kill child processes or close open sockets to avoid leaking resources between test runs.
- **Port Management**: When testing network transports, use ephemeral ports to avoid collisions in parallel CI environments.

When running E2E tests in CI, prefer MemoryTransport for most logic verification, and reserve real I/O tests for a smaller set of smoke tests to keep the test suite fast.

Mocking and Stubbing
--------------------

Because the SDK uses interfaces for transports, you can easily mock :cpp:class:`mcp::ITransport` to simulate network errors, timeouts, or malformed protocol messages. This allows you to test edge cases that are difficult to trigger with the standard transports.

For tool handlers, you can pass mock dependencies into your lambda captures or function objects to verify that tools interact with your business logic correctly. This is particularly useful for testing tools that interact with databases, external APIs, or the filesystem.

CI Integration
--------------

MCP tests can be integrated into any standard C++ CI pipeline. Since the SDK relies on Boost.Asio and C++20, ensure your CI environment has a compatible compiler (GCC 11+, Clang 13+, or MSVC 2022+) and the necessary libraries installed.

We recommend running tests under sanitizers (ASan, UBSan) to catch memory leaks or undefined behavior in your asynchronous code.

Example GitHub Action snippet:

.. code-block:: yaml

    - name: Build and Test
      run: |
        python scripts/build.py --test --sanitize
        cd build/release && ctest --output-on-failure

Best Practices
--------------

- **Use Strands**: When sharing objects between coroutines or interacting with shared state, use `boost::asio::strand` to prevent race conditions and ensure thread safety.
- **Timeout your Tests**: Always use a timeout (e.g., `boost::asio::steady_timer`) in your test `io_context`. This prevents your CI pipeline from hanging indefinitely if a coroutine fails to complete.
- **Verify Error Paths**: Don't just test the happy path. Use MemoryTransport to verify how your client handles server-side exceptions, invalid tool arguments, or resource not found errors.
- **Keep Tests Small and Focused**: Each integration test should focus on a single feature, tool, or specific interaction flow. This makes it easier to identify the cause of a failure.
- **Clean Up Resources**: Ensure `io_context::stop()` is called or all pending work is finished so your test binary exits cleanly.
- **Log Failures**: Use the `Context::log_*` methods to capture server-side state during test runs, which can be invaluable when debugging failures in headless CI environments.
