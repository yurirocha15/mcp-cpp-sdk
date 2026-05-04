Examples
========

The mcp-cpp-sdk includes several example programs demonstrating different aspects
of the SDK and various usage patterns. All examples are located in the ``examples/``
directory.

Overview
--------

The SDK provides seven core examples plus 12 specialized feature examples:

1. **server_stdio** - Full-featured MCP server over stdio
2. **client_stdio** - MCP client demonstrating all client operations
3. **server_with_sampling** - Server with reverse RPC (sampling)
4. **echo_websocket** - WebSocket transport example
5. **http_loopback** - HTTP transport loopback demo
6. **llama_server** - llama.cpp MCP adapter
7. **debugger_server** - LLDB debugger MCP server

Feature Examples
----------------

The ``examples/features/`` directory contains 12 specialized examples, each
demonstrating a specific MCP capability using the in-process loopback pattern
for fast, deterministic testing.

1. **progress_cancellation** - Real-time progress updates and request cancellation
2. **notifications_subscriptions** - Server-to-client notifications and subscription handling
3. **middleware** - Intercepting and processing requests via a middleware chain
4. **completions** - Argument completion suggestions for tools
5. **pagination** - Handling large lists via cursor-based pagination
6. **elicitation** - Server-initiated information requests (input forms)
7. **roots** - Declaring and discovering client-side root URIs
8. **error_handling** - Robust error patterns and exception management
9. **transport_memory** - Using in-memory transports for testing and modularity
10. **graceful_shutdown** - Clean exit patterns and signal handling
11. **http_server_convenience** - Simplified HTTP server setup via ``Server::run_http()``
12. **oauth_flow** - Full OAuth 2.0 authentication flow (PKCE, token refresh)

These examples are non-interactive and designed to be run as part of a
test suite or to understand specific API patterns.

Benchmarks
----------

Performance-focused examples demonstrating the efficiency of different transport
layers under load. These are quick teaching benchmarks that run locally and in
CI; for sustained multi-service load testing, use the separate ``benchmark/``
suite.

1. **benchmark_stdio** - Measures roundtrip latency and throughput over the in-memory stdio-equivalent transport
2. **benchmark_http** - Performance analysis of the HTTP transport layer
3. **benchmark_websocket** - Benchmarking WebSocket communication throughput

Each benchmark performs a fixed number of iterations and reports total time,
average latency, and calls per second. The current examples run 1000 iterations
for the memory and HTTP transports, and 50 iterations for the WebSocket
transport so the example remains fast and deterministic in automation.

Each example is fully functional and can be used as a starting point for your
own MCP applications.

server_stdio
------------

**Location**: ``examples/servers/stdio/server_stdio.cpp``

A complete MCP server implementation demonstrating:

* **All 4 handler signatures**: Async with context, async without context, sync with context, sync without context
* **Custom types**: JsonSerializable structs with to_json/from_json
* **Tools**: Mathematical operations (add, multiply) with typed parameters
* **Resources**: Static and dynamic resource handlers
* **Resource Templates**: URI templates with variable substitution
* **Prompts**: Example prompt with arguments
* **Context logging**: Using ``Context::log_info()`` and other log levels

This is the most comprehensive example, showing virtually every SDK feature.

**Key patterns demonstrated**:

* Type-safe tool handlers with custom input/output types
* Resource listing and reading
* Resource template URI expansion
* Prompt registration and retrieval
* Structured logging via Context

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-server-stdio

client_stdio
------------

**Location**: ``examples/clients/stdio/client_stdio.cpp``

A complete MCP client implementation demonstrating:

* **Connection**: Initialize handshake with server capabilities
* **Tools**: List available tools, call tools with typed arguments
* **Resources**: List resources, read resource contents
* **Resource Templates**: List templates, expand URIs
* **Prompts**: List prompts, get prompt with arguments
* **Completion**: Request completion suggestions
* **Notifications**: Send notifications to server
* **Graceful shutdown**: Proper connection close

**Key patterns demonstrated**:

* Client initialization and capability negotiation
* Calling server methods with typed parameters
* Handling server responses
* Error handling with try/catch
* Clean resource cleanup

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-client-stdio

server_with_sampling
--------------------

**Location**: ``examples/advanced/server_with_sampling.cpp``

Demonstrates **reverse RPC** (server-to-client calls) via ``Context::sample_llm()``:

* **Server capabilities**: Declares tool support with change notifications
* **Sampling tool**: Tool handler that calls back to the client via ``ctx.sample_llm()``
* **Async context handlers**: Using ``Context&`` parameter for reverse RPC

This example shows how a server can request LLM sampling from a connected client,
enabling bidirectional communication patterns.

**Key patterns demonstrated**:

* Declaring sampling capability in ServerCapabilities
* Using ``Context::sample_llm()`` for reverse RPC
* Awaiting client responses within a tool handler
* Structured error handling for sampling failures

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-server-sampling

echo_websocket
--------------

**Location**: ``examples/advanced/echo_websocket.cpp``

In-process loopback demonstration using **WebSocket transport** over TCP:

* **WebSocket transport**: Using ``WebSocketTransport`` instead of stdio
* **Server + Client**: Both running in the same process
* **TCP acceptor**: Server listens on localhost port
* **Echo tool**: Simple echo tool that returns its input

This example shows how to use the WebSocket transport layer for network-based
MCP communication.

**Key patterns demonstrated**:

* Creating WebSocket transports from Boost.Beast streams
* Running server and client concurrently in one process
* TCP acceptor setup for WebSocket connections
* Same API for both stdio and WebSocket transports

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-echo-websocket

http_loopback
-------------

**Location**: ``examples/advanced/http_loopback.cpp``

In-process loopback demonstration using **Streamable HTTP transport**:

* **HttpServerTransport + HttpClientTransport**: Full HTTP stack for MCP communication
* **Anti-hang co_spawn pattern**: Safe concurrent server/client execution
* **MCP-Protocol-Version header**: Support for versioned HTTP headers (e.g., ``2025-11-25``)

This example shows how to use the HTTP transport layer for network-based
MCP communication with standard headers.

**Key patterns demonstrated**:

* Using ``HttpServerTransport`` and ``HttpClientTransport``
* Handling HTTP-specific MCP headers
* Coordinating server and client lifecycle in a single process

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-http-loopback

llama_server
------------

**Location**: ``examples/advanced/llama/llama_server.cpp``

MCP adapter for llama.cpp's ``llama-server`` (chat, completion, embedding):

* **External HTTP API integration**: Communicating with llama.cpp via Boost.Beast
* **Dual transport support**: Runtime selection between stdio and HTTP transports
* **CLI argument parsing**: Comprehensive configuration via command line flags

This example demonstrates how to wrap an existing external service into
a fully compliant MCP server.

**Key patterns demonstrated**:

* Asynchronous HTTP client requests within tool handlers
* Exposing external service capabilities (LLM) as MCP tools and resources
* Flexible transport configuration and CLI parameter management

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-llama-mcp
   # or with HTTP transport:
   ./build/release/example-llama-mcp --transport=http

debugger_server
---------------

**Location**: ``examples/advanced/debugger/debugger_server.cpp``

MCP server wrapping LLDB's SB API for programmatic debugging:

* **Optional external dependency**: Integration with ``liblldb``
* **LLDB SB API integration**: Tools for launching, attaching, stepping, and evaluation
* **Resource templates**: Dynamic discovery of threads and stack frames
* **RAII lifecycle management**: Safe initialization and termination of LLDB

This example shows how to build a complex MCP server that provides deep
integration with system-level debugging tools.

**Key patterns demonstrated**:

* Managing external library state within an MCP server
* Using resource templates for dynamic metadata discovery
* Implementing complex interactive workflows (debugging) via tools

**Build and run**:

.. code-block:: bash

   cmake -DBUILD_LLDB_EXAMPLE=ON -B build && cmake --build build
   ./build/release/example-debugger-server

Note: Requires ``-DBUILD_LLDB_EXAMPLE=ON`` and LLDB development libraries installed.

Building All Examples
---------------------

To build all examples at once:

.. code-block:: bash

   # Using build script (recommended)
   python scripts/build.py --examples

   # Or using CMake directly
   cmake -DBUILD_EXAMPLES=ON -B build && cmake --build build

   # Note: The debugger example is OFF by default:
   # cmake -DBUILD_LLDB_EXAMPLE=ON -B build && cmake --build build

All example binaries will be located in ``build/``.

Using Examples as Templates
---------------------------

Each example is designed to be self-contained and can be copied as a starting
point for your own applications:

1. Copy the example file to your project
2. Modify the server/client capabilities as needed
3. Add your custom tools, resources, or prompts
4. Update the CMakeLists.txt to include your new source file

For more details on the API used in these examples, see the :doc:`api/index`.
