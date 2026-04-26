Transports
==========

Transports define how the MCP server and client communicate. The `mcp-cpp-sdk`
provides multiple transport layers out of the box, with a consistent API.

Standard Transports
-------------------

The SDK includes three built-in transport types:

1. **StdioTransport**: For local process communication via standard input/output.
2. **WebSocketTransport**: For bidirectional network communication using WebSockets.
3. **HttpServerTransport / HttpClientTransport**: For MCP over HTTP with custom streamable headers.

Using Stdio (Local)
-------------------

Stdio is the default and recommended way to integrate with tools like **Claude Desktop**.

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   int main() {
       mcp::Server server(info, caps);
       // ... add tools, resources ...
       server.run_stdio(); // Easy, high-level API
   }

Using WebSocket (Network)
-------------------------

For network-based communication, use the `WebSocketTransport`:

.. code-block:: cpp

   #include <mcp/transport/websocket.hpp>

   auto transport = std::make_unique<mcp::WebSocketTransport>(io_ctx.get_executor());
   co_await server.run(std::move(transport), io_ctx.get_executor());

Using HTTP (Streamable)
-----------------------

For more complex network setups where HTTP is preferred:

.. code-block:: cpp

   #include <mcp/transport/http_server.hpp>

   auto transport = std::make_unique<mcp::HttpServerTransport>(io_ctx.get_executor());
   co_await server.run(std::move(transport), io_ctx.get_executor());

Custom Transports
-----------------

You can implement your own transport layer by inheriting from the `ITransport`
interface defined in `include/mcp/transport.hpp`.
