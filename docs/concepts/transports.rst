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

In sync mode ``run_stdio()`` serves stdio on an event loop of its own and
blocks until the connection is closed. In async mode ``Server::run()`` serves a
``StdioTransport`` on the application's event loop, here an ``mcp::Runtime``:

.. literalinclude:: ../../examples/sync/notes_server.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The event loop:
   :end-before: }
   :dedent: 4

.. literalinclude:: ../../examples/async/notes_server.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The event loop:
   :end-before: }
   :dedent: 4

``run_stdio()`` returns when the client closes standard input, once the requests
already read have ended and their answers are written, and when standard output
is closed. When standard input ends, and on SIGINT or SIGTERM, running
handlers are told to stop, so one that checks ``Context::is_cancelled()`` ends
early. A signal ends the run once those handlers have ended, also while the
client has stopped reading, and after five seconds at most. Input lines are limited to
16 MiB; a longer one ends the input. To change that, build a ``StdioTransport``,
call ``set_max_message_bytes()`` and pass it to ``Server::run()``.

Client Side
~~~~~~~~~~~

``StdioClientTransport`` launches the server as a child process and owns it:
the server's standard input and output carry the messages, and the transport
ends the process when the client is done. In sync mode ``Endpoint::stdio()``
makes it for a client that runs its own event loop, from a command or from the
same ``StdioProcessOptions``; in async mode the application makes it on its own
event loop:

.. literalinclude:: ../../examples/sync/notes_client.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The client launches the server
   :end-before: // The session
   :dedent: 4

.. literalinclude:: ../../examples/async/notes_client.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The client launches the server
   :end-before: // The session
   :dedent: 4

The options of the process, in either mode:

.. code-block:: cpp

   #include <mcp/core/async.hpp>
   #include <mcp/client/client.hpp>
   #include <mcp/transport/stdio_client.hpp>

   mcp::StdioProcessOptions server;
   server.command = "my-mcp-server";
   server.arguments = {"--root", "/data"};
   server.environment = {{"MY_SERVER_LOG", "debug"}};
   server.working_directory = "/data";
   server.stderr_mode = mcp::StderrMode::eCapture;
   server.on_stderr_line = [](std::string_view line) { /* log it */ };

   boost::asio::co_spawn(executor, [&]() -> mcp::Task<void> {
       auto transport = std::make_shared<mcp::StdioClientTransport>(executor, server);
       mcp::Client client(transport, executor);

       co_await client.connect(client_info, {});
       // Interact with the server...

       client.close();
       const mcp::ProcessExit exit = co_await transport->shutdown();
   }, boost::asio::detached);

The constructor starts the process and throws ``mcp::TransportError`` when it
cannot (the program is not found, the working directory is missing). A command
without a directory is searched for in ``PATH``; a relative path is taken
relative to the client's working directory. ``environment`` adds to the client's
own environment, or is the whole environment with ``inherit_environment`` set to
``false``. What the server writes to standard error is inherited
(``StderrMode::eInherit``, the default), passed line by line to
``on_stderr_line`` (``eCapture``), or discarded (``eDiscard``); the
specification lets a client capture, forward or ignore it, and a line there is
not an error.

Shutdown follows the specification's stdio text. ``close()`` (which
``Client::close()`` calls) and ``shutdown()`` close the server's standard input and wait
``stdin_close_wait`` (two seconds by default) for it to exit. A server that is
still running then gets SIGTERM, and after ``terminate_wait`` SIGKILL. On
Windows, which has no signal to ask a process to exit, ``TerminateProcess``
follows the first wait. ``shutdown()`` resolves with a ``ProcessExit``: the exit
status, or the signal that ended the process (POSIX), and whether the
transport asked for the end (``requested``) and had to force it (``forced``). The
destructor ends the process the same way, so none is left behind. The server's
output is read and discarded from the moment shutdown begins, so a server that
writes while it exits is not held up.

By default (``own_process_tree = true``) the server runs as the leader of its own
process group on POSIX, or in a job object on Windows, and the signals above,
or termination of the job, go to the whole group. Helper
processes the server starts, such as the node process behind an ``npx``
launcher, then do not outlive it: when the server has ended, whatever is left of
its group or job is killed. On POSIX the server no longer receives the
terminal's Ctrl-C directly. ``own_process_tree = false`` signals the server
process alone. When the application ignores ``SIGCHLD`` the system reaps the
server itself, ``ProcessExit::code`` is ``-1``, and the leftover helpers are not
signalled.

A command is started directly, not through a shell. On Windows that finds
``.exe`` files only; ``npx`` and ``npm`` are ``.cmd`` scripts and do not start.
The application launches such a script through ``cmd.exe`` itself, and the SDK
never does it automatically:

.. code-block:: cpp

   mcp::StdioProcessOptions server;
   server.command = "cmd.exe";
   server.arguments = {"/c", "npx.cmd", "-y", "@modelcontextprotocol/server-everything"};

``cmd.exe`` interprets the arguments it is given (``&``, ``|``, ``%VAR%`` and
quotes have meaning there), so pass it only values the application controls.

The destructor closes the pipe through the transport's executor. It closes the
pipe directly only when the executor is an ``io_context`` that has stopped; with
an executor that no longer runs handlers in any other form, the process is ended
by the waits above instead.

A process that ends on its own is an unexpected termination: ``read_message()``
throws, the ``Client`` reports a closed connection (its pending requests fail
with ``g_CONNECTION_CLOSED`` and its subscriptions end as
``SubscriptionEnd::eDropped``), and ``on_exit`` runs with ``requested`` false.

The specification asks a client to restart a server that exits unexpectedly,
and to establish active ``subscriptions/listen`` streams again. A ``Client``
built from ``Endpoint::stdio()`` with a ``StdioRestartPolicy`` starts the server
again itself, and goes on with the new process, keeping its handlers, callbacks
and ``sync()`` view:

.. code-block:: cpp

   mcp::Client client(mcp::Endpoint::stdio({"./server"}, mcp::StdioRestartPolicy{.max_restarts = 3}));

The requests waiting for an answer when the process ended fail with
``g_CONNECTION_CLOSED``; requests made while it starts again wait for it, within
their own timeouts. A legacy session sends its ``initialize`` request to the new
process again. Each open ``subscriptions/listen`` request goes to the new
process again, so its ``Subscription`` goes on. What the old process kept for a
legacy session, such as a log level, resource subscriptions or tasks, is gone.
A start that fails uses one of the restarts the policy allows; when it allows
no more, the client closes as it does without a restart.

With a ``StdioClientSession`` the application opens the streams again: the
session makes a new ``Client`` for each process and runs the application's
``on_connected`` after each connect:

.. code-block:: cpp

   #include <mcp/core/async.hpp>
   #include <mcp/client/stdio_session.hpp>

   mcp::StdioSessionOptions options;
   options.process = server;
   options.client_info = {"my-client", "1.0"};
   options.restart.max_restarts = 3;
   options.configure = [](mcp::Client& client) {
       client.on_elicitation(handle_elicitation);  // before every connect
   };
   options.on_connected = [](mcp::Client& client, const mcp::ConnectResult&) -> mcp::Task<void> {
       mcp::SubscriptionFilter filter;
       filter.toolsListChanged = true;
       auto subscription = co_await client.listen(filter);  // after every restart too
       // keep the subscription for the application...
   };

   mcp::StdioClientSession session(executor, options);
   co_await session.start();
   co_await session.client()->call_tool("add", args);

When the process ends without the session or the transport having asked it to,
the session, like the ``Client`` above, waits ``restart.delay`` and starts the
server again, up to
``restart.max_restarts`` times over its life (the default is not to restart).
By default only an abnormal end restarts the server (a non-zero status or a
signal): one that exits with status 0 is shutting down by choice. Set
``restart.restart_after_clean_exit`` to restart after a clean exit too. A server
that ends before its connection is set up does not trigger a restart; ``start()``
throws and the process is shut down. At most one restart sequence runs at a
time.
It creates a new ``Client`` for the new process, runs ``configure`` and
connects it (a legacy session performs ``initialize`` again), then runs
``on_connected``, where the application opens its subscriptions. Requests in
flight when the server ended fail with ``g_CONNECTION_CLOSED``; the
application calls ``session.client()`` again and retries them.
``on_restart_failed`` runs when the policy allows no further start or a start
fails.

``examples/features/stdio_launch.cpp`` launches the example server both ways.

HTTP Transport (Network)
------------------------

The HTTP implementation accepts MCP POST requests and answers with JSON or an
SSE stream. In a session of protocol revision 2025-11-25 or earlier, a POST
from a client that accepts SSE becomes a stream as soon as the server sends
anything for the request before its response: the handler's notifications and
the server's own requests, such as ``elicitation/create``, go on that request's
stream as they are sent, and the response ends it. A POST answered before
anything else was sent gets that response alone. A GET opens the session's
stream for what the server sends outside any request, one per session, for a
client whose ``Accept`` lists ``text/event-stream`` (any other GET is answered
406); the transport sends a request's messages on its own stream, and anything else on
the GET stream, or on the stream of the only POST waiting when no GET is open.
Every event is kept in a bounded store, and a GET with ``Last-Event-ID``
resumes a stream from there: one still going on continues live, and one that
ended is replayed. A client leaving a stream does not cancel its request.
Managed sessions also support session IDs and DELETE teardown.

``HttpClientTransport`` reaches an ``https://`` URL over TLS, directly or through
an HTTP proxy (see :doc:`/guides/tls`). It sends each message on an exchange of
its own, so requests overlap, and reads an SSE answer as it arrives: a request the server
sends while it handles one of the client's reaches the client at once. After
notifications/initialized it opens the session's GET stream, and opens it again
from its last event when it ends; a server that answers that GET with 405 is
not asked again. A request's stream that ends before the response is resumed
with a GET naming its last event. After initialization the transport carries
the server-selected protocol version on subsequent session requests and
best-effort DELETE teardown, including when the initialize response arrived in
an SSE event.

A request of protocol revision 2026-07-28 needs no session. ``HttpClientTransport``
sends it as a POST of its own, with the ``MCP-Protocol-Version``, ``Mcp-Method``
and ``Mcp-Name`` headers (and ``Mcp-Param-*`` headers for tools that ask for
them), and no ``Mcp-Session-Id``. ``StreamableHttpSessionManager`` and
``HttpServerTransport`` serve such requests on the same endpoint as legacy
sessions. See :doc:`/guides/deployment` for the headers and the error statuses,
and :doc:`protocol` for the protocol itself.

The HTTP server transports serve plain HTTP unless ``set_tls()`` gives them a
certificate (see :doc:`/guides/tls`). Bind local development servers to
loopback, and serve remote clients over TLS, the transport's own or a reverse
proxy's. A request that reaches a server through a loopback address must name a
loopback host in ``Host`` unless ``set_allowed_hosts()`` lists others (see
:doc:`/guides/deployment`). Requests carrying an ``Origin`` header are denied
by default; configure an exact allowlist (or explicitly opt into all origins)
before starting the listener. Bearer-token validators must likewise be
installed before ``listen()`` or ``run()``.

HTTP Server Convenience
~~~~~~~~~~~~~~~~~~~~~~~

For developers who want a quick way to host an MCP server over HTTP without
worrying about the underlying networking boilerplate, the SDK provides `run_http()`.

``Server::run_http()`` owns its ``StreamableHttpSessionManager`` internally,
which gives each client of protocol revision 2025-11-25 or earlier a session of
its own. Its optional third argument is a callback that receives the manager
before it listens, where the Origin allowlist, bearer-token validation and the
other manager settings are set. Without one, the manager keeps its default
settings: use it that way only on a suitable trusted boundary, such as
loopback. In async mode the application constructs a
``StreamableHttpSessionManager`` with the ``Server`` instead, configures it and
runs ``listen()`` on its own event loop. Both servers here set a bearer token
validator and an allowed origin:

.. literalinclude:: ../../examples/sync/server.cpp
   :language: cpp
   :caption: Sync mode
   :start-at: // HTTP until a signal
   :end-at: server.run_http(
   :dedent: 4

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :caption: Async mode
   :start-at: // The event loop:
   :end-at: io.run();
   :dedent: 4

A client connects with ``Endpoint::http(url)`` in sync mode, and with an
``HttpClientTransport`` on its own event loop in async mode. These send a bearer
token:

.. literalinclude:: ../../examples/sync/client.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The connection:
   :end-before: // Logging:
   :dedent: 4

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The connection:
   :end-before: // Logging:
   :dedent: 4

Manual HTTP Configuration
~~~~~~~~~~~~~~~~~~~~~~~~~

If you need to integrate the MCP transport into an existing HTTP server (like
one built with Boost.Beast), you can manage the `HttpServerTransport` lifecycle
yourself.

.. literalinclude:: ../../examples/features/http_server_convenience.cpp
   :language: cpp
   :start-after: using namespace mcp;
   :end-before: // ========== CONVENIENCE SETUP (this example) ==========
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

``WebSocketServerTransport`` refuses an upgrade request that carries an
``Origin`` header with ``403`` unless that origin was added with
``set_allowed_origins()`` or ``set_allow_all_origins(true)`` was called, as
the HTTP transports do. An upgrade without an ``Origin`` header is accepted.
Its ``Host`` header is checked as the HTTP servers check it, with
``set_allowed_hosts()``. Configure both before the first ``read_message()`` or
``write_message()``, which perform the handshake. Both transports also speak
TLS (wss); see :doc:`/guides/tls`.

Three limits protect a server from a peer that stops cooperating, and take
effect when set before that first read or write:

- ``set_handshake_timeout()`` limits the upgrade, 30 seconds by default. Give
  each accepted socket its own coroutine: accepting one at a time lets a socket
  that says nothing hold up the next one for this long.
- ``set_idle_timeout()`` closes a connection whose peer has gone quiet or stopped
  reading, five minutes by default. A quiet peer is pinged first, so one that
  answers pings stays connected however idle the session is.
- ``set_max_message_bytes()`` sets the largest message accepted, 16 MiB by
  default; a larger one ends the connection. The client transport has the same
  setter, and needs it to receive larger results.

MemoryTransport for Testing
---------------------------

Testing is a critical part of developing MCP integrations. The `MemoryTransport`
allows you to run both a server and a client in the same process, communicating
entirely in memory. This eliminates the need for network configuration or local
process management during testing.

Creating Transport Pairs
~~~~~~~~~~~~~~~~~~~~~~~~

You should always use the `make_memory_transport_pair` helper to ensure that
both ends of the transport are correctly linked.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :start-after: // Create bidirectional memory transport pair
   :end-before: std::cout << "[Main] Created MemoryTransport pair
   :dedent: 8

Usage in Integration Tests
~~~~~~~~~~~~~~~~~~~~~~~~~~

Because `MemoryTransport` implements the same `ITransport` interface as all other
transports, your server and client code remain identical to production.

.. literalinclude:: ../../examples/features/transport_memory.cpp
   :language: cpp
   :start-after: std::cout << "[Main] Transports are bidirectionally connected
   :end-before: // ========== RUN IO CONTEXT ==========
   :dedent: 8

A default ``Client`` and a default ``Server`` speak protocol revision 2026-07-28
over this transport. Set ``ClientOptions::protocol`` to test an older revision;
see :doc:`/guides/older-protocols`.

For more information on writing effective tests, see the :doc:`/guides/testing` guide.

Runtime
-------

``mcp::Runtime`` is an event loop that a program uses without Boost:
``executor()`` binds a transport, a ``Client`` or ``Server::run()`` to it, such
as ``std::make_shared<mcp::StdioTransport>(runtime.executor())``, ``spawn()``
starts a coroutine on it, ``run()`` runs the loop on the calling thread until
its work is done, and ``start(threads)`` runs it on threads of its own until
``stop()``. The async stdio server and client above run on one.

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
|                      |                          | - TLS of its own or from a reverse proxy       |
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

   #include <mcp/core/async.hpp>
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
