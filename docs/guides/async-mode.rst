Async Mode
==========

In async mode the program runs the Boost.Asio event loop itself, and its
handlers and client calls are C++20 coroutines that return ``mcp::Task``. This
guide walks through ``examples/async/server.cpp`` and
``examples/async/client.cpp``, a server and a client with every feature of the
sync-mode ``server_all.cpp`` and ``client_all.cpp``. What each feature does is
explained in :doc:`../concepts/index`; this page covers what async mode changes.
:doc:`growing-a-server` and :doc:`growing-a-client` move a sync-mode program to
async mode one step at a time, and :doc:`../examples` shows how to run the two
programs.

Coroutines
----------

``<mcp/core/async.hpp>`` defines ``mcp::Task<T>``, the return type of a
coroutine that produces a ``T``. A ``Task`` is a ``boost::asio::awaitable<T>``:
it awaits Boost.Asio operations through ``boost::asio::use_awaitable``, and
``boost::asio::co_spawn`` starts it, or a callable that returns it, on an
executor.

GCC 11, 12 and 13 have coroutine bugs that the programs avoid, for instance by
building each argument of a ``co_await`` in a named variable first;
:ref:`known-issues` lists them.

The Server
----------

Coroutine Handlers
^^^^^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :start-at: // A coroutine handler
   :end-before: // Resources, one that
   :dedent: 4

The server runs each request in a coroutine of its own. A handler that returns
``mcp::Task`` suspends at each ``co_await``, and the server answers other
requests until it resumes. This one waits on a timer of the executor it runs
on, which ``boost::asio::this_coro::executor`` gives. A handler that blocks
instead holds up the thread it runs on. The ``Context`` carries the request's
log messages and progress (:doc:`../concepts/context`).

Resources, Prompts and Requests to the Client
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :start-at: // Resources, one that
   :end-before: // Middleware, lists in pages
   :dedent: 4

Text resources, prompts and the completion provider take coroutines as tools
do. See :doc:`../concepts/resources`, :doc:`../concepts/subscriptions`,
:doc:`../concepts/prompts`, :doc:`../concepts/completions`,
:doc:`../concepts/elicitation`, :doc:`../concepts/roots`, and
:doc:`../concepts/context` for requests to the client's model.

Middleware, Pages, Tasks, Apps and Skills
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :start-at: // Middleware, lists in pages
   :end-before: // Tool, resource and prompt handlers run on a pool
   :dedent: 4

See :doc:`../concepts/middleware`, :doc:`../concepts/pagination`,
:doc:`../concepts/tasks`, :doc:`../concepts/apps` and
:doc:`../concepts/skills`.

A Pool for Handlers
^^^^^^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :start-at: // Tool, resource and prompt handlers run on a pool
   :end-before: // The event loop:
   :dedent: 4

``set_handler_executor()`` moves the call of each tool, resource and prompt
handler to the pool's threads, and the server goes back to its own executor when
the handler returns. Reading, parsing and answering messages stay on the event
loop, so a handler that blocks or computes for long does not hold it up. A
handler that only awaits needs no pool. Handlers that run on the pool share
state through a mutex, as ``remember`` does. The pool's ``Runtime`` is
constructed before the ``io_context``, so that it is destroyed after it, as
``set_handler_executor()`` requires.

The Event Loop
^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :start-at: // The event loop:
   :end-at: io.run();
   :dedent: 4

A ``StreamableHttpSessionManager`` constructed with the server serves it as
``run_http()`` does (:doc:`deployment`), and the settings that ``run_http()``'s
callback makes are made on it before ``listen()``. ``listen()`` accepts
connections until ``close()``, which the program calls on SIGINT or SIGTERM, as
``run_http()`` does itself; ``io.run()`` then returns once the handlers still
running have finished.
:ref:`signal-handling` shows a shutdown that also sets a deadline for them. A
stdio server is one more coroutine on the loop, ``server.run(transport,
executor)``, as :doc:`growing-a-server` shows.

The Client
----------

Construction and Handlers
^^^^^^^^^^^^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :start-at: // The transport and the client run
   :end-before: // co_spawn hands what
   :dedent: 4

A client constructed with a transport and an executor runs on the program's
event loop and is used through its coroutines; ``sync()`` and ``run()`` are for
a client that runs a loop of its own (:doc:`growing-a-client`). The handlers for
the server's requests are coroutines in both modes.

Starting the Session
^^^^^^^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :start-at: // co_spawn hands what
   :end-before: // Two calls in flight
   :dedent: 4

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :start-at: // The session: connect
   :end-at: io.run();
   :dedent: 4

``co_spawn`` is given the lambda itself, not the result of calling it: it keeps
the lambda, and so the captures its coroutine reads, alive until the coroutine
finishes. What the coroutine throws goes to the completion handler.
``boost::asio::detached`` ignores it; ``rethrow`` makes ``io.run()`` throw it.
``io.run()`` returns once no work is left, here after the client is closed.

Awaiting Calls
^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :start-at: // Every feature of the client
   :end-before: int main(

Each call is awaited in turn. See :doc:`../concepts/context` for progress and
log messages, :doc:`../concepts/resources`, :doc:`../concepts/prompts`,
:doc:`../concepts/completions`, :doc:`../concepts/tasks`,
:doc:`../concepts/subscriptions`, :doc:`error-handling` and
:doc:`../concepts/pagination`.

Calls in Flight Together
^^^^^^^^^^^^^^^^^^^^^^^^

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :start-at: // Two calls in flight
   :end-before: // The session: connect
   :dedent: 4

Each ``co_spawn`` starts a coroutine that runs on its own, so both calls wait
for their answers at the same time. The last one to return closes the client.

Another Event Loop
------------------

The programs run a ``boost::asio::io_context``. ``mcp::Runtime`` is an event
loop that the SDK provides: its ``executor()`` takes the place of
``io.get_executor()``, ``spawn()`` starts a ``Task`` and ``run()`` runs the
loop, as the last steps of :doc:`growing-a-server` and :doc:`growing-a-client`
show.
