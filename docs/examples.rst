Examples
========

The mcp-cpp-sdk includes several example programs demonstrating different aspects
of the SDK and various usage patterns. All examples are located in the ``examples/``
directory.

Overview
--------

The SDK builds nine core examples plus 18 specialized feature examples:

1. **server_stdio** - Full-featured MCP server over stdio
2. **server_simple** - Minimal MCP server over stdio
3. **client_stdio** - MCP client demonstrating representative server operations
4. **interactive_client** - Interactive stdio client
5. **server_with_sampling** - Server with reverse RPC (sampling)
6. **echo_websocket** - WebSocket transport example
7. **http_loopback** - HTTP transport loopback demo
8. **llama_server** - llama.cpp MCP adapter
9. **debugger_server** - LLDB debugger MCP server (uses stubs when LLDB is unavailable)

Sync-Mode and Async-Mode Examples
---------------------------------

``examples/sync/`` and ``examples/async/`` hold the same programs in the two
modes:

* ``notes_server`` and ``notes_client``: a server with a tool, a resource that
  can be subscribed to and a prompt, and a client that uses them, over stdio.
  :doc:`guides/sync-to-async` converts the sync programs to async mode.
* ``server`` and ``client``: a server and a client with every feature, over HTTP
  with a bearer token. The pages of :doc:`concepts/index` show each feature from
  them, and :doc:`guides/async-mode` walks through the async programs.

``examples/sync/`` also holds ``minimal_server`` and ``minimal_client``, the
programs of :doc:`getting-started`. Each program builds as
``example-<mode>-<name>``, with dashes for the underscores. A stdio client takes
the server's command, an HTTP server its port, and an HTTP client the server's
URL:

.. code-block:: bash

   ./build/release/example-async-notes-client ./build/release/example-async-notes-server
   ./build/release/example-sync-server 8080 &
   ./build/release/example-sync-client http://127.0.0.1:8080/mcp

Feature Examples
----------------

The ``examples/features/`` directory contains 18 specialized examples. They use
small, self-contained setups to demonstrate individual capabilities; some use
in-memory loopback and others exercise local network transports.

1. **progress_cancellation** - Real-time progress updates and request cancellation
2. **notifications_subscriptions** - Server-to-client notifications and subscription handling
3. **middleware** - Intercepting and processing requests via a middleware chain
4. **completions** - Completion suggestions for prompt arguments
5. **pagination** - Handling large lists via cursor-based pagination
6. **elicitation** - Server-initiated information requests (input forms)
7. **roots** - Declaring and discovering client-side root URIs
8. **error_handling** - Error patterns and exception management
9. **transport_memory** - Using in-memory transports for testing and modularity
10. **graceful_shutdown** - Clean exit patterns and signal handling
11. **http_server_convenience** - HTTP server setup via ``Server::run_http()``
12. **oauth_flow** - Loopback OAuth building blocks (discovery, PKCE, exchange, refresh)
13. **schema_validation** - Tool schema checks on the server and the client
14. **modern_client** - Protocol revision 2026-07-28: era detection, ``ConnectResult``,
    per-request log levels and cache hints
15. **legacy_protocols** - The older protocol revisions: pinning a version, ``ServerProtocolPolicy``,
    ``ping()`` and ``resources/subscribe``
16. **stdio_launch** - ``StdioClientTransport`` and ``StdioClientSession``: launching a server process,
    capturing its standard error, shutting it down and restarting it
17. **tls_client** - An HTTPS client with the system's or a private certificate authority, through
    the proxy the environment names (see :doc:`guides/tls`)
18. **tls_server** - ``run_http()`` serving https, with mutual TLS when given a client
    certificate authority

These examples are non-interactive and designed to be run as part of a
test suite or to understand specific API patterns.

Benchmarks
----------

These examples exercise different transport paths and report measurements from
the local run. They are teaching benchmarks that run locally and in CI; for
sustained multi-service load testing, use the separate ``benchmark/`` suite.

1. **benchmark_stdio** - Measures roundtrip latency and throughput over the in-memory stdio-equivalent transport
2. **benchmark_http** - Performance analysis of the HTTP transport layer
3. **benchmark_websocket** - Benchmarking WebSocket communication throughput

Each benchmark performs a fixed number of iterations and reports total time,
average latency, and calls per second. The current examples run 1000 iterations
for the memory and HTTP transports, and 50 iterations for the WebSocket
transport to keep the automated run bounded.

Use the example closest to your integration as an API reference, then add the
deployment-specific validation, security, and error handling your application
requires.

server_stdio
------------

**Location**: ``examples/servers/stdio/server_stdio.cpp``

A complete MCP server implementation demonstrating:

* **All 4 handler signatures**: Async with context, async without context, sync with context, sync without context
* **Custom types**: JsonSerializable structs with to_json/from_json
* **Tools**: Typed add/greet handlers and JSON echo/uppercase handlers
* **Resources**: A typed static resource handler
* **Resource Templates**: Metadata registration for a URI template
* **Prompts**: Example prompt with arguments
* **Context logging**: Using ``Context::log_info()`` and other log levels

**Key patterns demonstrated**:

* Type-safe tool handlers with custom input/output types
* Resource listing and reading
* Resource template metadata listing
* Prompt registration and retrieval
* Structured logging via Context

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-server-stdio

client_stdio
------------

**Location**: ``examples/clients/stdio/client_stdio.cpp``

A stdio MCP client demonstrating:

* **Launching the server**: ``StdioClientTransport`` starts ``example-server-stdio``
  as a child process and shuts it down at the end
* **Connection**: ``server/discover``, falling back to ``initialize`` for an older server
* **Tools**: List available tools, call tools with typed arguments
* **Resources**: List resources, read resource contents
* **Resource Templates**: List template metadata
* **Prompts**: List prompts, get prompt with arguments

**Key patterns demonstrated**:

* Client initialization and capability negotiation
* Calling server methods with typed parameters
* Handling server responses
* Handling typed content variants in server responses

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-client-stdio

server_with_sampling
--------------------

**Location**: ``examples/advanced/server_with_sampling.cpp``

Demonstrates reverse RPC (server-to-client calls) via ``Context::sample_llm()``:

* **Server capabilities**: Declares tool support with change notifications, and logging
  because the tool logs to the client
* **Sampling tool**: Tool handler that calls back to the client via ``ctx.sample_llm()``
* **Async context handlers**: Using ``Context&`` parameter for reverse RPC

**Key patterns demonstrated**:

* Relying on the connected client to advertise its sampling capability
* Using ``Context::sample_llm()`` for reverse RPC
* Awaiting client responses within a tool handler
* Reading text content from the sampling response

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-server-sampling

echo_websocket
--------------

**Location**: ``examples/advanced/echo_websocket.cpp``

In-process loopback demonstration using the WebSocket transport over TCP:

* **WebSocket transport**: Using ``WebSocketServerTransport`` and
  ``WebSocketClientTransport`` instead of stdio
* **Server + Client**: Both running in the same process
* **TCP acceptor**: Server listens on localhost port
* **Echo tool**: Simple echo tool that returns its input

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

In-process loopback demonstration using the Streamable HTTP transport:

* **HttpServerTransport + HttpClientTransport**: Full HTTP stack for MCP communication
* **Anti-hang co_spawn pattern**: Safe concurrent server/client execution
* **MCP-Protocol-Version header**: Sent with every request (``2026-07-28`` here)

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
* **CLI argument parsing**: Configuration via command line flags

**Key patterns demonstrated**:

* Tool handlers that ``co_await`` an external HTTP API without blocking the server
* Typed ``mcp::CallToolResult`` results, and thrown exceptions turned into
  ``isError`` results by the SDK
* Exposing external service capabilities (LLM) as MCP tools and resources
* ``Server::run_stdio()`` and ``Server::run_http()`` selected at runtime

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

**Key patterns demonstrated**:

* Running blocking library calls on a dedicated thread with
  ``Server::set_handler_executor()``, so the server keeps answering requests
  while a handler waits
* Moving a resource handler to the same thread with ``co_spawn``
* Using resource templates for dynamic metadata discovery
* Implementing complex interactive workflows (debugging) via tools

**Build and run**:

.. code-block:: bash

   python scripts/build.py --examples
   ./build/release/example-debugger-server

The example is built with the other examples. It links LLDB when CMake finds
the LLDB development files, and is built against stubs otherwise.

Building All Examples
---------------------

To build all examples at once:

.. code-block:: bash

   python scripts/build.py --examples

All example binaries are placed in ``build/release/``.

Using Examples as Templates
---------------------------

Each example is designed to be self-contained and can be copied as a starting
point for your own applications:

1. Copy the example file to your project
2. Modify the server/client capabilities as needed
3. Add your custom tools, resources, or prompts
4. Update the CMakeLists.txt to include your new source file

For more details on the API used in these examples, see the :doc:`api/index`.
