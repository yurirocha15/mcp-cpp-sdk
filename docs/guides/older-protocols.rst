Older Protocol Revisions
========================

The SDK serves protocol revision 2026-07-28 and the revisions 2025-11-25,
2025-06-18, 2025-03-26 and 2024-11-05. A session of the older revisions starts
with an ``initialize`` request, so the SDK calls them the legacy era; 2026-07-28
is the modern era (see :doc:`/concepts/protocol`). This guide shows how to keep
speaking a legacy revision from a client, how to limit what a server serves,
and what differs in a legacy session.

By default nothing needs to change. A ``Server`` serves both eras on the same
transport, and a ``Client`` detects the era of the server it connects to.

Choosing a Protocol on the Client
---------------------------------

``ClientOptions::protocol`` takes a ``ClientProtocol``, built by one of three
factory functions:

``ClientProtocol::Auto()``
   The default. ``connect()`` sends ``server/discover``. A server that answers it
   (or answers with an error only a 2026-07-28 server sends) is spoken to in the
   modern era. Any other outcome sends ``initialize`` instead.

``ClientProtocol::Legacy()``
   ``connect()`` sends ``initialize`` naming 2025-11-25 and nothing else. It sends
   no probe.

``ClientProtocol::Version("...")``
   Exactly one version. "2025-11-25", "2025-06-18", "2025-03-26" and "2024-11-05"
   send ``initialize`` naming that version. "2026-07-28" speaks the modern
   protocol: ``connect()`` sends one ``server/discover`` request to fill in the
   ``ConnectResult`` and never falls back to ``initialize``. Any other string
   throws ``std::invalid_argument``.

.. code-block:: cpp

   mcp::ClientOptions options;
   options.protocol = mcp::ClientProtocol::Legacy();
   // or: options.protocol = mcp::ClientProtocol::Version("2025-06-18");

   mcp::Client client(transport, executor, options);

   {
       mcp::ConnectResult connected = co_await client.connect("my-client", "1.0.0");
       // connected.era is mcp::ProtocolEra::eLegacy
       // connected.protocolVersion is the version initialize negotiated
   }

Pin the protocol when you know the server's revision and want to skip the
probe. ``Auto()`` costs a
legacy server one extra round trip, or ``ClientOptions::probe_timeout`` (3
seconds by default) if the server ignores the request, and a server that closes
the connection on receiving it fails ``connect()``. ``Legacy()`` or
``Version(...)`` avoids all three.

The era is fixed once ``connect()`` returns. To find out why the client chose
it, set ``ClientOptions::on_era_decision``.

What Differs in a Legacy Session
--------------------------------

``connect()`` sends ``initialize`` and then ``notifications/initialized``. The
version and the capabilities the two sides negotiated belong to the session. In
particular:

* The capabilities you pass to ``connect()`` go into ``initialize``. Call
  ``set_roots()`` before ``connect()`` so that the roots capability is
  advertised.
* ``Client::ping()`` works.
* The server's log level is set with ``logging/setLevel``. ``RequestOptions::log_level``
  has no effect.
* ``resources/subscribe`` and ``resources/unsubscribe`` work, and the server's
  ``on_subscribe()`` and ``on_unsubscribe()`` callbacks run for them. The
  ``Client`` has no helper for them: send the request with ``send_request()``,
  as ``examples/features/notifications_subscriptions.cpp`` does.
* ``Client::listen()`` is not available and throws ``McpError`` with
  ``g_METHOD_NOT_FOUND``.
* Server requests to the client (elicitation, sampling, roots) are real
  requests that the server sends to the client during the handler.
  ``set_roots(roots, true)`` sends ``notifications/roots/list_changed``.
* Results carry no cache hints.
* The response of a request that the client cancelled with
  ``notifications/cancelled`` is still sent for 2025-11-25 and earlier.

Methods that exist only in legacy sessions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Protocol revision 2026-07-28 does not have these methods:

* ``initialize`` and ``notifications/initialized``
* ``ping``
* ``logging/setLevel``
* ``resources/subscribe`` and ``resources/unsubscribe``
* ``shutdown``

A server answers a 2026-07-28 request for any of them (other than
``initialize``, which starts a legacy session) with ``g_METHOD_NOT_FOUND``. In a
modern session the client does not send ``initialize``, ``ping``,
``logging/setLevel``, ``resources/subscribe`` or ``resources/unsubscribe`` at
all: the call throws ``McpError`` with ``g_METHOD_NOT_FOUND`` and a message that
names the protocol version.

Limiting the Protocols a Server Serves
--------------------------------------

``ServerProtocolPolicy`` and ``Server::set_protocol_policy()`` choose what a
server serves:

``serve_modern`` (default true)
   Serve requests that carry their protocol version in ``_meta``. When false,
   each of them is answered with ``g_UNSUPPORTED_PROTOCOL_VERSION``.

``serve_legacy`` (default true)
   Serve clients that start with ``initialize``. When false, ``initialize`` is
   answered with ``g_METHOD_NOT_FOUND`` and any other request without a
   protocol version in ``_meta`` with ``g_INVALID_PARAMS``.

``legacy_versions`` (default unset, which means all four)
   The versions ``initialize`` may negotiate, each one of
   ``g_LEGACY_PROTOCOL_VERSIONS``. An ``initialize`` that asks for another
   version negotiates the newest one in the list.

.. code-block:: cpp

   // Serve 2026-07-28 only.
   mcp::ServerProtocolPolicy modern_only;
   modern_only.serve_legacy = false;
   server.set_protocol_policy(modern_only);

   // Serve 2026-07-28 and the two newest legacy revisions.
   mcp::ServerProtocolPolicy recent;
   recent.legacy_versions = std::vector<std::string>{"2025-06-18", "2025-11-25"};
   server.set_protocol_policy(recent);

Set the policy before the server serves its first request.
``set_protocol_policy()`` throws ``std::logic_error`` afterwards, and
``std::invalid_argument`` for a policy that serves neither era, serves legacy
clients with an empty ``legacy_versions``, or names a version that is not a
legacy one (``ServerProtocolPolicy::validate()`` makes the same check).
``StreamableHttpSessionManager::set_protocol_policy()`` applies a policy to
every server the manager builds.

``served_protocol_versions(policy)`` lists the versions a policy serves, and
``negotiate_protocol_version(requested, policy)`` gives the version that
``initialize`` negotiates under it. ``g_LEGACY_PROTOCOL_VERSIONS``,
``g_MODERN_PROTOCOL_VERSIONS``, ``g_LATEST_LEGACY_PROTOCOL_VERSION``,
``is_legacy_protocol_version()`` and ``is_modern_protocol_version()`` name the
two families. ``initialize`` never selects 2026-07-28.

What a Legacy Client Gets From a Modern-Only Server
---------------------------------------------------

A server with ``serve_legacy = false`` answers ``initialize`` with
``g_METHOD_NOT_FOUND``. The error names the versions the server serves, in its
message and in ``data.supported``. Over HTTP the status is ``404``. For a client
pinned to ``Legacy()``, ``connect()`` fails with that message, so the client
learns which versions to ask for. ``HttpClientTransport`` reports the JSON-RPC
error the server sent with the ``404`` instead of only the HTTP status.

A legacy request other than ``initialize`` is answered with
``g_INVALID_PARAMS``. ``Server::protocol_rejection_stats()`` counts these
refusals in ``legacy_shaped``, separately from the requests refused for their
version (``unsupported_version``) or their ``_meta`` fields
(``invalid_metadata``). That count is how an operator sees legacy clients being
turned away.

A client left on ``Auto()`` does not run into this: it probes, finds the modern
server and speaks 2026-07-28.
