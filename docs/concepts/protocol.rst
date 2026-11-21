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
``ClientOptions::protocol`` defaults to ``ClientProtocol::automatic()``, which sends
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

A ``Client`` reuses these results when ``ClientOptions::response_cache.enabled``
is set. ``list_tools()``, ``list_prompts()``, ``list_resources()``,
``list_resource_templates()`` and ``read_resource()`` then keep each result whose
``ttlMs`` is positive, under the method and its parameters, and answer the same
call from it until ``ttlMs`` runs out or ``ResponseCacheOptions::max_ttl``
passes, whichever comes first. A ``list_changed`` notification drops the lists
it names, and ``notifications/resources/updated`` the reads of its URI. Set
``RequestOptions::cache_mode`` to ``CacheMode::eRefresh`` to ask the server and
keep the new answer, or to ``CacheMode::eBypass`` to ask it and leave the cache
alone. The cache serves one client only, so a ``private`` result stays with
the client that received it. Call ``Client::clear_response_cache()`` when its
credentials change to another user's.

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

* The ``requestState`` of stored state is a random token. It works once, for the same principal,
  method and parameters, and an unknown, expired or reused one is answered with
  ``g_INVALID_PARAMS`` ("Unknown or expired requestState").
* A question that needs a capability the client did not declare in the request
  is answered with ``g_MISSING_REQUIRED_CLIENT_CAPABILITY`` (-32021), whose
  ``data.requiredCapabilities`` names it. The ``ctx`` call throws.
* By default a waiting handler lives in the ``Server`` instance, and the retry
  must reach the same instance. Sealed request state removes that rule; see
  below.
* ``Server::set_mrtr_options()`` bounds waiting handlers: how long they wait (5
  minutes by default), and how many may wait (1024 in all and 64 per principal
  by default). ``Server::mrtr_stats()`` counts them. A handler that is given up
  throws from its question.
* Only ``tools/call``, ``resources/read`` and ``prompts/get`` can ask the client
  anything. In any other 2026-07-28 request these ``ctx`` calls throw
  ``std::runtime_error``.

Naming the questions
~~~~~~~~~~~~~~~~~~~~

The key a question travels under, in the result's ``inputRequests`` and in the
client's ``inputResponses``, is chosen by the server. By default the SDK names
the questions ``elicitation-1``, ``sampling-2``, ``roots-3`` and so on, counting
the questions it names. A handler can choose the key itself with the
``InputOptions`` argument of ``elicit()``, ``sample_llm()`` and
``request_roots()``:

.. code-block:: cpp

   // Named first: GCC 11 mishandles a braced temporary inside a co_await.
   const mcp::InputOptions options{.key = "user_name"};
   auto answer = co_await ctx.elicit(params, options);

The key works the same in stored and sealed mode; a retried sealed handler that
asks the same named question again gets the answer it was given earlier. A key
that is empty, that has the form of a generated name (``elicitation-``,
``sampling-`` or ``roots-`` followed by digits) or that another question of the
same request already used, in this round or an earlier one, makes the call
throw ``std::invalid_argument`` before anything is asked. A named key cannot be
reused within a request, so a handler that asks again must use a new key.
Requests of protocol revisions before 2026-07-28 send each question as a
request of its own and ignore the key.

Sealed request state
~~~~~~~~~~~~~~~~~~~~

With ``MrtrOptions::state_mode`` set to ``MrtrStateMode::eSealed``, nothing
waits in the server. The ``requestState`` carries the state, encrypted and
authenticated with AES-256-GCM under a key derived (HKDF-SHA256) from
``MrtrOptions::sealing_keys``, and the retry may reach any ``Server`` instance
that has the key:

.. code-block:: cpp

   mcp::MrtrOptions options;
   options.state_mode = mcp::MrtrStateMode::eSealed;
   options.sealing_keys = {current_secret, previous_secret};  // 32 bytes or more each
   server.set_mrtr_options(std::move(options));

The state holds a digest of the principal, an expiry, the method, a digest of
the request's parameters, the round count, the keys of the open questions and
the answers of the earlier rounds. A retry whose state does not open, is expired, or belongs to another principal,
method or parameters is answered with ``g_INVALID_PARAMS`` ("Unknown or expired
requestState"). Callers that are not authenticated can use sealed state; their
principal is empty.

On a retry the server runs the handler again from the start. The questions the
earlier rounds answered return their answers at once, and the handler goes on
until it reaches the question that is still open, or finishes. This has
consequences for handlers:

* A handler must ask the same questions in the same order when given the same
  answers.
* Code before the open question runs once per round. ``Context::is_replay()`` is
  true from the start of a run until the handler has taken the last answer of
  the earlier rounds or asks a question they do not answer, so
  ``if (!ctx.is_replay()) { ... }`` runs a side effect once. A handler that
  never asks again after the earlier rounds sees ``is_replay()`` true until it
  ends.
* A handler is released at its first unanswered question without running any more
  of its code: its locals are destroyed, and no ``catch`` of the handler sees the
  release. The question is carried in the ``requestState`` instead.
* Progress and log notifications the handler sends while replaying are sent
  again.

The other options of sealed state:

* The first key of ``sealing_keys`` seals; every key opens. To rotate, put the
  new key first and drop the old one after ``state_ttl`` has passed.
* ``state_ttl`` (5 minutes by default) is how long a state is accepted after the
  round that sealed it. ``max_rounds`` (16) bounds the rounds of one request,
  and ``max_state_bytes`` (32 KiB) the length of the ``requestState``. A request
  that outgrows either is answered with ``g_INTERNAL_ERROR``.
* **A sealed state is not single-use.** Until it expires it can be sent again,
  and each time the handler runs again from the start with the same answers, so
  the steps after its last question run once more. Make those steps idempotent,
  or set ``MrtrOptions::consume``, which is called with the unique id of each
  state before its retry is served; return false for an id that was used already
  to refuse it. The hook is optional.
* The expiry is a wall-clock time. Server instances that share keys need
  reasonably synchronized clocks.
* ``max_rounds`` must be at least 1 and ``max_state_bytes`` at least 512;
  ``set_mrtr_options()`` throws ``std::invalid_argument`` otherwise, and when two
  keys are the same secret or share a 4-byte key id.
* The limits on waiting handlers (``max_parked`` and the others) apply to stored
  state only, since sealed state keeps no handler waiting.

Extensions
----------

An optional extension is negotiated under the ``extensions`` member of the
capabilities, keyed by its identifier. A server lists the extensions it serves in
the capabilities it gives ``Server`` and reports them in the ``server/discover``
result. A client declares the extensions it understands in the capabilities of
each request, which a handler reads through ``Context::request_meta()``. Declaring
an extension does not ask for it: the server decides, request by request, whether
to use one the client declared, and uses none a client did not declare.

``mcp/protocol/extensions.hpp`` has the helpers for both sides:
``declare_extension()``, ``declares_extension()`` and ``extension_settings()``.
The one extension the SDK implements is :doc:`tasks`.

Subscriptions
-------------

A 2026-07-28 client opens a ``subscriptions/listen`` stream with
``Client::listen()`` to receive list-changed and resource-updated
notifications. ``resources/subscribe`` does not exist in this model. See
:doc:`subscriptions`.

Older sessions
--------------

See :doc:`/guides/older-protocols` to pin an older revision or to limit what a
server serves.

A client of 2025-11-25 or earlier starts a session with ``initialize``. The same
``Server`` serves it, by default, next to 2026-07-28 requests. A session of an
older revision keeps ``ping``, ``logging/setLevel``, ``resources/subscribe`` and
``resources/unsubscribe``. Its results are unchanged: they carry no cache
hints. A 2026-07-28 request is answered with ``g_METHOD_NOT_FOUND`` for any of
those methods.
