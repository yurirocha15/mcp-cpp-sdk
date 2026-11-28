Deployment Guide
================

Deploying an MCP server to production involves transport selection, lifecycle management and operational security.

Overview
--------

A production server communicates with clients over one of these transports:

- **Stdio**: Best for local integration with coding agents, IDEs and desktop applications.
- **HTTP/SSE**: Recommended for remote deployments, multi-client support, or web-based integrations.

A server built with the SDK serves protocol revision 2026-07-28 and the older revisions on the same transport by default. See `Serving Both Protocol Eras`_ for what that means over HTTP.

HTTP Server Setup
-----------------

An HTTP server is set up with the convenience API or manually.

Convenience API (run_http)
~~~~~~~~~~~~~~~~~~~~~~~~~~

For most deployments, use :cpp:func:`mcp::Server::run_http`. It manages the ``io_context``, sets up a :cpp:class:`mcp::StreamableHttpSessionManager` that serves each older-protocol client in a session of its own with the same server, and maps signals to shutdown.

.. literalinclude:: ../../examples/sync/server.cpp
   :language: cpp
   :start-at: // HTTP until a signal
   :end-at: server.run_http(
   :dedent: 4

``run_http()`` blocks until SIGINT or SIGTERM, or until ``server.stop()`` is
called from another thread or a handler. ``server.bound_port()`` returns the
port it listens on, which is the one the system chose when it was given port 0.

Manual HTTP Setup
~~~~~~~~~~~~~~~~~

To integrate the MCP server into an existing Boost.Asio application, set up the transport manually.

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
revision. All 2026-07-28 requests are answered by one shared
``Server``: the one passed to ``set_modern_server()``, or else one built from
the factory the first time it is needed. ``modern_server()`` returns it, and
builds it if no request has yet. Because every 2026-07-28 client sees the same
tools, resources and prompts, they must not depend on the connection.
``Server::run_http()`` serves through a manager, which keeps its default
settings unless the callback ``run_http()`` takes changes them before it
listens: the server itself answers 2026-07-28 requests, and each session gets a
server that shares its tools, resources, prompts, handlers and settings. A
manager constructed with a ``Server`` instead of a factory serves it the same
way, on the application's event loop, and the ``Server`` must outlive it.

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
session, with the same header checks and error statuses. The server that reads
the transport answers each request directly. The answer is plain JSON, or
a chunked event stream of the notifications and then the response under the
same conditions as on ``StreamableHttpSessionManager``. A
``subscriptions/listen`` request is an event stream that stays open until the client closes it, the server ends the
subscription or the transport closes. A client that disconnects while its
request runs cancels it, and so does a reset after a half-close. A client that
accepts ``text/event-stream`` and half-closes its connection gets an event stream
even when no notification is sent, carrying comments that probe whether it is
still there, because a client that closed cleanly cannot be told from one that
only half-closed until a write fails. The HTTP status of such an answer is 200
even for an error; the JSON-RPC body still carries the error. A client that
does not accept a stream gets plain JSON with the specified status, and a reset
after its half-close is found by checking the socket.

Notifications wait for the client to read them, so each request keeps at most
``max_queued_notifications`` of them (4096 by default) and ``max_queued_bytes``
(1 MiB by default, the size of the serialized messages, taken from the
``max_request_bytes`` budget too). A notification that would take the request
past a limit, or past that budget, is dropped and counted in
``StreamStats::dropped_notifications``; the handler is not told and the answer is
unchanged. A ``subscriptions/listen`` stream ends instead, with its closing
result, so that the client reconnects and lists again.

A request holds its place under ``HttpConnectionLimits::max_requests`` and
``max_request_bytes`` until its answer or stream has ended, and the connection
is closed when the client does not take a write within ``request_read_timeout``.
``set_stream_options()`` sets the keep-alive and probe intervals and the limits
on queued notifications, and ``stream_stats()`` counts what the streams did. ``set_json_only(true)`` keeps
the answers to plain JSON.

Request headers
~~~~~~~~~~~~~~~

A 2026-07-28 request over HTTP repeats some of its content in headers, so that
a gateway can route or limit it without reading the body. The server requires
them and checks them against the body:

* ``MCP-Protocol-Version`` and ``Mcp-Method`` on every request.
* ``Mcp-Name`` on ``tools/call``, ``prompts/get``, ``resources/read`` and, for the
  tasks extension, ``tasks/get``, ``tasks/update`` and ``tasks/cancel``, where
  it holds ``params.taskId``. A value in the ``=?base64?...?=`` form is decoded
  first.
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

Servers are restarted or moved in production. Graceful shutdown lets the server finish active requests before it exits, until the shutdown timeout expires.

.. _signal-handling:

Signal Handling
~~~~~~~~~~~~~~~

``run_stdio()`` and ``run_http()`` handle ``SIGINT`` and ``SIGTERM`` themselves,
and ``server.stop()`` ends them the same way. A program that runs the event loop
itself, with ``Server::run()``, handles the signals as below.

Handle ``SIGINT`` and ``SIGTERM`` to trigger the shutdown. ``mcp::graceful_shutdown()`` in ``mcp/core/signal.hpp`` closes the transport and stops the ``io_context`` after in-flight work finishes or times out.

.. literalinclude:: ../../examples/features/graceful_shutdown.cpp
   :language: cpp
   :start-after: // ========== GRACEFUL SHUTDOWN (auto-triggered for CI/testing) ==========
   :end-before: // ========== SERVER COROUTINE ==========
   :dedent: 8

The :cpp:func:`mcp::graceful_shutdown` function closes the transport (preventing new connections) and starts a watchdog timer. If requests do not finish within the timeout (defined by ``mcp::constants::shutdown::g_TIMEOUT_MS``), it stops the ``io_context`` forcefully. It returns the timer: cancel it once ``Server::run()`` has returned, as the example does, or the pending timer keeps ``io_context::run()`` going until the timeout.

Production Considerations
-------------------------

Port Binding and Security
~~~~~~~~~~~~~~~~~~~~~~~~~

- **Binding**: Use ``127.0.0.1`` if the server only needs to be accessed by a local proxy (like Nginx) or ``0.0.0.0`` to listen on all interfaces.
- **TLS/SSL**: The HTTP server transports end TLS themselves once ``set_tls()`` gives them a certificate, and can require client certificates; see :doc:`tls`. They can also sit behind a reverse proxy like Nginx, Apache, or a cloud load balancer that ends TLS for them.
- **Host check**: A request that reaches a server through a loopback address must name a loopback host (``localhost`` or a loopback address) in its ``Host`` header, or it is answered 403. This defends a local server against DNS rebinding, where a web page whose name points at ``127.0.0.1`` reaches it from the browser. A reverse proxy on the same machine that passes on the public name, as Nginx does with ``proxy_set_header Host $host``, needs that name listed with ``set_allowed_hosts({"mcp.example.com"})``. Once there is a list, every request must name a host on it, whatever address it reached.
- **Authentication**: For sensitive tools, implement authentication middleware or use the OAuth flow patterns demonstrated in the ``oauth_flow.cpp`` example.

Unix Daemon / Service Setup
---------------------------

A server run as a background process (daemon) stays alive and restarts on failure.

systemd Integration
~~~~~~~~~~~~~~~~~~~

On Linux, ``systemd`` manages the server as a service that starts on boot and restarts after a crash.

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

Use a multi-stage Dockerfile to keep the production image small.

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

Map the ports and pass configuration through environment variables.

Load Balancing and Scaling
--------------------------

Several instances can run behind a load balancer to scale horizontally.

Sticky Sessions
~~~~~~~~~~~~~~~

A session of 2025-11-25 or earlier is stateful (initialization, followed by multiple requests), so the load balancer must use **sticky sessions** (session affinity) for such clients. A request routed to an instance that has not seen the ``initialize`` request is rejected.

A 2026-07-28 request needs no session, and any instance that serves the same tools, resources and prompts can answer it. Three cases can still need the request to reach a particular instance:

- A request whose handler asks the client a question (``Context::elicit()``, ``sample_llm()``, ``request_roots()``) waits in the ``Server`` instance by default. The client's retry, which carries the ``requestState``, must reach that instance; a retry that does not is answered as for an unknown ``requestState``. With sealed request state (``MrtrOptions::state_mode``, see :doc:`/concepts/protocol`) the state travels in the ``requestState`` and any instance configured with the same ``sealing_keys`` serves the retry.
- A ``subscriptions/listen`` stream is one long-lived request, and the notifications it carries come from the instance that holds it.
- A task (see :doc:`/concepts/tasks`) lives in the ``TaskStore`` of the instance that started it, and its handler runs there. With the default in-memory store, ``tasks/get``, ``tasks/update`` and ``tasks/cancel`` must reach that instance: route on the ``Mcp-Name`` header, which holds the task id. An instance that does not know the task answers ``g_INVALID_PARAMS``.

Health Checks
~~~~~~~~~~~~~

The MCP protocol defines no health check. An HTTP deployment can add a tool or a parallel HTTP route to report the server's status.

- **Liveness**: Verify the process is running and the ``io_context`` event loop is responsive.
- **Readiness**: Verify that any external resources (databases, APIs, or model endpoints) that the tools depend on are currently available.

Monitoring and Logging
----------------------

Production servers should emit structured logs.

- **Standard Streams**: In Stdio mode, avoid writing anything to ``stdout`` except MCP messages. Use ``stderr`` for all logging.
- **Structured Logging**: When using HTTP, consider a logging library that outputs JSON for log aggregators such as ELK or Datadog.

Example Walkthrough
-------------------

``http_server_convenience.cpp`` is a runnable HTTP deployment that uses the high-level API.

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :start-after: // ========== CONVENIENCE SETUP (this example) ==========
   :end-before: constexpr auto startup_delay
   :dedent: 8

``graceful_shutdown.cpp`` handles system signals and manages the connection lifecycle manually, which suits a program that coordinates the shutdown of other services or databases with the MCP server.

.. literalinclude:: ../../examples/features/graceful_shutdown.cpp
   :language: cpp
   :start-after: // ========== SERVER COROUTINE ==========
   :end-before: // ========== RUN IO CONTEXT ==========
   :dedent: 8

Cross-References
----------------

- :doc:`/concepts/transports` for HTTP and Stdio transport specifics.
- :doc:`error-handling` for managing production errors and logging exceptions.
