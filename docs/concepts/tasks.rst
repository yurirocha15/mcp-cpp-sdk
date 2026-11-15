Tasks
=====

A tool call can take longer than a client wants to wait inside one request. The
tasks extension (``io.modelcontextprotocol/tasks``) of protocol revision
2026-07-28 lets a server answer such a ``tools/call`` at once with a task
handle, run the tool apart from the request, and keep its outcome for the client
to fetch. Sessions of 2025-11-25 and earlier have no tasks in this SDK: a legacy
peer never sees one.

The server decides, call by call, whether a call becomes a task. A client only
declares that it understands the extension, and the server never returns a task
to a client that did not declare it.

Serving tasks
-------------

Enable tasks on the server, then choose which tools may become tasks:

.. code-block:: cpp

   mcp::Server server({"reports", "1.0"}, capabilities);
   server.set_handler_executor(pool.get_executor());

   server.add_tool<json, json>("build_report", "Builds a report", schema, handler);

   mcp::TaskOptions options;
   options.ttl = std::chrono::hours(1);
   options.poll_interval = std::chrono::seconds(2);
   server.enable_tasks(options);
   server.set_tool_task_mode("build_report", mcp::ToolTaskMode::eAuto);

``Server::enable_tasks()`` advertises the extension in the ``server/discover``
result (``capabilities.extensions``) and starts answering ``tasks/get``,
``tasks/update`` and ``tasks/cancel``. A server of protocol revision 2025-11-25
and earlier does not advertise it in ``initialize``. Call it before the server
serves its first request; it throws ``std::logic_error`` afterwards.

``set_tool_task_mode()`` sets what a tool does for a client that declared the
extension. The mode is ``ToolTaskMode::eNever`` until set, and it may be set
before or after ``enable_tasks()``; it takes effect once tasks are enabled.

``eNever``
   The call runs inside its request, as without the extension.
``eAuto``
   A client that declared the extension gets a task handle. Any other client
   waits for the result as usual.
``eRequired``
   As ``eAuto``, except that a client that did not declare the extension is
   answered with ``g_MISSING_REQUIRED_CLIENT_CAPABILITY`` naming the extension
   in ``data.requiredCapabilities``. A legacy session is answered the same way.

The server never turns a call that is already running into a task after some
time; the decision is made when the request arrives. A tool that has to ask the
client something before its task exists sets a preparation, described next.

Asking before the task is created
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``set_tool_task_preparation()`` gives a tool a coroutine that runs inside the
``tools/call`` of a client that declared the extension, before the task exists.
It runs only for a call that would create a task: tasks are enabled, the tool's
mode is ``eAuto`` or ``eRequired`` and the client declared the extension. A call
of any other kind is unchanged, and the preparation is not called for it.

.. code-block:: cpp

   server.set_tool_task_preparation(
       "build_report",
       [](mcp::Context& ctx, const json& arguments) -> mcp::Task<mcp::TaskPreparation> {
           const mcp::InputOptions options{.key = "user_name"};
           const auto answer = co_await ctx.elicit(name_form(), options);
           mcp::TaskPreparation preparation;
           preparation.arguments = arguments;
           preparation.arguments["name"] = answer.content.value_or(json::object()).value("name", "");
           co_return preparation;
       });

The preparation gets the Context a synchronous handler of the tool would get, so
``Context::elicit()``, ``sample_llm()`` and ``request_roots()`` work through the
multi round-trip flow of the request, in stored and in sealed request state: the
client is answered with an ``input_required`` result without a ``taskId``, and
the retry that carries the answers gets the ``CreateTaskResult``, with no
``requestState`` and no ``inputRequests``. A client that cannot answer what the
preparation asks gets what it would get from a synchronous handler. The task's
handler is then called with ``TaskPreparation::arguments``. If the preparation
sets ``TaskPreparation::result`` instead, the call is answered with that result
and no task is created. A preparation that throws answers the call the way a
tool handler that throws does.

Middleware sees such a call twice: around the preparation, where it may reject
the call, rewrite the parameters the preparation receives, or answer without
calling the next handler, in which case no task is created; and later around
the task's handler. When the preparation returns arguments, the middleware
around it sees an empty ``CallToolResult`` as the answer. The task limits
described below are checked before the preparation runs, so a client at its
limit is answered without being asked anything, and again when the task is
created. A request cancelled while the preparation runs creates no task. Like
``set_tool_task_mode()``, the preparation can be set
only for a registered tool and before the server serves its first request.

``TaskOptions`` sets the time to live of a new task (``ttl``, or unset to keep
it without limit), the polling interval suggested to clients, and two limits on
handlers that run at once: ``max_tasks_per_principal`` and ``max_tasks`` for all
principals. A task counts against them from its creation until its handler
returns, whatever its status. A cancelled task, or one whose time to live passed,
keeps counting while its handler still runs, so a handler that ignores
``Context::is_cancelled()`` holds its slot. A call that would pass a limit is
answered with a JSON-RPC internal error. The defaults are starting points, and
the limits belong to the deployment.

What a task's handler sees
~~~~~~~~~~~~~~~~~~~~~~~~~~

The answer to the ``tools/call`` is a ``CreateTaskResult``: ``resultType`` is
``"task"``, with the ``taskId``, ``status``, ``createdAt``, ``lastUpdatedAt``,
``ttlMs`` and ``pollIntervalMs`` of the task. The server builds it before the
handler starts, so it always describes a task that is ``working``, and the
task is findable by ``tasks/get`` from then on.

The handler runs on the executor of ``Server::set_handler_executor()`` when one
is set, and otherwise on the executor of the request. A handler that blocks
stalls whatever else runs there, so ``set_tool_task_mode()`` writes a warning
(``TaskOptions::warning_handler``, standard error by default) when no handler
executor is set.

The handler is the same function as for a call that runs in place, and what it
returns becomes the task's outcome:

- A result, including one with ``isError: true`` (and so a handler that
  throws), completes the task. ``tasks/get`` then returns the result in
  ``result``.
- A JSON-RPC error from the call itself, for example a result that does not fit
  the tool's output schema or an exception from middleware, fails the task.
  ``tasks/get`` then returns the error in ``error``.
- ``Context::is_cancelled()`` becomes true when the task is cancelled, and when
  the server is destroyed.

Notifications of the handler have no request to travel with. ``Context::log()``
sends nothing from a task, and ``Context::report_progress()`` sets the task's
``statusMessage`` to the message of the report, or to ``Progress: 3 of 10``
without one. A handler that is not a coroutine reports the same way with
``Context::report_progress_sync()``, which does not suspend. It returns false
and does nothing outside a task.

Asking the client
~~~~~~~~~~~~~~~~~

``Context::elicit()``, ``sample_llm()`` and ``request_roots()`` work inside a
task. The question goes into the task's ``inputRequests``, the task's status
becomes ``input_required``, and the handler waits until the client answers with
``tasks/update``. The key of each question is unique for the life of the task:
the SDK generates it, or the handler names it with ``InputOptions`` (an empty
key, one that looks like a generated name or one the task used before makes the
call throw ``std::invalid_argument``). An answer to a key that is not
outstanding is ignored. A question the client did not declare the capability for is not asked: the call throws, which ends the
tool with an ``isError`` result.

Who can read a task
~~~~~~~~~~~~~~~~~~~

A task belongs to the principal that started it (``Context::principal()``), and
only that principal can fetch, answer or cancel it. Another principal's request
for the task is answered as if it did not exist.

When the transport authenticates no principal, as on stdio, WebSocket and
in-memory transports and on HTTP without an ``Authorization`` header, every
caller is the same anonymous principal. The task id then is all that protects a
task: ids are 128 random bits from the system's cryptographic generator, so they
cannot be guessed, but anyone who learns one can read the task. Serve tasks to
anonymous callers only when that is acceptable.

Where tasks are kept
~~~~~~~~~~~~~~~~~~~~

``TaskOptions::store`` takes a ``TaskStore``, the interface the server keeps its
tasks through. Without one, the server uses an ``InMemoryTaskStore``, which
holds tasks in the memory of the process: they are gone when the process ends,
and tasks of a restarted server cannot be fetched. An application that needs
more implements ``TaskStore`` over its own storage. The store records state
only; the handler of a task runs in the process that started it, so a task that
was running when the process ended does not resume.

A store must be thread-safe, and each of its calls must return quickly. See
``mcp/server/task_store.hpp`` for the contract of each method. The in-memory
store expires a task once its ``ttlMs`` has passed, refuses a new task past its
per-owner and total limits, and makes room for the oldest finished task of an
owner before it refuses.

Following tasks from a client
-----------------------------

A client declares the extension per request. ``ClientOptions::tasks`` is
``ClientTaskMode::eOff`` unless set, and a client that leaves it off does not
declare the extension, so a server never answers it with a task. With
``ClientTaskMode::eAuto``, a 2026-07-28 session declares the extension and
``Client::call_tool()`` follows a task the server starts:

.. code-block:: cpp

   mcp::ClientOptions options;
   options.tasks = mcp::ClientTaskMode::eAuto;
   mcp::Client client(transport, executor, options);
   co_await client.connect("reports-client", "1.0");

   // Returns the tool's result, whether the server ran it in place or as a task.
   auto result = co_await client.call_tool("build_report", arguments);

``call_tool()`` polls ``tasks/get`` until the task finishes. It waits for the
task's ``pollIntervalMs`` between polls, or ``ClientOptions::task_poll_interval``
when the task names none, answers the task's ``inputRequests`` with the
registered elicitation, sampling and roots handlers (each question once), and
gives up with ``g_REQUEST_TIMEOUT`` after ``ClientOptions::task_timeout``. A
call that gives up, for the timeout or because a handler for an input request
failed, first sends a best-effort ``tasks/cancel`` for the task, since nothing
else holds it; ``TaskHandle::wait()`` leaves the task running when it gives up.
``call_tool()`` takes no per-request options, so its requests and its follow use
the ``ClientOptions`` defaults. A task that failed throws the ``McpError`` it failed with, and one that was cancelled
throws an ``McpError``. The result is checked against the tool's output schema as
for any call. A legacy session ignores the option.

To keep the handle instead, call ``Client::start_tool()``. It declares the
extension for that call whatever the option says, and returns the result when the
server ran the call in place, or a ``TaskHandle`` when it started a task:

.. code-block:: cpp

   auto started = co_await client.start_tool("build_report", arguments);
   if (auto* handle = std::get_if<mcp::TaskHandle>(&started)) {
       save_for_later(handle->id());

       mcp::DetailedTask task = co_await handle->get();   // tasks/get
       co_await handle->cancel();                         // tasks/cancel
       auto final_state = co_await handle->wait();        // polls to the end
       auto result = co_await handle->result();           // wait(), then the CallToolResult
   }

``TaskHandle::update()`` sends ``tasks/update`` with the answers to a task's
``inputRequests`` for an application that answers them itself. ``wait()`` and
``result()`` answer them with the registered handlers. A task id kept from
earlier gives a handle again through ``Client::task_handle(id)``, which a
restarted client can poll if the server still has the task.

Cancellation is a request: ``tasks/cancel`` is acknowledged, the handler sees
``Context::is_cancelled()``, and the server records the task as ``cancelled``
without waiting for the handler to return. A handler that is waiting for the
client's answer unwinds without changing the recorded status. A task whose time
to live passes is also reported to its handler through ``Context::is_cancelled()``.
``notifications/cancelled`` is not used for tasks.

Notifications about a task
--------------------------

A client can ask to be told when a task changes instead of polling.
``SubscriptionFilter::taskIds`` lists the tasks, and the stream then carries a
``notifications/tasks`` notification whose params are the task as ``tasks/get``
would return it (``mcp::DetailedTask``), whenever its status, status message or
requests change:

.. code-block:: cpp

   mcp::SubscriptionFilter filter;
   filter.taskIds = {handle->id()};
   mcp::Subscription subscription = co_await client.listen(filter);
   auto event = co_await subscription.next();   // method "notifications/tasks"

The server honors only the tasks the caller owns; ``Subscription::acknowledged()``
lists them. A request for ``taskIds`` without the extension declared is answered
with ``g_MISSING_REQUIRED_CLIENT_CAPABILITY``. Progress and log notifications are
not carried on the stream. Right after the acknowledgment the stream carries the
state each honored task has at that moment, and then its changes, so a task that
finishes while the stream opens is not missed. A client may still receive a state
it already knows. Tasks work
without streams, by polling, where the transport has none: ``HttpServerTransport``
answers ``subscriptions/listen`` with ``g_METHOD_NOT_FOUND``, see
:doc:`subscriptions`.

HTTP
----

``tasks/get``, ``tasks/update`` and ``tasks/cancel`` carry the task id in the
``Mcp-Name`` header, next to ``Mcp-Method``, so that a load balancer can send
every request for a task to the instance that holds it. The SDK's HTTP client
sets the header, and the HTTP server answers a request whose header is missing
or differs from ``params.taskId`` with ``g_HEADER_MISMATCH``. Task state lives in
the server process, not in a transport session, so a deployment with several
instances routes by that header, see :doc:`/guides/deployment`.

Wire summary
------------

=========================  =====================================================
Method or notification     Meaning
=========================  =====================================================
``tasks/get``              The task's state, with ``inputRequests``, ``result``
                           or ``error`` according to its status.
``tasks/update``           Answers to ``inputRequests``, by key. Acknowledged
                           with an empty result.
``tasks/cancel``           Asks the server to cancel. Acknowledged with an
                           empty result.
``notifications/tasks``    The task's state, on a ``subscriptions/listen``
                           stream that asked for its id.
=========================  =====================================================

``tasks/list`` and ``tasks/result`` of 2025-11-25 do not exist, and answering an
unknown task id, an expired task or another principal's task is a JSON-RPC
``g_INVALID_PARAMS`` error. A task request from a client that did not declare
the extension is answered with ``g_MISSING_REQUIRED_CLIENT_CAPABILITY``.

Other extensions
----------------

``mcp/protocol/extensions.hpp`` holds the helpers the tasks extension uses to
negotiate, and any extension can: ``declare_extension()`` adds an extension and
its settings to a ``ClientCapabilities`` or ``ServerCapabilities``,
``declares_extension()`` asks whether a peer declared one, and
``extension_settings()`` reads its settings. A handler passes
``ctx.request_meta()`` to read what the client of its request declared.
