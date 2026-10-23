Protocol Revisions
==================

The SDK serves protocol revision 2026-07-28, the newest one, and the older
revisions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05 on the same
``Server`` and the same transports. This page describes the 2026-07-28 model,
which is what a default ``Client`` and a default ``Server`` speak with each
other.

The model in short
------------------

A 2026-07-28 session has no handshake. The client does not send ``initialize``
or ``notifications/initialized``, and the server keeps no per-connection
protocol state. Instead:

* Every request carries the protocol fields it needs in its ``_meta`` object.
* A client learns what a server offers with the ``server/discover`` request.
* A result says what kind of result it is, and says how long a client may reuse
  it.
* A server that needs input from the user while it handles a request answers
  with an ``input_required`` result, and the client retries the request.
* A client that wants notifications opens a ``subscriptions/listen`` stream.

Per-request fields
------------------

A 2026-07-28 request carries these keys in ``params._meta``:

* ``io.modelcontextprotocol/protocolVersion``: the version of the request. It is
  required.
* ``io.modelcontextprotocol/clientCapabilities``: the client capabilities that
  apply to the request. It is required.
* ``io.modelcontextprotocol/clientInfo``: the client name and version. It is
  optional.
* ``io.modelcontextprotocol/logLevel``: the least severe level of log message
  the server should send for this request. It is optional.

``mcp::Client`` adds these keys itself, and merges them with any ``_meta``
entries you supply. ``clientCapabilities`` holds the capabilities you pass to
``connect()`` plus those that the handlers you registered imply, such as
``roots`` when a ``roots/list`` handler exists. To set the log level of a
request, set ``RequestOptions::log_level``.

On the server, a handler reads the parsed fields with
``Context::request_meta()``. It returns a ``RequestMetadata`` (declared in
``mcp/protocol/request_meta.hpp``, with the ``g_META_*`` key names) for a
2026-07-28 request, and a null pointer for a request of an older revision.

.. code-block:: cpp

   server.add_tool<nlohmann::json, nlohmann::json>(
       "who_is_asking", "Names the client", schema,
       [](mcp::Context& ctx, nlohmann::json) -> mcp::Task<nlohmann::json> {
           std::string who = "unknown";
           if (const auto* meta = ctx.request_meta()) {
               if (meta->clientInfo) {
                   who = meta->clientInfo->name;
               }
           }
           co_return nlohmann::json{{"client", who}};
       });

A request whose version the server does not serve is answered with
``g_UNSUPPORTED_PROTOCOL_VERSION`` (-32022). Its ``data`` is an
``UnsupportedProtocolVersionData`` that lists the versions the server serves. A
request with a missing or malformed field is answered with
``g_INVALID_PARAMS``. See :doc:`/guides/error-handling`.

Logging has no session level in this model. A handler's ``ctx.log()`` sends a
message only if the request carried a ``logLevel`` and the message is at least
that severe.

Discovery
---------

``server/discover`` replaces the information ``initialize`` returned. Its
``DiscoverResult`` holds ``supportedVersions``, the server ``capabilities``,
and optionally the server's ``serverInfo`` and ``instructions``. The request
itself must carry the 2026-07-28 ``_meta`` fields; without a protocol version
the server answers it with ``g_INVALID_PARAMS``.

``Client::connect()`` sends ``server/discover`` once and returns a
``ConnectResult``:

.. code-block:: cpp

   mcp::ConnectResult connected = co_await client.connect("my-client", "1.0.0");
   if (connected.era == mcp::ProtocolEra::eModern) {
       // connected.protocolVersion is "2026-07-28"
   }
   if (connected.serverInfo) {
       std::cout << connected.serverInfo->name << "\n";
   }

``ConnectResult`` has ``era``, ``protocolVersion``, ``capabilities``,
``serverInfo`` (optional), ``instructions`` (optional) and
``supportedVersions``. The era is fixed for the lifetime of the client.

By default the client does not know which era a server speaks.
``ClientOptions::protocol`` defaults to ``ClientProtocol::Auto()``, which sends
``server/discover`` first. A ``DiscoverResult``, or an error that only a
2026-07-28 server sends, means the server is modern. Any other outcome means it
is an older server, and the client sends ``initialize``. ``ConnectResult::era``
tells you which one happened; ``ClientOptions::on_era_decision`` reports each
decision and its reason.

Results
-------

The result of a 2026-07-28 request carries ``resultType``. The server sends
``"complete"`` for a finished result. ``Client`` fails a request with
``McpError`` when ``resultType`` is neither absent nor ``"complete"``. A
``tools/call``, ``prompts/get`` or ``resources/read`` may be answered with an
``input_required`` result, which the client resumes as described below.

By default a result also carries the server's name and version in ``_meta``
under ``io.modelcontextprotocol/serverInfo``. ``Server::set_report_server_info(false)``
leaves it out.

Cache hints
~~~~~~~~~~~

The results of ``server/discover``, ``tools/list``, ``prompts/list``,
``resources/list``, ``resources/templates/list`` and ``resources/read`` carry
two hints for caching: ``ttlMs``, how long in milliseconds a client may treat
the result as fresh, and ``cacheScope``, either ``public`` (any client may share
the cached result) or ``private`` (only the same authorization context may).
Set them per kind of result with ``Server::set_cache_hints()``:

.. code-block:: cpp

   server.set_cache_hints(mcp::CacheTarget::eToolsList,
                          mcp::CacheHint{.ttlMs = 60'000, .scope = mcp::CacheScope::ePublic});

Without a call, a target uses the defaults of ``CacheHint``: ``ttlMs`` of ``0``,
which tells a client not to reuse the result, and a ``private`` scope. A result
that already carries ``ttlMs`` or ``cacheScope`` from the handler keeps its own
value. Call ``set_cache_hints()`` before the server serves its first request.
It does not change results of older revisions, which carry no hints.

Requests that need input
------------------------

A tool handler can ask the user a question while it runs, with
``Context::elicit()``, or ask the client to run an LLM or list its roots, with
``Context::sample_llm()`` and ``Context::request_roots()``. In a 2026-07-28
session the server cannot send a request to the client on its own. This is how
the same calls work there:

1. The handler of a ``tools/call``, ``prompts/get`` or ``resources/read`` calls
   ``ctx.elicit()``. The handler waits.
2. The server answers the request with an ``input_required`` result. It holds
   the question under a key the server picked, and a ``requestState`` token.
3. The client runs the handler it registered for that kind of question:
   ``on_elicitation()``, ``set_roots()`` or ``on_roots_list()``, or an
   ``on_request("sampling/createMessage", ...)`` handler.
4. The client sends the request again, with a new id, the same method and
   parameters, the ``requestState`` and the answers in ``inputResponses``.
5. The waiting handler resumes with the answer, and its result is the answer to
   the retry.

Your handler code does not change: it still writes ``co_await ctx.elicit(...)``,
and the client code still registers ``on_elicitation()``.
``RequestOptions::max_input_rounds`` (8 by default) limits how many times the
client retries one call. The timeout of the call covers all rounds. A question
for which the client has no handler fails the call with an error that names the
method.

Some details to know:

* The ``requestState`` is a random token. It works once, for the same principal,
  method and parameters, and an unknown, expired or reused one is answered with
  ``g_INVALID_PARAMS`` ("Unknown or expired requestState").
* A question that needs a capability the client did not declare in the request
  is answered with ``g_MISSING_REQUIRED_CLIENT_CAPABILITY`` (-32021), whose
  ``data.requiredCapabilities`` names it. The ``ctx`` call throws.
* A waiting handler lives in the ``Server`` instance. The retry must reach the
  same instance.
* ``Server::set_mrtr_options()`` bounds waiting handlers: how long they wait (5
  minutes by default), and how many may wait (1024 in all and 64 per principal
  by default). ``Server::mrtr_stats()`` counts them. A handler that is given up
  throws from its question.
* Any other 2026-07-28 request, such as ``tools/list``, cannot ask the client
  anything, and these ``ctx`` calls throw ``std::runtime_error``.

Subscriptions
-------------

A 2026-07-28 client opens a ``subscriptions/listen`` stream with
``Client::listen()`` to receive list-changed and resource-updated
notifications. ``resources/subscribe`` does not exist in this model. See
:doc:`subscriptions`.

Older sessions
--------------

A client of 2025-11-25 or earlier starts a session with ``initialize``. The same
``Server`` serves it, by default, next to 2026-07-28 requests. A session of an
older revision keeps ``ping``, ``logging/setLevel``, ``resources/subscribe`` and
``resources/unsubscribe``. Its results are unchanged: they carry no cache
hints. A 2026-07-28 request is answered with ``g_METHOD_NOT_FOUND`` for any of
those methods.
