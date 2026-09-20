OAuth
=====

:cpp:class:`mcp::auth::OAuthAuthorizationManager` is the entry point for OAuth
in this SDK, and the only supported way to act on a ``WWW-Authenticate``
challenge. Applications remain responsible for browser interaction, redirect
handling, consent UX, secure persistent storage, and naming the origins their
deployment may contact.

Lower-level pieces are also exported — PKCE generation, metadata discovery,
authorization-code exchange, refresh, volatile token storage, and
transport-level bearer authentication — but they are **components of the
manager, not an alternative to it**. See
:ref:`oauth-low-level-building-blocks` for what using them directly costs you.

Challenge-driven authorization
------------------------------

A protected MCP server answers an unauthenticated request with ``401`` and a
``WWW-Authenticate`` header naming its protected-resource metadata (RFC 9728).
Hand that header to the manager and it runs the whole exchange:

.. note::

   The ``https://`` origins below are what a production deployment should
   use. This build has no TLS support of its own — see
   :ref:`oauth-security-boundary` before contacting anything but a loopback
   or internally-trusted ``http://`` target.

.. code-block:: cpp

   #include <mcp/auth/oauth.hpp>

   // A default-constructed policy refuses every origin, so name the ones this
   // client may fetch metadata and tokens from, before the first request.
   mcp::auth::MetadataFetchPolicy policy;
   policy.allowed_origins = {"https://mcp.example.com", "https://auth.example.com"};

   mcp::auth::OAuthAuthorizationConfig config;
   config.server_url = "https://mcp.example.com/mcp";  // also the token-store key
   config.client_id = "my-mcp-client";
   config.redirect_uri = "https://app.example.com/callback";
   config.policy = std::move(policy);

   auto token_store = std::make_shared<mcp::auth::InMemoryTokenStore>();

   // The consent step. The SDK never launches a browser and never binds a
   // listener for the redirect: carrying the user agent to the authorization
   // endpoint and collecting the response is the application's job.
   auto authorize = [](const mcp::auth::AuthorizationRequest& request)
       -> mcp::Task<mcp::auth::AuthorizationResponse> {
       auto redirect_url = co_await open_in_browser(request.authorization_url);
       co_return mcp::auth::parse_authorization_response(redirect_url);
   };

   auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
       executor, token_store, std::move(config), authorize);

   if (co_await manager->try_handle_challenge(www_authenticate)) {
       auto authed = std::make_shared<mcp::auth::OAuthClientTransport>(inner, manager);
       mcp::Client client(authed, executor);
       co_await client.connect("my-client", "1.0.0");
   }

``try_handle_challenge`` returns ``false`` when the header carried no ``Bearer``
challenge to act on, and throws when discovery is refused by the policy or the
authorization response is rejected.

.. _oauth-what-the-manager-validates:

What the manager validates
~~~~~~~~~~~~~~~~~~~~~~~~~~

Each of these is a control you would otherwise have to write, and get right,
yourself:

* **Metadata fetch policy.** Every discovery and token URL is checked against
  :cpp:struct:`mcp::auth::MetadataFetchPolicy` before host resolution, so a
  refused target is never contacted. A challenge-supplied ``resource_metadata``
  URL is attacker-influenced input.
* **Issuer binding.** The issuer is recorded from the authorization-server
  metadata document the SDK itself fetched and validated, bound byte-for-byte
  to the URL it came from. A redirect or a substituted document cannot move the
  flow to a different issuer.
* **Cryptographic state.** The ``state`` parameter is generated from a
  cryptographic random source, and a response whose ``state`` does not match the
  recorded value is rejected.
* **RFC 9207 issuer validation.** Runs *before* the response's ``error``,
  ``error_description`` and ``error_uri`` are read, so a response with a
  mismatched issuer cannot smuggle attacker-chosen text to your user.
* **S256 PKCE.** The challenge is sent to the authorization endpoint and the
  verifier is retained for the token request.
* **RFC 8707 resource indicator.** Carried into the code exchange, so the token
  you receive is bound to this MCP server rather than replayable at another.

Auditing an attempt
~~~~~~~~~~~~~~~~~~~

:cpp:func:`mcp::auth::OAuthAuthorizationManager::last_authorization_request`
returns the record the attempt was validated against — the state, the recorded
issuer, the resource indicator and the PKCE verifier — so an application can
audit the binding rather than take it on trust.
:cpp:func:`mcp::auth::OAuthAuthorizationManager::last_client_identity` reports
which path produced the client identity: an injected credential, a client ID
metadata document, or a dynamic registration.

A complete runnable flow, including a mock authorization server, is in
``examples/features/oauth_flow.cpp``.

.. _oauth-protecting-a-server:

Protecting a server
-------------------

The other half of the flow above is a server that tells an unauthorized client
where to get a token. Both HTTP server transports —
:cpp:class:`mcp::StreamableHttpSessionManager` and
:cpp:class:`mcp::HttpServerTransport` — take the same three settings, and one
call is enough to make the flow work end to end:

.. code-block:: cpp

   mcp::StreamableHttpSessionManager manager(executor, host, port, factory);
   manager.set_bearer_token_validator(
       [](std::string_view token) { return validate_token(token); });

   mcp::ProtectedResourceMetadataConfig metadata;
   metadata.resource = "https://mcp.example.com/mcp";
   metadata.authorization_servers = {"https://auth.example.com"};
   metadata.scopes_supported = {"mcp:read", "mcp:write"};
   manager.set_protected_resource_metadata(metadata);

:cpp:func:`set_protected_resource_metadata` does two things. It serves the
RFC 9728 document, answering GET requests at its path without an
``Authorization`` header so a client holding no token can read it. And it fills
the ``resource_metadata`` parameter of the ``WWW-Authenticate`` challenge with
that document's URL, which is the value ``try_handle_challenge`` needs to begin
discovery.

Both the path and the URL are derived from ``resource``, and from nothing else.
RFC 9728 §3.1 inserts the well-known segment between the authority and the
resource's own path, so the example above publishes at
``https://mcp.example.com/.well-known/oauth-protected-resource/mcp``. Only a
resource sitting at the origin root is described at the bare
``/.well-known/oauth-protected-resource``. Set ``metadata.path`` to override the
derivation; leave it empty to get it.

.. important::

   The advertised URL is never inferred from the address the transport is bound
   to. Behind a TLS terminator, a reverse proxy, or a container port mapping —
   which is to say in most deployments — the listener's own origin is not the one
   clients can reach, so a URL built from it would send clients somewhere they
   cannot fetch. ``resource`` is what the outside world calls this server, and it
   is required for exactly that reason.

To send other challenge parameters, or to point at metadata this server does not
host itself, set them explicitly:

.. code-block:: cpp

   mcp::BearerChallengeConfig challenge;
   challenge.realm = "mcp";
   challenge.scope = "mcp:read";
   challenge.error = "invalid_token";
   manager.set_bearer_challenge(challenge);

Parameters are sent in the order realm, error, scope, resource_metadata, each as
an RFC 7235 quoted-string. A ``resource_metadata`` set here is kept as written
and is not overwritten by the metadata document's URL. The two setters are
order-independent: each renders from the whole current configuration, so calling
them in either order produces the same header. A value that cannot appear in a
quoted-string is rejected by the setter with ``std::invalid_argument``, so it
cannot reach the wire as a header injection.
:cpp:func:`mcp::format_www_authenticate` renders the same string for a caller
that wants to serve the challenge from its own code.

Validating tokens
~~~~~~~~~~~~~~~~~

``set_bearer_token_validator`` takes a synchronous predicate, which suits a
locally-verifiable token such as a signed JWT whose key is already in memory.
When the decision needs I/O — token introspection, a JWKS fetch — use
``set_async_bearer_token_validator`` instead, so the wait suspends rather than
blocking the executor that is concurrently serving MCP traffic:

.. code-block:: cpp

   manager.set_async_bearer_token_validator(
       [](std::string token) -> mcp::Task<bool> {
           co_return co_await introspect(std::move(token));
       });

The token is passed by value because the validator may suspend, and a view into
the request buffer is not guaranteed to survive that. A transport accepts one
validator: installing both throws ``std::logic_error``. A server that installs
only the synchronous one pays nothing for the asynchronous path.

Health checks and other routes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Routes that must answer before a token exists are named individually:

.. code-block:: cpp

   manager.set_unauthenticated_paths({"/health"});

Each entry is matched for equality against the path component of the request
target, with any query string or fragment removed first, so ``/health`` also
exempts ``/health?probe=1``.

.. warning::

   An exempt path is excused from the bearer check **and** excluded from MCP
   dispatch. MCP is otherwise served on every path the custom handler declines,
   so exempting a path without also excluding it would serve MCP there with no
   authentication at all — and naming the path MCP runs on would silently
   disable authentication for the whole server. An exempt request that the
   metadata route and the custom request handler both decline is therefore
   answered ``404 Not Found``, which makes that misconfiguration fail loudly
   instead of quietly.

The request pipeline runs the protected-resource metadata route first, then the
unauthenticated path list, then the bearer check, then the custom request
handler on ``StreamableHttpSessionManager``, then MCP dispatch. Every one of
these settings is opt-in: a server that sets none of them behaves exactly as
before, down to the bare ``Bearer`` challenge on a 401. All of them must be
configured before ``listen()`` starts, and a setter called afterwards throws
``std::logic_error``.

Request body size
~~~~~~~~~~~~~~~~~

Both transports cap the HTTP request body they will read, answering
``413 Payload Too Large`` and closing the connection beyond it. The default is
:cpp:var:`mcp::constants::g_default_max_request_body_bytes`, 8 MiB, and
``set_max_request_body_bytes`` overrides it.

The default is deliberately well above Boost.Beast's own 1 MB. Binary content
reaches an MCP server as base64 inside the JSON body — ``ImageContent::data``,
``BlobResourceContents::blob`` and ``AudioContent`` have no out-of-band or
streaming path — and base64 inflates by 4/3, so a 1 MB cap admits only about
750 KB of raw bytes, which ordinary phone photos and screenshots already exceed.

.. warning::

   Lower this only deliberately. A rejection happens in the HTTP parser, before
   the request becomes an MCP message, so it is reported as a ``413`` on the
   transport and not through the MCP error channel. A cap set near the size of
   the raw content presents to users as large tool calls failing for no visible
   reason.

.. _oauth-security-boundary:

Security boundary
-----------------

``OAuthHttpClient`` and the built-in MCP HTTP transports currently accept only
plain ``http://`` URLs. Use them directly only for loopback development or a
trusted internal boundary. Production traffic outside that boundary must use a
TLS-terminating proxy or a custom TLS transport. Never log access tokens,
refresh tokens, authorization codes, client secrets, or PKCE verifiers.

.. _oauth-low-level-building-blocks:

Low-level building blocks
-------------------------

.. warning::

   The pieces below perform **no** issuer binding, **no** ``state`` check and
   **no** RFC 9207 validation. Composing them into your own authorization flow
   compiles and appears to work, and silently gives up every control listed
   under :ref:`what the manager validates <oauth-what-the-manager-validates>`.
   Use them to inspect metadata or to drive a token exchange whose issuer you
   have already established by other means. To act on a ``WWW-Authenticate``
   challenge, use :cpp:class:`mcp::auth::OAuthAuthorizationManager`.

Configuration
~~~~~~~~~~~~~

``OAuthConfig`` describes the endpoints and client information used for an
explicit token exchange or refresh. It is unrelated to
``OAuthAuthorizationConfig``, which configures the manager:

.. code-block:: cpp

   #include <mcp/auth/oauth.hpp>

   mcp::auth::OAuthConfig config;
   config.client_id = "my-mcp-client";
   config.token_endpoint = "http://127.0.0.1:9000/token";
   config.authorization_endpoint = "http://127.0.0.1:9000/authorize";
   config.redirect_uri = "http://127.0.0.1:8080/callback";
   config.scope = "mcp:read";

Discovery and PKCE
~~~~~~~~~~~~~~~~~~

``OAuthDiscoveryClient`` reads protected-resource and authorization-server
metadata. Discovery results are cached for a configurable interval. Install a
:cpp:struct:`mcp::auth::MetadataFetchPolicy` on the ``OAuthHttpClient`` first,
or every fetch is refused.

.. code-block:: cpp

   auto oauth_http = std::make_shared<mcp::auth::OAuthHttpClient>(executor);
   mcp::auth::OAuthDiscoveryClient discovery(oauth_http);

   auto resource = co_await discovery.discover_protected_resource(server_url);
   auto authorization_server =
       co_await discovery.discover_auth_server(resource.authorization_servers.front());
   auto pkce = mcp::auth::generate_pkce_pair();

Taken this far by hand, the application must then build and open the
authorization URL, receive the callback, generate and check its own ``state``,
apply RFC 9207 issuer validation itself, and call ``exchange_code`` with the
original PKCE verifier. The manager does all of that, correctly, and is the
reason this route is not recommended.

Token storage and refresh
~~~~~~~~~~~~~~~~~~~~~~~~~

``InMemoryTokenStore`` is thread-safe but volatile. Production applications
should implement ``TokenStore`` with encryption and operating-system-appropriate
secret storage.

``OAuthAuthenticator`` reads the current access token and can explicitly
refresh it, for an application that already holds a token it obtained some
other way. ``OAuthAuthorizationManager`` implements the same
:cpp:class:`mcp::auth::Authenticator` interface, so either can be handed to
``OAuthClientTransport``; only the manager can acquire a token in the first
place. Neither runs a background refresh task. ``OAuthClientTransport``
attempts one refresh-and-retry after an HTTP 401 from ``HttpClientTransport``;
for non-HTTP transports it also understands the legacy JSON-RPC
``g_UNAUTHORIZED`` path. Because a request can be replayed, callers should
avoid wrapping non-idempotent operations unless their server provides its own
deduplication semantics.

HTTP authentication
~~~~~~~~~~~~~~~~~~~

When ``OAuthClientTransport`` wraps ``HttpClientTransport``, the access token
is sent as ``Authorization: Bearer <token>``. It is not copied into MCP request
metadata. Configure the matching server-side validator before starting the
listener:

.. code-block:: cpp

   mcp::StreamableHttpSessionManager manager(executor, host, port, factory);
   manager.set_bearer_token_validator(
       [](std::string_view token) { return validate_token(token); });

``HttpServerTransport`` exposes the same validator. The validator decides
whether a token is acceptable; what an unauthorized client is told to do next is
the challenge, covered under :ref:`oauth-protecting-a-server`. The older
``make_auth_middleware`` helper checks ``_meta.auth_token`` and exists for
non-HTTP or legacy integrations; it should not replace authentication at the
HTTP boundary.

Example and current limits
--------------------------

``examples/features/oauth_flow.cpp`` is a loopback demonstration using a mock
authorization server and ``MemoryTransport`` for MCP messages. It drives
``OAuthAuthorizationManager`` from a ``WWW-Authenticate`` challenge through
discovery, consent, RFC 9207 validation and a resource-indicated code exchange,
then performs one refresh, without exposing secret values. Its consent callback
mints the code directly from the mock authorization server, which is possible
only because the example owns both ends; the file says so at that point and
should not be copied there.

Its MCP-side authentication uses the legacy ``_meta.auth_token`` middleware
path, so it does not demonstrate the HTTP ``Authorization: Bearer`` boundary
described above.

Both ends of the flow are in the SDK. A server configured as
:ref:`oauth-protecting-a-server` describes answers an unauthenticated request
with a challenge naming its protected-resource metadata, serves that metadata
without a token, and ``OAuthAuthorizationManager`` drives the rest from that
header. The server side is deliberately narrow: it advertises a challenge and
publishes a metadata document, and it neither issues tokens nor validates them
beyond calling the ``BearerTokenValidator`` the application supplies, so an
authorization server and the token verification behind that callback remain the
deployment's own.

The official client conformance baseline records the OAuth scenarios that are
not yet implemented. In particular, the SDK does not currently orchestrate all
protected-resource metadata variants, CIMD/pre-registration, scope escalation,
or token-endpoint authentication modes as a complete end-user flow.

Cross-references
----------------

* See :doc:`transports` for HTTP transport security and Origin validation.
* See :doc:`context` for request context and the legacy metadata path.
