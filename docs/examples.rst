Examples
========

The mcp-cpp-sdk includes several example programs demonstrating different aspects
of the SDK and various usage patterns. All examples are located in the ``examples/``
directory.

Overview
--------

The SDK provides four comprehensive examples:

1. **server_stdio** - Full-featured MCP server over stdio
2. **client_stdio** - MCP client demonstrating all client operations
3. **server_with_sampling** - Server with reverse RPC (sampling)
4. **echo_websocket** - WebSocket transport example

Each example is fully functional and can be used as a starting point for your
own MCP applications.

server_stdio
------------

**Location**: ``examples/server_stdio.cpp``

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

   make build
   ./build/examples/server_stdio

client_stdio
------------

**Location**: ``examples/client_stdio.cpp``

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

   make build
   ./build/examples/client_stdio

server_with_sampling
--------------------

**Location**: ``examples/server_with_sampling.cpp``

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

   make build
   ./build/examples/server_with_sampling

echo_websocket
--------------

**Location**: ``examples/echo_websocket.cpp``

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

   make build
   ./build/examples/echo_websocket

Building All Examples
---------------------

To build all examples at once:

.. code-block:: bash

   # Using make (recommended)
   make build

   # Or using CMake directly
   cmake --build build --target all

All example binaries will be located in ``build/examples/``.

Using Examples as Templates
---------------------------

Each example is designed to be self-contained and can be copied as a starting
point for your own applications:

1. Copy the example file to your project
2. Modify the server/client capabilities as needed
3. Add your custom tools, resources, or prompts
4. Update the CMakeLists.txt to include your new source file

For more details on the API used in these examples, see the :doc:`api/index`.
