Architecture
============

This document provides an overview of the mcp-cpp-sdk architecture, including
high-level design principles, component structure, and key design decisions.

Design Principles
-----------------

The mcp-cpp-sdk is built on several core design principles:

Compiled Static Library
^^^^^^^^^^^^^^^^^^^^^^^^

The SDK is a compiled static library (``libmcp-cpp-sdk.a``). Implementation
details — including all Boost.Asio and Boost.Beast usage — are hidden behind
PImpl boundaries in ``.cpp`` files. Only ``core.hpp`` retains a direct Boost
include, because the ``Task<T>`` template alias must be visible at call sites.

This design:

* Reduces consumer compile times — Boost headers are not parsed for every
  translation unit that includes an SDK header
* Isolates ABI-unstable Boost internals behind a stable C++ interface
* Keeps public headers free of Boost types except ``Task<T>``

RAII and Value Semantics
^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK follows modern C++ best practices:

* **RAII**: Resources (sockets, timers) are owned by objects and cleaned up automatically
* **Move semantics**: Expensive objects like ``Server`` and ``Client`` are move-only
* **Smart pointers**: ``std::unique_ptr`` for ownership, ``std::shared_ptr`` where needed
* **No raw pointers**: All memory is managed automatically

Coroutine-Based Async
^^^^^^^^^^^^^^^^^^^^^^

All asynchronous operations use C++20 coroutines via Boost.Asio:

* ``Task<T>`` return type for async functions (compatible with ``boost::asio::awaitable<T>``)
* ``co_await`` for sequential async operations
* ``co_spawn`` for launching concurrent tasks
* No callback hell - linear, readable async code

Type Safety
^^^^^^^^^^^

The SDK emphasizes compile-time safety:

* **Concepts**: ``JsonSerializable`` concept ensures types are JSON-compatible
* **Strong typing**: Protocol types are structs, not raw JSON
* **Template metaprogramming**: Handler signatures validated at compile time
* **No ``void*`` or type erasure leaks**: Type erasure is internal only

Component Overview
------------------

The SDK is organized into several key components:

Server
^^^^^^

``mcp::Server`` is the core server implementation:

* Manages tool, resource, prompt, and resource template registrations
* Handles JSON-RPC message dispatch
* Executes handler functions with proper context
* Supports multiple handler signatures (sync/async, with/without context)
* Thread-safe via Boost.Asio strand

**Key responsibilities**:

* Protocol compliance (MCP handshake, request handling)
* Handler type erasure and invocation
* ``ensure_async_handler`` — automatically normalizes handler signatures (sync/async, with/without ``Context``) into unified async form; called internally by ``add_tool``, ``add_resource``, etc.
* Error handling: exceptions thrown during request dispatch are automatically caught and returned as ``INTERNAL_ERROR`` (-32603) JSON-RPC error responses
* Context creation and lifecycle management

Client
^^^^^^

``mcp::Client`` is the core client implementation:

* Connects to MCP servers and performs initialization handshake
* Sends requests and matches responses via request ID
* Manages pending requests with timeout support
* Handles server notifications
* Thread-safe via Boost.Asio strand

**Key responsibilities**:

* Request/response correlation (JSON-RPC id matching) via ``RequestId`` — a type-safe wrapper for JSON-RPC request identifiers (string or integer)
* Reverse RPC: ``dispatch_incoming_request`` handles server-to-client JSON-RPC requests, enabling servers to request client capabilities (elicitation, sampling, roots)
* Timeout management for requests
* Connection lifecycle (connect, run, close)
* Error propagation from server responses

Transport Abstraction
^^^^^^^^^^^^^^^^^^^^^

``mcp::ITransport`` is the transport interface:

.. code-block:: cpp

   class ITransport {
   public:
       virtual Task<std::string> read_message() = 0;
       virtual Task<void> write_message(std::string_view message) = 0;
       virtual void close() = 0;
   };

Built-in implementations:

* **StdioTransport**: Reads from stdin, writes to stdout (common for AI tools)
* **WebSocketTransport**: Communicates over WebSocket (Boost.Beast)
* **HttpClientTransport**: Sends MCP messages to a Streamable HTTP endpoint
* **HttpServerTransport**: Exposes a single-session Streamable HTTP endpoint

For multi-session HTTP deployments, ``StreamableHttpSessionManager`` owns the
listener and creates per-session ``Server`` instances backed by in-memory
transports. The canonical session header name used in the docs is
``MCP-Session-Id``.

Custom transports can be added by implementing the ``ITransport`` interface.

**Transport responsibilities**:

* Message framing (newline-delimited JSON for stdio)
* Underlying I/O (pipes, sockets, etc.)
* Connection state management
* Async read/write operations

Context
^^^^^^^

``mcp::Context`` provides execution context for handlers:

* **Logging**: ``log_debug()``, ``log_info()``, ``log_warning()``, ``log_error()``
* **Reverse RPC**: ``sample_llm()`` for server-to-client sampling requests
* **Notifications**: Send notifications back to the connected peer
* **Request utilities**: Progress reporting, roots requests, elicitation, and cancellation state

Context is passed to handlers that declare a ``Context&`` parameter.

**Context responsibilities**:

* Structured logging with severity levels
* Bidirectional communication (reverse RPC)
* Request metadata (future extension point)

Core Types
^^^^^^^^^^

``mcp::core`` provides fundamental types:

* ``Task<T>`` - Coroutine return type (alias for ``boost::asio::awaitable<T>``)
* ``LogLevel`` - Enum for log severity
* ``McpError`` - Exception type for MCP-specific errors

Protocol Types
^^^^^^^^^^^^^^

``mcp::protocol`` defines all MCP protocol messages:

* Request types: ``InitializeRequest``, ``ListToolsRequest``, ``CallToolRequest``, etc.
* Response types: ``InitializeResult``, ``ListToolsResult``, ``CallToolResult``, etc.
* Notification types: ``InitializedNotification``, ``ProgressNotification``, etc.
* Capability structs: ``ServerCapabilities``, ``ClientCapabilities``
* ``RequestId`` — a named wrapper for JSON-RPC request identifiers (string or integer), replacing raw ``std::variant`` usage

All types support JSON serialization via nlohmann_json. Protocol types are
organized into focused sub-headers (``capabilities.hpp``, ``content.hpp``,
``tools.hpp``, ``roots.hpp``, ``sampling.hpp``, ``elicitation.hpp``, etc.)
included via the ``mcp/protocol.hpp`` umbrella.

Data Flow
---------

The typical data flow for a tool call:

1. **Client** serializes ``CallToolRequest`` to JSON
2. **Client Transport** writes JSON message
3. **Server Transport** reads JSON message
4. **Server** deserializes to ``CallToolRequest``
5. **Server** looks up tool handler by name
6. **Server** creates ``Context`` for the request
7. **Server** invokes type-erased handler (async coroutine)
8. **Handler** processes request, possibly using ``Context::log_info()``
9. **Handler** returns result (``co_return``)
10. **Server** serializes result to ``CallToolResult``
11. **Server Transport** writes JSON response
12. **Client Transport** reads JSON response
13. **Client** matches response to pending request by ID
14. **Client** deserializes and returns result to caller

This flow demonstrates the clean separation between protocol layer, transport
layer, and application logic.

Design Decisions
----------------

Why Boost.Asio?
^^^^^^^^^^^^^^^

Boost.Asio is the de facto standard for async I/O in C++:

* Mature, stable, and widely used
* Excellent coroutine support (awaitable<T>)
* Cross-platform (Windows, Linux, macOS)
* Provides all necessary primitives (timers, streams, executors)
* Well-documented and performant

Alternative considered: ``std::execution`` - not yet standardized or widely available.

Authentication and Authorization
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK includes OAuth 2.1 helpers in ``mcp::auth``:

* ``Authenticator``: abstract interface for token providers, enabling custom authentication beyond OAuth
* ``OAuthAuthenticator``: concrete OAuth 2.0 implementation of the ``Authenticator`` interface
* ``OAuthHttpClient`` for token and metadata HTTP calls
* ``OAuthDiscoveryClient`` for protected resource and authorization-server discovery
* ``OAuthClientTransport`` for injecting Bearer tokens into outgoing MCP requests, with automatic token refresh on ``-32001`` or ``-32000`` error responses
* ``InMemoryTokenStore`` as a simple token persistence implementation

This keeps authentication concerns out of the core client/server types while
still allowing authenticated transports and middleware-based validation.

Why nlohmann_json?
^^^^^^^^^^^^^^^^^^

nlohmann_json is the most popular C++ JSON library:

* Intuitive API (``j["key"] = value``)
* Excellent error messages
* Automatic type conversion
* Wide adoption in the C++ community
* Active maintenance and good performance

Alternative considered: RapidJSON - faster but more complex API.

Why Compiled Static Library?
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Compiling the SDK as a static library reduces consumer build times:

* **Faster builds**: Boost headers are compiled once into the library, not
  re-parsed in every consumer translation unit
* **Encapsulation**: Boost.Asio and Boost.Beast internals stay behind PImpl
  boundaries and do not leak into consumer headers
* **Stable interface**: Public headers expose only standard C++ and
  ``Task<T>`` (the one unavoidable Boost type)
* **Unchanged link model**: Consumers still link a single ``mcp-cpp-sdk``
  target — no change to CMake integration

Tradeoff: Cross-boundary inlining is no longer possible for PImpl'd types,
which is acceptable given that the hot paths are I/O-bound.

Why Multiple Handler Signatures?
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Supporting 4 handler signatures (sync/async × with/without context) provides flexibility:

* **Async with context**: Most powerful, enables logging and reverse RPC
* **Async without context**: Simpler for pure computation
* **Sync with context**: For quick operations that need logging
* **Sync without context**: Simplest possible handler

Type erasure unifies these at runtime, so ``Server`` internals stay simple.

Thread Safety Model
-------------------

All async operations execute on a Boost.Asio **strand**:

* Strand serializes handler execution
* No explicit locks (``std::mutex``) needed
* Simple mental model: handlers never run concurrently
* Safe to modify ``Server``/``Client`` state from handlers

External synchronization is required only if accessing SDK objects from outside
the strand (e.g., from a different thread).

Performance Characteristics
---------------------------

* **Zero-copy**: Messages are moved, not copied (``std::string`` via move semantics)
* **Lazy serialization**: JSON parsed only when fields are accessed
* **Minimal allocations**: Handler type erasure uses ``std::function`` (one allocation)
* **No virtual dispatch in hot path**: Templates resolve at compile time

For high-throughput scenarios, consider:

* Reusing ``io_context`` across multiple connections
* Tuning ``nlohmann_json`` allocator (custom allocator support)
* Batching small messages if protocol allows

Extensibility
-------------

The SDK is designed for extension:

* **Custom transports**: Implement ``ITransport`` (e.g., for HTTP/2)
* **Custom serialization**: Provide ``to_json``/``from_json`` for your types
* **Custom executors**: Pass any ``mcp::Runtime``-compatible executor
* **Middleware**: Intercept handler execution for auth, logging, and request shaping

Future Work
-----------

Potential future enhancements:

* **Connection pooling**: Reuse transports across multiple requests
* **HTTP/2 transport**: For high-performance network scenarios
* **std::execution support**: When standardized and available
* **Batched operations**: Protocol extension for bulk requests

See the GitHub issues for planned features and contributions.
