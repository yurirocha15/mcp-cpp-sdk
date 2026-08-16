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
2. **HttpServerTransport / HttpClientTransport**: Single-session Streamable HTTP
   building blocks. ``StreamableHttpSessionManager`` provides a multi-session
   server endpoint.
3. **WebSocketServerTransport / WebSocketClientTransport**: An optional
   persistent transport for integrations that explicitly agree on WebSocket;
   it is not the standard Streamable HTTP transport.
4. **MemoryTransport**: An in-memory transport for unit testing and in-process
   communication.

Stdio Transport (Local)
-----------------------

The Stdio transport is the default and recommended way to integrate with local tools.
It leverages the standard input and output streams of the process, making it
extremely easy to deploy as a subprocess.

Server Side
~~~~~~~~~~~

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

Client Side
~~~~~~~~~~~

Clients can connect to a stdio-based server by providing the executor to the
`StdioTransport`.

.. code-block:: cpp

   #include <mcp/client/client.hpp>
   #include <mcp/transport/stdio.hpp>

   boost::asio::co_spawn(executor, [&]() -> mcp::Task<void> {
       auto transport = std::make_shared<mcp::StdioTransport>(executor);
       mcp::Client client(transport, executor);

       co_await client.connect(client_info, {});
       // Interact with the server...
   }, boost::asio::detached);

HTTP Transport (Network)
------------------------

The HTTP implementation accepts MCP POST requests and returns either JSON or a
finite SSE body. Managed sessions support session IDs, DELETE teardown, and
bounded event replay through GET with ``Last-Event-ID``. Continuously open SSE
polling streams are not yet implemented.

After initialization, ``HttpClientTransport`` carries the server-selected
protocol version on subsequent session requests and best-effort DELETE
teardown, including when the initialize response arrived in an SSE event.

The built-in HTTP transports are plaintext. Bind local development servers to
loopback. For remote deployments, place them behind a TLS-terminating proxy or
provide a custom TLS transport. Requests carrying an ``Origin`` header are
denied by default; configure an exact allowlist (or explicitly opt into all
origins) before starting the listener. Bearer-token validators must likewise be
installed before ``listen()`` or ``run()``.

HTTP Server Convenience
~~~~~~~~~~~~~~~~~~~~~~~

For developers who want a quick way to host an MCP server over HTTP without
worrying about the underlying networking boilerplate, the SDK provides `run_http()`.

``Server::run_http()`` owns its ``HttpServerTransport`` internally and does not
currently expose Origin allowlist or bearer-token validator configuration. Use
it only on a suitable trusted boundary, such as the loopback example below. For
a configurable deployment, construct ``HttpServerTransport`` or
``StreamableHttpSessionManager`` directly, apply the security settings, and
then start the listener.

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

Manual HTTP Configuration
~~~~~~~~~~~~~~~~~~~~~~~~~

If you need to integrate the MCP transport into an existing HTTP server (like
one built with Boost.Beast), you can manage the `HttpServerTransport` lifecycle
yourself.

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :lines: 39-49
   :dedent: 8

WebSocket Transport
-------------------

For integrations that explicitly choose full-duplex WebSocket communication,
``WebSocketServerTransport`` and ``WebSocketClientTransport``
provide a persistent connection. Interoperability with standard MCP clients is
not implied; use Streamable HTTP when protocol-standard remote transport is
required.

.. code-block:: cpp

   #include <mcp/transport/websocket.hpp>

   // On the server, accept a TCP socket first and then wrap it.
   mcp::WebSocketServerTransport server_transport(std::move(socket));

   // On the client, the transport performs the TCP + WebSocket handshake.
   mcp::WebSocketClientTransport client_transport(io_ctx.get_executor(), "127.0.0.1", "9001");

MemoryTransport for Testing
---------------------------

Testing is a critical part of developing MCP integrations. The `MemoryTransport`
allows you to run both a server and a client in the same process, communicating
entirely in memory. This eliminates the need for network configuration or local
process management during testing.

Creating Transport Pairs
~~~~~~~~~~~~~~~~~~~~~~~~

You should always use the `create_memory_transport_pair` helper to ensure that
both ends of the transport are correctly linked.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :lines: 32-39
   :dedent: 8

Usage in Integration Tests
~~~~~~~~~~~~~~~~~~~~~~~~~~

Because `MemoryTransport` implements the same `ITransport` interface as all other
transports, your server and client code remain identical to production.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :lines: 43-66
   :dedent: 8

For more information on writing effective tests, see the :doc:`/guides/testing` guide.

TransportFactory and Runtime
----------------------------

The `Runtime` class is a high-level wrapper around the Boost.Asio event loop,
providing a simple `run()` and `stop()` interface. The `TransportFactory`
further simplifies transport creation by binding them to a specific runtime.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :lines: 88-99
   :dedent: 4

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
| **HTTP**             | MCP remote endpoints     | - Standard Streamable HTTP request model       |
|                      |                          | - Managed sessions and bounded event replay    |
|                      |                          | - Deploy behind TLS for non-loopback use       |
+----------------------+--------------------------+------------------------------------------------+
| **WebSocket**        | Agreed custom integration| - Full-duplex persistent channel               |
|                      |                          | - Separate client and server transport types   |
|                      |                          | - Requires explicit interoperability agreement |
+----------------------+--------------------------+------------------------------------------------+
| **Memory**           | Unit & Integration Tests | - In-process message exchange                  |
|                      |                          | - No network setup                             |
|                      |                          | - Linked endpoint-pair helper                  |
+----------------------+--------------------------+------------------------------------------------+

Custom Transports
-----------------

If none of the built-in transports meet your needs, you can implement a custom
one. This might be useful for IPC mechanisms like Unix Domain Sockets or
proprietary messaging protocols.

All you need to do is inherit from `mcp::ITransport` and implement three methods:

.. code-block:: cpp

   #include <mcp/transport/transport.hpp>

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
