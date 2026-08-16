OAuth helpers
=============

The SDK currently provides experimental OAuth building blocks rather than a
complete, policy-free OAuth 2.1 client. Applications remain responsible for
browser interaction, redirect handling, client registration, consent UX,
secure persistent storage, and selecting the authorization-server policy that
fits their deployment.

The available pieces cover PKCE generation, protected-resource and
authorization-server metadata discovery, authorization-code exchange, refresh
tokens, volatile token storage, and transport-level bearer authentication.

Security boundary
-----------------

``OAuthHttpClient`` and the built-in MCP HTTP transports currently accept only
plain ``http://`` URLs. Use them directly only for loopback development or a
trusted internal boundary. Production traffic outside that boundary must use a
TLS-terminating proxy or a custom TLS transport. Never log access tokens,
refresh tokens, authorization codes, client secrets, or PKCE verifiers.

Configuration
-------------

``OAuthConfig`` describes the endpoints and client information used for an
explicit token exchange or refresh:

.. code-block:: cpp

   #include <mcp/auth/oauth.hpp>

   mcp::auth::OAuthConfig config;
   config.client_id = "my-mcp-client";
   config.token_endpoint = "http://127.0.0.1:9000/token";
   config.authorization_endpoint = "http://127.0.0.1:9000/authorize";
   config.redirect_uri = "http://127.0.0.1:8080/callback";
   config.scope = "mcp:read";

Discovery and PKCE
------------------

``OAuthDiscoveryClient`` reads protected-resource and authorization-server
metadata. Discovery results are cached for a configurable interval.

.. code-block:: cpp

   auto oauth_http = std::make_shared<mcp::auth::OAuthHttpClient>(executor);
   mcp::auth::OAuthDiscoveryClient discovery(oauth_http);

   auto resource = co_await discovery.discover_protected_resource(server_url);
   auto authorization_server =
       co_await discovery.discover_auth_server(resource.authorization_servers.front());
   auto pkce = mcp::auth::generate_pkce_pair();

The application builds and opens the authorization URL, receives the callback,
validates its own state value, and then calls ``exchange_code`` with the code
and the original PKCE verifier.

Token storage and refresh
-------------------------

``InMemoryTokenStore`` is thread-safe but volatile. Production applications
should implement ``TokenStore`` with encryption and operating-system-appropriate
secret storage.

``OAuthAuthenticator`` reads the current access token and can explicitly
refresh it. It does not run a background refresh task. ``OAuthClientTransport``
attempts one refresh-and-retry after an HTTP 401 from ``HttpClientTransport``;
for non-HTTP transports it also understands the legacy JSON-RPC
``g_UNAUTHORIZED`` path. Because a request can be replayed, callers should
avoid wrapping non-idempotent operations unless their server provides its own
deduplication semantics.

HTTP authentication
-------------------

When ``OAuthClientTransport`` wraps ``HttpClientTransport``, the access token
is sent as ``Authorization: Bearer <token>``. It is not copied into MCP request
metadata. Configure the matching server-side validator before starting the
listener:

.. code-block:: cpp

   mcp::StreamableHttpSessionManager manager(executor, host, port, factory);
   manager.set_bearer_token_validator(
       [](std::string_view token) { return validate_token(token); });

``HttpServerTransport`` exposes the same validator. The older
``make_auth_middleware`` helper checks ``_meta.auth_token`` and exists for
non-HTTP or legacy integrations; it should not replace authentication at the
HTTP boundary.

Example and current limits
--------------------------

``examples/features/oauth_flow.cpp`` is a loopback demonstration using a mock
authorization server and ``MemoryTransport`` for MCP messages. Its MCP-side
authentication uses the legacy ``_meta.auth_token`` middleware path; it does
not demonstrate the HTTP ``Authorization: Bearer`` boundary described above.
The example exercises discovery, PKCE, code exchange, legacy token injection,
and one refresh without exposing secret values.

The official client conformance baseline records the OAuth scenarios that are
not yet implemented. In particular, the SDK does not currently orchestrate all
protected-resource metadata variants, CIMD/pre-registration, scope escalation,
or token-endpoint authentication modes as a complete end-user flow.

Cross-references
----------------

* See :doc:`transports` for HTTP transport security and Origin validation.
* See :doc:`context` for request context and the legacy metadata path.
