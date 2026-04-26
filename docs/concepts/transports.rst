Transports
==========

Transports define how the MCP server and client communicate. The `mcp-cpp-sdk`
provides multiple transport layers out of the box, with a consistent API. Every
transport implements the `ITransport` interface, ensuring that your server or
client logic remains independent of the underlying communication protocol.

This guide explores the various transport options available in the SDK, from
standard local process communication to remote network-based protocols.

Standard Transports
-------------------

The SDK includes several built-in transport types to cover different use cases:

1. **StdioTransport**: For local process communication via standard input/output.
   This is the most common choice for connecting to local agents like Claude Desktop.
2. **HttpServerTransport / HttpClientTransport**: For MCP over HTTP with custom
   streamable headers, following the official MCP specification.
3. **WebSocketTransport**: For persistent, bidirectional network communication.
   Ideal for high-performance remote integrations.
4. **MemoryTransport**: An in-memory transport for unit testing and in-process
   communication.

Stdio Transport (Local)
-----------------------

The Stdio transport is the default and recommended way to integrate with local tools.
It leverages the standard input and output streams of the process, making it
extremely easy to deploy as a subprocess.

### Server Side

The simplest way to start a stdio-based server is using the high-level `run_stdio()`
method, which blocks the main thread until the connection is closed.

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   int main() {
       // Define server information and capabilities
       mcp::Implementation info{"my-server", "1.0.0"};
       mcp::ServerCapabilities caps;
       caps.tools = mcp::ServerCapabilities::ToolsCapability{};

       mcp::Server server(info, caps);

       // Add your tools, resources, and prompts here
       server.add_tool("echo", "Echoes input", schema, [](auto args) { ... });

       // Run the server on stdin/stdout
       server.run_stdio();
   }

### Client Side

Clients can connect to a stdio-based server by providing the executor to the
`StdioTransport`.

.. code-block:: cpp

   #include <mcp/client.hpp>
   #include <mcp/transport/stdio.hpp>

   boost::asio::co_spawn(executor, [&]() -> mcp::Task<void> {
       auto transport = std::make_shared<mcp::StdioTransport>(executor);
       mcp::Client client(transport, executor);

       co_await client.connect(client_info, {});
       // Interact with the server...
   }, boost::asio::detached);

HTTP Transport (Network)
------------------------

The HTTP transport follows the MCP over HTTP specification, which uses a long-running
POST request for server-sent events or streamable data.

### HTTP Server Convenience

For developers who want a quick way to host an MCP server over HTTP without
worrying about the underlying networking boilerplate, the SDK provides `run_http()`.

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :start-after: // This convenience method handles everything:
   :end-before: server.run_http("127.0.0.1", http_port);
   :dedent: 16

The following example shows how to use the convenience method:

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :lines: 83-99
   :dedent: 8

### Manual HTTP Configuration

If you need to integrate the MCP transport into an existing HTTP server (like
one built with Boost.Beast), you can manage the `HttpServerTransport` lifecycle
yourself.

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :lines: 39-49
   :dedent: 8

WebSocket Transport
-------------------

For full-duplex, bidirectional communication over the network, `WebSocketTransport`
is the ideal choice. Unlike HTTP, which often requires polling or long-running
streams for bidirectional data, WebSockets provide a native persistent connection.

.. code-block:: cpp

   #include <mcp/transport/websocket.hpp>

   // On the server
   auto transport = std::make_shared<mcp::WebSocketTransport>(io_ctx.get_executor());
   co_await server.run(transport, io_ctx.get_executor());

MemoryTransport for Testing
---------------------------

Testing is a critical part of developing MCP integrations. The `MemoryTransport`
allows you to run both a server and a client in the same process, communicating
entirely in memory. This eliminates the need for network configuration or local
process management during testing.

### Creating Transport Pairs

You should always use the `create_memory_transport_pair` helper to ensure that
both ends of the transport are correctly linked.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :lines: 74-80
   :dedent: 8

### Usage in Integration Tests

Because `MemoryTransport` implements the same `ITransport` interface as all other
transports, your server and client code remain identical to production.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :lines: 82-89
   :dedent: 8

For more information on writing effective tests, see the :doc:`/guides/testing` guide.

TransportFactory and Runtime
----------------------------

The `Runtime` class is a high-level wrapper around the Boost.Asio event loop,
providing a simple `run()` and `stop()` interface. The `TransportFactory`
further simplifies transport creation by binding them to a specific runtime.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :lines: 172-194
   :dedent: 8

Choosing the Right Transport
----------------------------

Selecting the appropriate transport is essential for the success of your MCP
integration. Use the table below as a guide:

+----------------------+--------------------------+------------------------------------------------+
| Transport            | Primary Use Case         | Key Advantages                                 |
+======================+==========================+================================================+
| **Stdio**            | Local IDE/Agent Plugins  | - Zero network configuration                   |
|                      |                          | - Secure by default (local only)               |
|                      |                          | - Easiest to deploy                            |
+----------------------+--------------------------+------------------------------------------------+
| **HTTP**             | Remote APIs, Web Hooks   | - Firewall and proxy friendly                  |
|                      |                          | - Works with standard web infrastructure       |
|                      |                          | - Good for one-way or slow bidirectional data  |
+----------------------+--------------------------+------------------------------------------------+
| **WebSocket**        | High-performance Remote  | - Native bidirectional communication           |
|                      |                          | - Persistent, low-latency connection           |
|                      |                          | - Minimal message overhead                     |
+----------------------+--------------------------+------------------------------------------------+
| **Memory**           | Unit & Integration Tests | - Extremely fast execution                     |
|                      |                          | - Deterministic (no network flakes)            |
|                      |                          | - No side effects on the host system           |
+----------------------+--------------------------+------------------------------------------------+

Custom Transports
-----------------

If none of the built-in transports meet your needs, you can implement a custom
one. This might be useful for IPC mechanisms like Unix Domain Sockets or
proprietary messaging protocols.

All you need to do is inherit from `mcp::ITransport` and implement three methods:

.. code-block:: cpp

   #include <mcp/transport.hpp>

   class MyCustomTransport : public mcp::ITransport {
   public:
       // Read a single JSON-RPC message as a string
       mcp::Task<std::string> read_message() override {
           // Wait for data on your custom pipe/socket
           std::string data = co_await my_socket.async_read(...);
           co_return data;
       }

       // Write a single JSON-RPC message
       mcp::Task<void> write_message(std::string_view message) override {
           co_await my_socket.async_write(message);
       }

       // Clean up resources and signal closure
       void close() override {
           my_socket.close();
       }
   };
