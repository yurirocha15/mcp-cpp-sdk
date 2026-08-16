Architecture
============

This document provides an overview of the mcp-cpp-sdk architecture, including
high-level design principles, component structure, and key design decisions.

Design Principles
-----------------

The mcp-cpp-sdk is built on several core design principles:

Compiled library boundaries
^^^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK builds both shared and static library variants. Much of the client,
server, and network-transport runtime is implemented in ``.cpp`` files, while
serialization helpers and typed handler templates remain in public headers.
The transport and OAuth runtime implementations live in compiled translation
units. The async and transport APIs still expose Boost.Asio types and
Boost.Beast aliases, so consumers parse the corresponding Boost headers.

This design:

* Keeps implementation state behind PImpl where it materially reduces public
  surface area
* Places non-template runtime behavior in compiled translation units where the
  public API does not require an inline definition
* Preserves templates in headers where C++ requires their definitions at the
  point of instantiation

RAII and Value Semantics
^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK follows modern C++ best practices:

* **RAII**: Resources (sockets, timers) are owned by objects and cleaned up automatically
* **Stable ownership**: ``Server`` and ``Client`` are non-copyable and
  non-movable; callers keep them alive while their ``run()`` or ``connect()``
  tasks are active
* **Smart pointers**: implementation, transport, and request state that must
  survive suspension is shared with the relevant coroutines
* **Explicit lifetime contracts**: request-scoped non-owning views are paired
  with an owning session or runtime state

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
* **Typed registration helpers**: Templates validate handler signatures, then
  bridge to the public ``TypeErasedHandler`` middleware interface and compiled
  runtime

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
* Serializes each server session through a Boost.Asio strand

**Key responsibilities**:

* Protocol compliance (MCP handshake, request handling)
* Handler type erasure and invocation
* ``ensure_async_handler`` — automatically normalizes handler signatures (sync/async, with/without ``Context``) into unified async form; called internally by ``add_tool``, ``add_resource``, etc.
* Error handling: tool-handler exceptions become ``CallToolResult`` values with
  ``isError=true``; other request-dispatch failures use JSON-RPC error responses
* Context creation and lifecycle management

Client
^^^^^^

``mcp::Client`` is the core client implementation:

* Connects to MCP servers and performs initialization handshake
* Sends requests and matches responses via request ID
* Manages pending requests with timeout support
* Handles server notifications
* Serializes client request, response, timeout, and close state through a
  Boost.Asio strand-backed runtime

**Key responsibilities**:

* Request/response correlation (JSON-RPC id matching) via ``RequestId`` — a type-safe wrapper for JSON-RPC request identifiers (string or integer)
* Reverse RPC: ``dispatch_incoming_request`` handles server-to-client JSON-RPC requests, enabling servers to request client capabilities (elicitation, sampling, roots)
* Timeout management for requests
* Connection lifecycle (``connect()`` and ``close()``; the read loop is managed
  internally)
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
* **WebSocketServerTransport / WebSocketClientTransport**: Communicate over WebSocket (Boost.Beast)
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
* Cancellation state and progress support when a request supplies a progress
  token

Core Types
^^^^^^^^^^

The core headers expose fundamental types in the ``mcp`` namespace:

* ``Task<T>`` - Coroutine return type (alias for ``boost::asio::awaitable<T>``)
* ``LoggingLevel`` - Protocol enum for log severity
* ``Error`` and JSON-RPC error responses - Structured peer-visible failures

Protocol Types
^^^^^^^^^^^^^^

The protocol headers expose MCP message types in the ``mcp`` namespace:

* Request types: ``InitializeRequest``, ``ListToolsRequest``, ``CallToolRequest``, etc.
* Response types: ``InitializeResult``, ``ListToolsResult``, ``CallToolResult``, etc.
* Notification types: ``InitializedNotification``, ``ProgressNotification``, etc.
* Capability structs: ``ServerCapabilities``, ``ClientCapabilities``
* ``RequestId`` — a named wrapper for JSON-RPC request identifiers (string or integer), replacing raw ``std::variant`` usage

All types support JSON serialization via nlohmann_json. Protocol types are
organized into focused sub-headers (``capabilities.hpp``, ``content.hpp``,
``tools.hpp``, ``roots.hpp``, ``sampling.hpp``, ``elicitation.hpp``, etc.)
included via the ``mcp/protocol/protocol.hpp`` umbrella.

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

The SDK uses Boost.Asio for async I/O because it provides the primitives used by
the public coroutine model:

* C++20 coroutine support through ``awaitable<T>`` and ``co_spawn``
* Cross-platform (Windows, Linux, macOS)
* Timers, streams, executors, and strands under one execution model
* Established documentation and ecosystem

Authentication and Authorization
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK includes OAuth 2.1 helpers in ``mcp::auth``:

* ``Authenticator``: abstract interface for token providers, enabling custom authentication beyond OAuth
* ``OAuthAuthenticator``: concrete OAuth 2.0 implementation of the ``Authenticator`` interface
* ``OAuthHttpClient`` for token and metadata HTTP calls
* ``OAuthDiscoveryClient`` for protected resource and authorization-server discovery
* ``OAuthClientTransport`` for sending HTTP Bearer credentials and attempting
  one refresh after HTTP 401 or the legacy ``g_UNAUTHORIZED`` JSON-RPC path
* ``InMemoryTokenStore`` as a simple token persistence implementation

This keeps authentication concerns out of the core client/server types while
still allowing authenticated transports and middleware-based validation.

Why nlohmann_json?
^^^^^^^^^^^^^^^^^^

nlohmann_json was selected for its direct mapping between JSON and C++ protocol
types:

* Object-style API (``j["key"] = value``)
* Automatic conversion through ``to_json`` and ``from_json``
* Support for the variant and optional fields used by MCP messages

Why compiled library variants?
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The compiled variants provide implementation and linkage boundaries while
retaining the template-based public API:

* **Encapsulation**: Internal request state and transport machinery stay behind
  implementation boundaries
* **Choice of linkage**: Consumers select ``mcp::sdk_shared`` or
  ``mcp::sdk_static`` explicitly, or use ``mcp::sdk`` for the configured
  default
* **Template ergonomics**: Typed handlers remain available without a separate
  code-generation step

Tradeoff: the coroutine and executor types remain part of the source-level API,
and PImpl calls cannot be inlined across the library boundary without link-time
optimization.

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

Client runtime transitions and each server session are serialized through
Boost.Asio strands. Separate HTTP sessions may run concurrently, and a
``StreamableHttpSessionManager`` can place tool work on a separate executor.
Application state shared by handlers therefore still needs its own
synchronization.

Complete server registrations before starting a session. Configure Origin and
bearer-token setters directly on ``HttpServerTransport`` or
``StreamableHttpSessionManager`` before ``listen()`` or ``run()``;
``Server::run_http()`` does not expose those transport settings. The token store
is independently synchronized, but that does not make every OAuth wrapper
operation safe for arbitrary concurrent calls.

Performance Characteristics
---------------------------

Protocol messages are parsed with ``nlohmann_json`` and cross the transport
interface as serialized strings. Typed handler dispatch is normalized once at
registration; transport dispatch remains virtual by design. Allocation and
copy behavior depends on message size, JSON values, and the selected transport,
so the project does not promise zero-copy operation.

For high-throughput scenarios, consider:

* Reusing an ``io_context`` and a multi-session HTTP manager
* Moving blocking application work to a bounded worker executor
* Measuring the complete workload with the reproducible ``benchmark/`` suite

Benchmark results are end-to-end measurements for a pinned workload and build
configuration; they are not a universal SDK throughput guarantee.

Extensibility
-------------

The SDK is designed for extension:

* **Custom transports**: Implement ``ITransport`` (e.g., for HTTP/2)
* **Custom serialization**: Provide ``to_json``/``from_json`` for your types
* **Asio executors**: Pass an executor accepted by the relevant
  ``boost::asio::any_io_executor`` API
* **Middleware**: Intercept handler execution for auth, logging, and request shaping

Future Work
-----------

Potential future enhancements:

* **Connection pooling**: Reuse transports across multiple requests
* **HTTP/2 transport**: For deployments that require HTTP/2
* **Sender/receiver interoperability**: Integration with standard execution
  APIs as supported toolchains make it practical
* **Batched operations**: Protocol extension for bulk requests

See the GitHub issues for planned features and contributions.
