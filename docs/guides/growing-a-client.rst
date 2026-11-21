Growing a Client
================

The client of :doc:`/getting-started`, ``examples/sync/client_base.cpp``, grows
one feature at a time, as the server does in :doc:`growing-a-server`. Each
section below is a compiled program in ``examples/sync`` that is the base
program plus one feature, shown as a diff against the base. CI checks that each
step changes only the lines it names (``scripts/check_example_progression.py``)
and runs each program against a server from the same directory.

The base program is in sync mode (see :doc:`/getting-started`). The step "Await
Calls in a Coroutine" uses async mode inside it, and "Run the Event Loop
Yourself" runs the client on the program's own event loop, in async mode only.

The base program, without its opening comment:

.. literalinclude:: ../../examples/sync/client_base.cpp
   :language: cpp
   :start-at: #include

Read a Resource
---------------

``read_resource(uri)`` reads a resource, such as the message of the day that the
server of :doc:`growing-a-server` adds in "Add a Resource". Its ``contents``
hold one ``TextResourceContents`` or ``BlobResourceContents`` per part; this one
is text. ``list_resources()`` lists the resources a server offers. See
:doc:`/concepts/resources`.

.. literalinclude:: /_generated/progression/client_add_resource.diff
   :language: diff

Get a Prompt
------------

``get_prompt(name, arguments)`` fills in a prompt with arguments given by name,
such as the welcome prompt that the server adds in "Add a Prompt". Its
``messages`` are the conversation to hand to a model; this one is one text
message. ``list_prompts()`` lists a server's prompts and the arguments each
takes. See :doc:`/concepts/prompts`.

.. literalinclude:: /_generated/progression/client_add_prompt.diff
   :language: diff

Connect over HTTP
-----------------

``mcp::Endpoint::http(url)`` connects to a Streamable HTTP server, such as the
one of :doc:`growing-a-server` that serves over HTTP, instead of launching a
process. Only the endpoint changes; the calls stay as they are. A server that
cannot be reached makes ``connect()`` throw ``mcp::McpError``, as "Handle
Errors" below describes. See :doc:`/concepts/transports`.

.. literalinclude:: /_generated/progression/client_add_http.diff
   :language: diff

Send a Bearer Token
-------------------

``Endpoint::http()`` takes ``HttpEndpointOptions``. Its ``configure`` function
receives the ``HttpClientTransport`` before the client uses it, for a bearer
token provider, a response timeout or a warning handler. Its ``authenticator``
member makes an ``auth::Authenticator`` instead, which provides the access
tokens and refreshes them, and the client then talks to the server through an
``auth::OAuthClientTransport``; see :doc:`/concepts/oauth`.

.. literalinclude:: /_generated/progression/client_add_auth.diff
   :language: diff

Follow the Progress of a Call
-----------------------------

``RequestOptions::on_progress`` receives the progress the server reports for
one request. The client asks for it by sending a progress token with the
request, and calls the function for each report until the answer arrives.
``read_resource()``, ``get_prompt()``, ``complete()``, ``start_tool()`` and the
``list_*()`` calls take ``RequestOptions`` too, and so do their blocking forms.
See :doc:`/concepts/context`.

.. literalinclude:: /_generated/progression/client_add_progress.diff
   :language: diff

Receive Log Messages
--------------------

``on_notification()`` registers a function for a notification method, here the
server's log messages. ``RequestOptions::log_level`` asks the server to send the
messages of that request at that level and above; a session of protocol revision
2025-11-25 or earlier sets one level for all its requests with a
``logging/setLevel`` request instead.

.. literalinclude:: /_generated/progression/client_add_logging.diff
   :language: diff

Answer the Server's Questions
-----------------------------

``on_elicitation()`` registers the function that answers a server's
``ctx.elicit()``, such as the one of :doc:`growing-a-server`. A real client
shows the form to its user and returns what they entered, or declines.
Registered before ``connect()``, the handler also declares the elicitation
capability, which a session of protocol revision 2025-11-25 or earlier sends
only then. The handler is a coroutine, so the program includes
``<mcp/core/async.hpp>``; the calls stay blocking. See
:doc:`/concepts/elicitation`.

.. literalinclude:: /_generated/progression/client_add_elicitation.diff
   :language: diff

Answer Requests for Model Output
--------------------------------

``on_sampling()`` registers the function that answers a server's
``ctx.sample_llm()``. A real client passes the messages to its language model
and returns the model's reply. Like the elicitation handler, it declares its
capability when registered before ``connect()``. See :doc:`/concepts/context`.

.. literalinclude:: /_generated/progression/client_add_sampling.diff
   :language: diff

Provide Roots
-------------

``on_roots_list()`` registers the function that answers a server's
``ctx.request_roots()`` with the client's roots; ``set_roots()`` gives a fixed
list instead. Protocol revision 2026-07-28 deprecates roots. See
:doc:`/concepts/roots`.

.. literalinclude:: /_generated/progression/client_add_roots.diff
   :language: diff

Run a Tool as a Task
--------------------

``start_tool()`` calls a tool and declares the tasks extension of protocol
revision 2026-07-28 for that call. It returns the tool's result when the server
ran the call at once, or a ``TaskHandle`` when the server made it a task; the
handle's ``sync().result()`` waits for the task to finish and returns the
tool's result. See :doc:`/concepts/tasks`.

.. literalinclude:: /_generated/progression/client_add_tasks.diff
   :language: diff

Subscribe to a Resource
-----------------------

``listen()`` opens a ``subscriptions/listen`` stream of protocol revision
2026-07-28 for the notifications a ``SubscriptionFilter`` names, here the
updates of one resource. ``unhonored()`` names what the server did not agree to
send, and the stream's ``sync().next()`` waits for the next notification, or
returns nothing once the stream has ended. A session of an earlier revision
subscribes with a ``resources/subscribe`` request instead. See
:doc:`/concepts/subscriptions`.

.. literalinclude:: /_generated/progression/client_add_subscriptions.diff
   :language: diff

Handle Errors
-------------

A tool that fails says so in its result: ``isError`` is set and the content
says why, as for these arguments, which lack the name the tool's schema
requires. A request the server refuses throws ``mcp::McpError``, whose
``code()`` is the JSON-RPC error code, here invalid params for a resource that
does not exist. A request that times out throws ``McpError`` too, with code
``-32001``, and so does a connection that fails, with code ``-31000``
(``mcp::constants::jsonrpc::g_CONNECTION_CLOSED``): a stdio server that exits,
an HTTP server that cannot be reached or drops the connection, or one that
answers with an HTTP error status, such as 401 for a missing or wrong bearer
token, which ``data()`` holds as ``httpStatus``. The transport's own exception
is nested in it, for ``std::rethrow_if_nested()``, except when the server
answers with an event stream that breaks off: the message alone says why then.
See :doc:`error-handling`.

.. literalinclude:: /_generated/progression/client_add_errors.diff
   :language: diff

Await Calls in a Coroutine
--------------------------

``client.run()`` runs a coroutine on the client's own event loop and blocks
until it ends, rethrowing what it throws. Inside it the client is used in async
mode: its coroutines are awaited with ``co_await``, for calls that depend on each other or run together;
the program includes ``<mcp/core/async.hpp>``, which defines ``mcp::Task``. The
blocking calls before it stay as they are.

.. literalinclude:: /_generated/progression/client_add_coroutine.diff
   :language: diff

Start the Server Again When It Ends
-----------------------------------

``Endpoint::stdio()`` takes a ``StdioRestartPolicy``. When the server process
ends without the client having asked it to, the same client starts it again, up
to ``max_restarts`` times, and goes on with the new process. Requests that were
waiting for an answer fail with ``g_CONNECTION_CLOSED``; requests made while the
server starts again wait for it. By default only an abnormal end (a non-zero
status or a signal) restarts the server.

.. literalinclude:: /_generated/progression/client_add_restart.diff
   :language: diff

Talk to an Older Server
-----------------------

By default ``connect()`` sends ``server/discover`` to learn whether the server
speaks protocol revision 2026-07-28, and sends ``initialize`` when it does not.
``ClientOptions::protocol = ClientProtocol::legacy()`` sends ``initialize``
alone, for a server that closes the connection when it receives a request it
does not know. ``connect()`` returns what the server declared, with the
protocol version agreed on. This step replaces the client's construction and
its ``connect()`` line. See :doc:`older-protocols`.

.. literalinclude:: /_generated/progression/client_add_legacy.diff
   :language: diff

Run the Event Loop Yourself
---------------------------

A client constructed with a transport and an executor runs on the program's
own event loop, here an ``mcp::Runtime``. Such a client is used in async mode,
through its coroutines: ``sync()`` and ``run()`` throw ``std::logic_error`` there, so the
calls move into a coroutine. This is the one step that changes the calls
themselves; any other transport can be used with the event loop the client owns
through ``Endpoint::custom()``, which keeps ``sync()`` working.

.. literalinclude:: /_generated/progression/client_add_own_loop.diff
   :language: diff

Every Step Together
-------------------

``client_all.cpp`` combines the steps that do not replace the same line in
another way: HTTP with a bearer token, log messages and progress for the call,
the answers to the server's questions, model requests and roots, the task, the
subscription, the error handling, and a coroutine on the client's loop. It
talks to ``server_all.cpp``. Plain HTTP, the restart and the older protocol
replace the client's construction, and running the event loop yourself replaces
the calls.

.. literalinclude:: /_generated/progression/client_all.diff
   :language: diff
