Growing a Server
================

The server of :doc:`/getting-started`, ``examples/sync/server_base.cpp``, grows
one feature at a time. Each section below is one step: a compiled program in
``examples/sync`` that is the base program plus that feature, shown as a diff
against the base. A step adds lines and changes only the lines it names, so the
steps combine: ``server_all.cpp`` holds every one of them that does not replace
the same line in another way. CI checks this for every step
(``scripts/check_example_progression.py``) and runs each program.

The base program is in sync mode (see :doc:`/getting-started`). A step that
needs a coroutine moves only that handler to async mode, and the other handlers
stay synchronous; "Run the Event Loop Yourself" moves the event loop into the
program.

The base program, without its opening comment:

.. literalinclude:: ../../examples/sync/server_base.cpp
   :language: cpp
   :start-at: #include

Add a Resource
--------------

``add_text_resource()`` registers a resource whose contents are the text a
function returns, read again on each ``resources/read``. Its MIME type is
``text/plain`` unless a fourth argument names another. A ``Resource``
definition can be passed instead of the URI and name, to set its title,
description, annotations, icons or size. See :doc:`/concepts/resources` for
binary contents, resource templates and subscriptions.

.. literalinclude:: /_generated/progression/server_add_resource.diff
   :language: diff

Add a Prompt
------------

``add_text_prompt()`` registers a prompt whose one message, from the user, is
the text a function makes of the request's arguments. The function receives
the arguments as a ``Server::PromptArguments`` map, empty when the request gave
none. A request that leaves out an argument marked as required is answered with
invalid params, and the function does not run. See :doc:`/concepts/prompts` for
prompts with several messages or other content.

.. literalinclude:: /_generated/progression/server_add_prompt.diff
   :language: diff

Complete Prompt Arguments
-------------------------

``set_completion_provider()`` suggests values for an argument as a user types
it. The provider is a coroutine, so the program includes
``<mcp/core/async.hpp>``, which defines ``mcp::Task``; the other handlers stay
synchronous. This step builds on the prompt above. See
:doc:`/concepts/completions`.

.. literalinclude:: /_generated/progression/server_add_completions.diff
   :language: diff

Serve over HTTP
---------------

``run_http(host, port)`` serves Streamable HTTP at ``http://host:port/mcp``
instead of stdio. The only line that changes is the run line; this program
also reads the port from its command line. Each client of protocol revision
2025-11-25 or earlier gets a session of its own with the same tools, and
``run_http()`` returns on SIGINT or SIGTERM, as ``run_stdio()`` does. Without
the settings of the next step, a request that carries an ``Origin`` header is
refused, so serve it this way only on a trusted boundary such as loopback. See
:doc:`/concepts/transports` and :doc:`deployment`.

.. literalinclude:: /_generated/progression/server_add_http.diff
   :language: diff

HTTP Settings and Authentication
--------------------------------

``run_http()`` takes a third argument, a function that receives the
``StreamableHttpSessionManager`` the server listens through before it listens.
Its setters configure the endpoint: here a bearer token validator, so that only
requests with the right token are served (the others get HTTP 401), and the
origins whose browser pages may call the server. Protected resource metadata,
scope requirements, limits and stream options are set the same way. See
:doc:`oauth-security` and :doc:`/concepts/oauth`.

.. literalinclude:: /_generated/progression/server_add_auth.diff
   :language: diff

Log and Report Progress from a Handler
--------------------------------------

A handler that sends something to the client while it runs takes the request's
``mcp::Context`` and becomes a coroutine: it returns ``mcp::Task<nlohmann::json>``
and awaits each message with ``co_await``. Only that handler changes; the
``add_tool()`` call keeps its form, and the other handlers stay synchronous.
The next three steps ask the client for something in the same way. See
:doc:`/concepts/context`.

.. literalinclude:: /_generated/progression/server_add_context.diff
   :language: diff

A handler that only needs to know whether its request was cancelled can stay
synchronous and take the ``Context`` as its first argument:
``[](mcp::Context& ctx, const nlohmann::json& args) -> nlohmann::json``, which
calls ``ctx.is_cancelled()``.

Ask the User a Question
-----------------------

``ctx.elicit()`` asks the user, through the client, to fill in a form whose
fields a JSON schema describes; ``tool_params()`` makes that schema as well. The
answer's ``action`` says whether the user accepted, declined or cancelled, and
an accepted answer's ``content`` holds the values, checked against the schema.
The client needs an elicitation handler, as :doc:`growing-a-client` adds.
Without one, a call of protocol revision 2026-07-28 is refused as needing a
capability the client did not declare, and in a session of an earlier revision
``elicit()`` throws, which ends the call with an error result. The same holds
for the next two steps. See :doc:`/concepts/elicitation`.

.. literalinclude:: /_generated/progression/server_add_elicitation.diff
   :language: diff

Ask the Client's Model
----------------------

``ctx.sample_llm()`` asks the client to have its language model answer a list
of messages, in at most ``maxTokens`` tokens. The reply's ``content`` is one
content block or a list of them; this tool expects one text block. The client
needs a sampling handler. See :doc:`/concepts/context`.

.. literalinclude:: /_generated/progression/server_add_sampling.diff
   :language: diff

Ask for the Client's Roots
--------------------------

``ctx.request_roots()`` asks the client for its roots, the directories or other
URIs it lets the server work in. The client needs a roots handler. Protocol
revision 2026-07-28 deprecates roots, which clients of earlier revisions still
use. See :doc:`/concepts/roots`.

.. literalinclude:: /_generated/progression/server_add_roots.diff
   :language: diff

Tell Subscribers That a Resource Changed
----------------------------------------

A client may subscribe to a resource, and is then told when it changes:
``notify_resource_updated(uri)`` sends ``notifications/resources/updated`` to
the clients subscribed to that URI. The tool that changes the resource here is a
coroutine because it awaits that notification. The server offers subscriptions
only when ``capabilities().resources->subscribe`` is true: registering a
resource declares the resources capability, but leaves that flag to the
program. The mutex keeps the name safe once tool handlers run on a pool of
their own, as in the next step. See :doc:`/concepts/subscriptions`.

.. literalinclude:: /_generated/progression/server_add_subscriptions.diff
   :language: diff

Run Handlers on a Thread Pool
-----------------------------

A synchronous handler runs on the server's event loop, so a slow one holds up
the others. ``start(4)`` on an ``mcp::Runtime`` starts four threads that wait for work,
and ``set_handler_executor()`` runs the tool handlers on them, while the server
reads and answers messages on its own loop. Tool handlers that share state then
synchronize it themselves.

.. literalinclude:: /_generated/progression/server_add_handler_pool.diff
   :language: diff

Let a Tool Run as a Task
------------------------

``enable_tasks()`` turns on the tasks extension of protocol revision
2026-07-28, and ``set_tool_task_mode()`` lets a tool's calls become tasks. With
``ToolTaskMode::eAuto`` a client that declared the extension, as the client's
``start_tool()`` does for its call, gets a task handle at once and fetches the
result when the tool has finished; any other client waits for the result as
usual. A task's handler runs on the handler executor, so this step starts the
pool of the step above as well: without one, the server writes a warning to
standard error, since a handler that blocks would stall the server's own loop.
Clients of earlier revisions never see tasks. See :doc:`/concepts/tasks`.

.. literalinclude:: /_generated/progression/server_add_tasks.diff
   :language: diff

Wrap Handlers in Middleware
---------------------------

``use()`` registers middleware: a coroutine that each request answered by an
application handler passes through (tool calls, resource reads, prompt
requests, resource subscriptions and completions), with the ``next`` handler to
call. It may look at or change the request and the result, or answer without
calling ``next``. Requests the server answers itself, such as the list methods
and ping, do not pass through it. See :doc:`/concepts/middleware`.

.. literalinclude:: /_generated/progression/server_add_middleware.diff
   :language: diff

List in Pages
-------------

``set_page_size()`` makes ``tools/list``, ``resources/list``,
``resources/templates/list`` and ``prompts/list`` answer with at most that many
items, and a ``nextCursor`` when more follow, which the client passes back for
the next page. A server with a single tool lists it as before. See
:doc:`/concepts/pagination`.

.. literalinclude:: /_generated/progression/server_add_pagination.diff
   :language: diff

Show an MCP Apps View
---------------------

The MCP Apps extension lets a host show an HTML view next to a tool's result.
``apps::add_view_resource()`` registers the page under a ``ui://`` URI, and
``apps::add_view_tool()`` registers a tool linked to it; a host without the
extension shows the tool's ordinary result. The program includes
``<mcp/server/apps.hpp>``. See :doc:`/concepts/apps`.

.. literalinclude:: /_generated/progression/server_add_apps.diff
   :language: diff

Serve an Agent Skill
--------------------

``skills::SkillsExtension`` serves Agent Skills: a ``SKILL.md`` file of
instructions, and any files that go with it, that a host can load for its
model. ``add_skill()`` adds one. The extension's methods are served to clients
of protocol revision 2026-07-28 only. The program includes
``<mcp/server/skills.hpp>``. See :doc:`/concepts/skills`.

.. literalinclude:: /_generated/progression/server_add_skills.diff
   :language: diff

Run the Event Loop Yourself
---------------------------

A program that runs other work beside the server, or serves other transports,
runs the event loop itself, as async mode allows: ``Server::run()`` is a coroutine that serves one
transport, started on an ``mcp::Runtime`` with ``spawn()``. Only the run line
changes. ``run_stdio()`` also handled SIGINT and SIGTERM, which such a program
now handles itself, for instance with ``mcp::graceful_shutdown()`` as
:ref:`the deployment guide <signal-handling>` shows. A program that already has a Boost.Asio ``io_context``
passes ``io.get_executor()`` where this one passes ``runtime.executor()``.

.. literalinclude:: /_generated/progression/server_add_own_loop.diff
   :language: diff

In a program that already runs a Boost.Asio ``io_context``, the server is one
more coroutine on it:

.. code-block:: cpp

   #include <mcp/core/async.hpp>
   #include <mcp/mcp.hpp>
   #include <mcp/transport/stdio.hpp>

   #include <boost/asio/co_spawn.hpp>
   #include <boost/asio/detached.hpp>
   #include <boost/asio/io_context.hpp>

   #include <memory>
   #include <string>

   int main() {
       mcp::Server server("greeter", "1.0.0");

       server.add_tool("greet", "Greets someone by name",
                       mcp::tool_params({{"name", "string", "Who to greet"}}),
                       [](const nlohmann::json& args) -> nlohmann::json {
                           return "Hello, " + args.at("name").get<std::string>() + "!";
                       });

       boost::asio::io_context io;
       auto transport = std::make_shared<mcp::StdioTransport>(io.get_executor());
       boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
       io.run();
   }

Every Step Together
-------------------

``server_all.cpp`` combines every step except the two that replace the run line
in another way, plain HTTP and running the event loop yourself: the resource
and the one that can be subscribed to, the prompt and its completion, the
handler that logs and reports progress, the questions to the user, the model and
the roots, the task, the middleware, the pages, the view, the skill, the handler
pool, and HTTP with its settings.

.. literalinclude:: /_generated/progression/server_all.diff
   :language: diff

Stopping, Ports and Notifications
---------------------------------

A few calls on the same ``Server`` control it while it runs, from any thread:

* ``server.stop()`` ends ``run_stdio()`` and ``run_http()`` as SIGINT does, and
  the task of ``run()`` by closing its transport. A handler may call it, but the
  requests still running when the server stops, that handler's own among them,
  may end without an answer.
* ``server.bound_port()`` returns the port ``run_http()`` listens on, which is
  the one the system chose when it was given port 0.
* ``server.sync()`` returns a view whose ``notify_tools_list_changed()``,
  ``notify_resources_list_changed()``, ``notify_prompts_list_changed()`` and
  ``notify_resource_updated(uri)`` block until the notification has been sent
  from the event loop of ``run_stdio()`` or ``run_http()``. They throw
  ``std::logic_error`` when called on that loop's thread, where synchronous
  handlers run (tool handlers excepted when a handler executor is set); there,
  and in coroutines, the ``notify_`` coroutines of the ``Server`` itself are
  awaited instead.
