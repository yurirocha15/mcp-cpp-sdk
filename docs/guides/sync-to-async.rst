Converting to Async Mode
========================

A server and a client written in sync mode are converted to async mode. The
server keeps notes: a tool adds one, a resource lists them
and tells its subscribers when they change, and a prompt asks for a summary.
The client subscribes to the notes, adds one, waits for the change, then reads
the notes and gets the prompt. Both talk over stdio. :doc:`async-mode` describes
async mode in full.

The Sync Programs
-----------------

.. literalinclude:: ../../examples/sync/notes_server.cpp
   :language: cpp
   :caption: examples/sync/notes_server.cpp
   :start-at: #include

.. literalinclude:: ../../examples/sync/notes_client.cpp
   :language: cpp
   :caption: examples/sync/notes_client.cpp
   :start-at: #include

Converting the Server
---------------------

Include the Coroutine Header
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Code that defines or awaits a coroutine includes ``<mcp/core/async.hpp>``,
which defines ``mcp::Task``. The server below also includes
``<mcp/transport/stdio.hpp>`` for the ``StdioTransport`` it serves.

Make the Handlers Coroutines
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A handler that returns a value becomes a coroutine that returns ``mcp::Task``
of that value and ends with ``co_return``. The registration calls stay as they
are: ``add_tool()``, ``add_text_resource()`` and ``add_text_prompt()`` take
either form.

.. literalinclude:: ../../examples/sync/notes_server.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The resource:
   :end-before: // The tool:
   :dedent: 4

.. literalinclude:: ../../examples/async/notes_server.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The resource:
   :end-before: // The tool:
   :dedent: 4

A blocking call in a handler becomes an awaited one. The tool sends its
notification with ``co_await server.notify_resource_updated()`` instead of the
blocking ``server.sync().notify_resource_updated()``:

.. literalinclude:: ../../examples/sync/notes_server.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The tool:
   :end-before: // The prompt:
   :dedent: 4

.. literalinclude:: ../../examples/async/notes_server.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The tool:
   :end-before: // The prompt:
   :dedent: 4

The prompt changes the same way:

.. literalinclude:: ../../examples/sync/notes_server.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The prompt:
   :end-before: // The handler pool
   :dedent: 4

.. literalinclude:: ../../examples/async/notes_server.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The prompt:
   :end-before: // The event loop
   :dedent: 4

Remove the Handler Pool
^^^^^^^^^^^^^^^^^^^^^^^

The sync server runs its handlers on a pool of threads, so that
``server.sync()`` can wait for the notification it sends from the server's event
loop:

.. literalinclude:: ../../examples/sync/notes_server.cpp
   :language: cpp
   :start-at: // The handler pool
   :end-before: // The event loop
   :dedent: 4

The coroutines await the notification instead, so the async server needs no
pool. Its handlers run one at a time on the thread that runs the event loop, and
the notes need no mutex. A server whose handlers block or compute for long keeps
a handler pool in async mode too; see :doc:`async-mode`.

Run the Event Loop
^^^^^^^^^^^^^^^^^^

``run_stdio()`` runs an event loop of its own. In async mode the program runs
it: ``Server::run()`` is a coroutine that serves one transport, here a
``StdioTransport``, which ``runtime.spawn()`` starts on an ``mcp::Runtime``.
``runtime.run()`` runs the loop until standard input closes. A
``boost::asio::io_context`` can take the place of the ``Runtime``, as in
:doc:`async-mode`.

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

``run_stdio()`` also handled SIGINT and SIGTERM. A program that runs the event
loop handles them itself, for instance with ``mcp::graceful_shutdown()``; see
:ref:`signal-handling`.

Converting the Client
---------------------

Make the Transport on the Program's Event Loop
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``Endpoint::stdio()`` gives the client an event loop of its own. In async mode
the program makes a ``StdioClientTransport`` from ``StdioProcessOptions`` on its
event loop, and constructs the client with the transport and the loop's
executor. The code that defines the coroutine includes
``<mcp/core/async.hpp>`` and ``<mcp/transport/stdio_client.hpp>``.

.. literalinclude:: ../../examples/sync/notes_client.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The client launches the server
   :end-before: // The calls
   :dedent: 4

.. literalinclude:: ../../examples/async/notes_client.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The client launches the server
   :end-before: // The calls
   :dedent: 4

Await the Calls in a Coroutine
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Each blocking call of ``client.sync()`` becomes an awaited call of the
``Client``, and the subscription's ``sync().next()`` and ``sync().close()``
become ``co_await subscription.next()`` and ``co_await subscription.close()``.
The calls move into a coroutine, which ``runtime.spawn()`` starts. The coroutine
closes the client when it is done, after which ``runtime.run()`` returns. The
arguments of each ``co_await`` are built in named variables first, as GCC 11 to
13 need; see :ref:`known-issues`.

.. literalinclude:: ../../examples/sync/notes_client.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // The calls:
   :end-at: prompt.messages.at(0)
   :dedent: 4

.. literalinclude:: ../../examples/async/notes_client.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // The calls:
   :end-at: runtime.run();
   :dedent: 4

``sync()`` throws ``std::logic_error`` on a client constructed with a transport
and an executor.

Coroutines in a Sync-Mode Client
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A client constructed from an ``Endpoint`` can also run a coroutine on its own
event loop: ``client.run()`` blocks until the coroutine ends and rethrows what
it throws. Inside it the client's calls are awaited as in async mode:

.. literalinclude:: ../../examples/sync/client.cpp
   :language: cpp
   :start-after: // Coroutines on the client's own event loop
   :end-at: client.run(greet_in_a_coroutine);
   :dedent: 4

The Async Programs
------------------

.. literalinclude:: ../../examples/async/notes_server.cpp
   :language: cpp
   :caption: examples/async/notes_server.cpp
   :start-at: #include

.. literalinclude:: ../../examples/async/notes_client.cpp
   :language: cpp
   :caption: examples/async/notes_client.cpp
   :start-at: #include

Each page of :doc:`../concepts/index` shows its feature in both modes.
