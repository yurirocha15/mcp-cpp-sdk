Deployment Guide
================

Deploying an MCP server into a production environment requires careful consideration of transport selection, lifecycle management, and operational security. This guide covers the essential patterns for deploying MCP servers built with the mcp-cpp-sdk.

Overview
--------

When moving from development to production, you must decide how your server will communicate with clients. The Model Context Protocol supports several transport mechanisms:

- **Stdio**: Best for local integration with desktop applications (like Claude Desktop or IDEs).
- **HTTP/SSE**: Recommended for remote deployments, multi-client support, or web-based integrations.

A server built with the SDK serves protocol revision 2026-07-28 and the older revisions on the same transport by default. See `Serving Both Protocol Eras`_ for what that means over HTTP.

Key considerations for deployment include:

- Choosing the right transport for your use case.
- Implementing graceful shutdown to protect in-flight requests.
- Integrating with system process managers (like systemd or Docker).
- Monitoring and health checks.

HTTP Server Setup
-----------------

The mcp-cpp-sdk provides two ways to set up an HTTP server: a high-level convenience API and a manual setup for fine-grained control.

Convenience API (run_http)
~~~~~~~~~~~~~~~~~~~~~~~~~~

For most deployments, the :cpp:func:`mcp::Server::run_http` method is the simplest approach. It manages the ``io_context``, sets up the :cpp:class:`mcp::HttpServerTransport`, and handles signal mapping automatically.

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :start-after: // Spawn server in background thread
   :end-before: // Give server time to start
   :dedent: 8

Manual HTTP Setup
~~~~~~~~~~~~~~~~~

If you need to integrate the MCP server into an existing Boost.Asio application or require custom middleware, you can set up the transport manually.

.. code-block:: cpp

   boost::asio::io_context io_ctx;
   auto transport = std::make_shared<mcp::HttpServerTransport>(
       io_ctx.get_executor(), "0.0.0.0", 8080);

   // Spawn the transport listener
   boost::asio::co_spawn(io_ctx, transport->listen(), boost::asio::detached);

   // Run the MCP server over the transport
   boost::asio::co_spawn(io_ctx, server.run(transport, io_ctx.get_executor()),
                         boost::asio::detached);

   io_ctx.run();

Serving Both Protocol Eras
--------------------------

A 2026-07-28 request is a complete message: it carries its protocol version and
client capabilities in ``_meta`` and belongs to no session. A request of an
older revision belongs to a session that ``initialize`` created. The HTTP
transports serve both on one endpoint, and tell them apart by what the request
carries.

``StreamableHttpSessionManager``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The manager builds a ``Server`` with its factory for each session of an older
revision, as before. All 2026-07-28 requests are answered by one shared
``Server``: the one passed to ``set_modern_server()``, or else one built from
the factory the first time it is needed. ``modern_server()`` returns it, and
builds it if no request has yet. Because every 2026-07-28 client sees the same
tools, resources and prompts, they must not depend on the connection.

Configure that server before ``listen()``, for example its cache hints:

.. code-block:: cpp

   mcp::StreamableHttpSessionManager manager(io_ctx.get_executor(), "127.0.0.1", 8080,
                                             factory);

   manager.modern_server()->set_cache_hints(
       mcp::CacheTarget::eToolsList,
       mcp::CacheHint{.ttlMs = 60'000, .scope = mcp::CacheScope::ePublic});

   boost::asio::co_spawn(io_ctx, manager.listen(), boost::asio::detached);

A 2026-07-28 request ignores ``Mcp-Session-Id`` and ``Last-Event-ID``, and its
response never carries ``Mcp-Session-Id``. The answer is plain JSON. If a
handler sends a notification while it runs (a log message at the request's own
log level, or progress) and the client accepts ``text/event-stream``, the answer
is a chunked event stream that carries the notifications and then the response.
It has no event ids and carries periodic keep-alive comments;
``set_stream_options()`` sets the intervals and ``stream_stats()`` counts what
the streams did. A client that disconnects while its request runs cancels the
request.

``set_protocol_policy()`` takes a ``ServerProtocolPolicy`` and applies it to
every ``Server`` the manager builds. With ``serve_legacy = false`` no session is
ever created: ``initialize`` is answered ``404`` with ``g_METHOD_NOT_FOUND``,
naming the versions served, and every ``GET`` and ``DELETE`` is answered
``405``.

``HttpServerTransport``
~~~~~~~~~~~~~~~~~~~~~~~

``HttpServerTransport`` serves 2026-07-28 requests next to its one legacy
session, with the same header checks and error statuses. Each request is
answered with plain JSON. Notifications a handler sends while it serves one are
dropped, because there is no stream; use ``StreamableHttpSessionManager`` when
handlers log or report progress, or when clients use ``subscriptions/listen``.
At most 256 such requests may wait at once, and more are answered ``503``.
``Server::run_http()`` uses ``HttpServerTransport``.

Request headers
~~~~~~~~~~~~~~~

A 2026-07-28 request over HTTP repeats some of its content in headers, so that
a gateway can route or limit it without reading the body. The server requires
them and checks them against the body:

* ``MCP-Protocol-Version`` and ``Mcp-Method`` on every request.
* ``Mcp-Name`` on ``tools/call``, ``prompts/get`` and ``resources/read``. A value
  in the ``=?base64?...?=`` form is decoded first.
* ``Mcp-Param-{Name}`` on a ``tools/call`` whose tool marks input properties with
  ``x-mcp-header``. See `x-mcp-header parameters`_.

A request that lacks one, repeats one, or sends a value that does not match the
body is answered ``400`` with ``g_HEADER_MISMATCH``.
``header_mismatch_rejections()`` counts those on the manager and on
``HttpServerTransport``. ``HttpClientTransport`` sends the headers itself.

The status of an error follows the specification: ``400`` for invalid params,
an unsupported protocol version, a header mismatch and a missing client
capability, and ``404`` for an unknown method. See :doc:`error-handling`.

x-mcp-header parameters
~~~~~~~~~~~~~~~~~~~~~~~

A tool can ask for some of its arguments to be mirrored into headers by
annotating the property in its input schema:

.. code-block:: cpp

   nlohmann::json schema = {
       {"type", "object"},
       {"properties",
        {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}},
         {"query", {{"type", "string"}}}}},
       {"required", {"region", "query"}}};

A 2026-07-28 ``tools/call`` of this tool over HTTP must then carry a
``Mcp-Param-Region`` header equal to the ``region`` argument, after decoding.
Integers are compared as numbers. ``add_tool()`` refuses with ``SchemaError`` an
annotation that is not a non-empty HTTP token string, repeats a name (ignoring
case), sits on a property that is not a ``string``, ``integer`` or ``boolean``,
or sits on a property not reached from the root through ``properties`` alone.
``HttpClientTransport`` fills the headers in. It leaves a tool with an invalid
annotation out of ``tools/list`` and reports it through
``set_warning_handler()``.

Graceful Shutdown
-----------------

In production, servers are frequently restarted or moved. Graceful shutdown ensures that the server finishes processing active requests before exiting, preventing data loss or client-side errors.

Signal Handling
~~~~~~~~~~~~~~~

You should handle signals like ``SIGINT`` and ``SIGTERM`` to trigger the shutdown process. The SDK provides ``mcp::graceful_shutdown()`` in ``mcp/core/signal.hpp`` to close the transport and stop the ``io_context`` after in-flight work finishes or times out.

.. literalinclude:: ../../examples/features/graceful_shutdown.cpp
   :language: cpp
   :start-after: // ========== GRACEFUL SHUTDOWN (auto-triggered for CI/testing) ==========
   :end-before: // ========== SERVER COROUTINE ==========
   :dedent: 8

The :cpp:func:`mcp::graceful_shutdown` function closes the transport (preventing new connections) and starts a watchdog timer. If requests don't finish within the timeout (defined by ``mcp::constants::g_shutdown_timeout_ms``), it stops the ``io_context`` forcefully.

Production Considerations
-------------------------

Port Binding and Security
~~~~~~~~~~~~~~~~~~~~~~~~~

- **Binding**: Use ``127.0.0.1`` if the server only needs to be accessed by a local proxy (like Nginx) or ``0.0.0.0`` to listen on all interfaces.
- **TLS/SSL**: The built-in HTTP transport does not handle TLS. In production, always place your MCP server behind a reverse proxy like Nginx, Apache, or a cloud load balancer that handles SSL termination.
- **Authentication**: For sensitive tools, implement authentication middleware or use the OAuth flow patterns demonstrated in the ``oauth_flow.cpp`` example.

Unix Daemon / Service Setup
---------------------------

Running your server as a background process (daemon) ensures it stays alive and restarts on failure.

systemd Integration
~~~~~~~~~~~~~~~~~~~

On Linux, ``systemd`` is the standard way to manage services. Using a system service ensures that your MCP server starts automatically on boot and recovers from crashes.

Create a service file at ``/etc/systemd/system/mcp-server.service``:

.. code-block:: ini

   [Unit]
   Description=MCP C++ Server
   After=network.target

   [Service]
   Type=simple
   User=mcpuser
   Group=mcpuser
   WorkingDirectory=/opt/mcp-server
   ExecStart=/opt/mcp-server/bin/my_mcp_server
   Restart=on-failure
   RestartSec=5s
   Environment=PORT=8080
   # Recommended: limit privileges
   NoNewPrivileges=yes
   PrivateTmp=yes
   ProtectSystem=full

   [Install]
   WantedBy=multi-user.target

After creating the file, reload the systemd manager and enable the service:

.. code-block:: bash

   sudo systemctl daemon-reload
   sudo systemctl enable mcp-server
   sudo systemctl start mcp-server

Docker Deployment
-----------------

Containerization provides a consistent environment for your MCP server, making it easier to deploy across different cloud providers. Use a multi-stage Dockerfile to keep the production image small and secure.

.. code-block:: dockerfile

   # Build stage
   FROM gcc:12 AS builder
   RUN apt-get update && apt-get install -y cmake libboost-all-dev
   COPY . /src
   WORKDIR /src
   RUN mkdir build && cd build && \
       cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF && \
       make -j$(nproc)

   # Production stage
   FROM debian:bookworm-slim
   RUN apt-get update && apt-get install -y libboost-dev && \
       rm -rf /var/lib/apt/lists/*

   # Create a non-root user
   RUN useradd -m mcpuser
   USER mcpuser

   COPY --from=builder /src/build/bin/my_mcp_server /usr/local/bin/
   EXPOSE 8080
   ENTRYPOINT ["/usr/local/bin/my_mcp_server"]

When running in Docker, ensure you map the ports correctly and provide any necessary configuration via environment variables.

Load Balancing and Scaling
--------------------------

As your MCP application grows, you might need to scale horizontally by running multiple instances.

Sticky Sessions
~~~~~~~~~~~~~~~

A session of 2025-11-25 or earlier is stateful (initialization, followed by multiple requests), so ensure your load balancer uses **sticky sessions** (session affinity) for such clients. If a client's request is routed to a different instance that hasn't seen the ``initialize`` request, the server will reject the request.

A 2026-07-28 request needs no session, and any instance that serves the same tools, resources and prompts can answer it. Two cases still need the request to reach a particular instance:

- A request whose handler asks the client a question (``Context::elicit()``, ``sample_llm()``, ``request_roots()``) waits in the ``Server`` instance. The client's retry, which carries the ``requestState``, must reach that instance; a retry that does not is answered as for an unknown ``requestState``.
- A ``subscriptions/listen`` stream is one long-lived request, and the notifications it carries come from the instance that holds it.

Health Checks
~~~~~~~~~~~~~

For HTTP deployments, you can implement a health check endpoint. While the MCP protocol doesn't define a standard health check, you can add a simple tool or a parallel HTTP route to verify the server's status.

- **Liveness**: Verify the process is running and the ``io_context`` event loop is responsive.
- **Readiness**: Verify that any external resources (databases, APIs, or model endpoints) that the tools depend on are currently available.

Monitoring and Logging
----------------------

Production servers should emit structured logs for troubleshooting.

- **Standard Streams**: In Stdio mode, avoid writing anything to ``stdout`` except MCP messages. Use ``stderr`` for all logging.
- **Structured Logging**: When using HTTP, consider using a logging library that outputs JSON for better integration with log aggregators like ELK or Datadog.

Example Walkthrough
-------------------

The ``http_server_convenience.cpp`` example provides a complete, runnable demonstration of an HTTP-based deployment using the high-level API.

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :lines: 81-99
   :dedent: 8

For advanced users, ``graceful_shutdown.cpp`` demonstrates how to build a robust server that respects system signals and manages the connection lifecycle manually. This is particularly useful when you need to coordinate the shutdown of multiple services or databases alongside the MCP server.

.. literalinclude:: ../../examples/features/graceful_shutdown.cpp
   :language: cpp
   :lines: 86-118
   :dedent: 8

Cross-References
----------------

- See :doc:`/concepts/transports` for more details on HTTP and Stdio transport specifics.
- See :doc:`error-handling` for patterns on managing production errors and logging exceptions.
