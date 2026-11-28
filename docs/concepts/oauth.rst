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

.. code-block:: cpp

   #include <mcp/core/async.hpp>
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

In sync mode a client constructed from an ``Endpoint`` gets the same transport
through ``HttpEndpointOptions::authenticator``, a function that makes the
authenticator, this manager or any other ``auth::Authenticator`` below, from the
executor the client runs on. The client then talks to the server through an
``auth::OAuthClientTransport``.

A manager serves the one server its ``server_url`` names. The transport sends
the manager's tokens to whatever server ``inner`` talks to, and does not compare
that with ``server_url``. Give each server its own manager, and wrap only
transports that talk to that server with it.

.. _oauth-authorization-server-change:

When the authorization server changes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every ``try_handle_challenge`` call reads the protected-resource metadata again
instead of serving the cached copy, because a challenge means the token in hand
was refused and the resource may have moved to another authorization server.
The authorization server the document names now is the one the manager
authorizes at. Client credentials are kept per issuer in the
``credential_store``, so a new server gets a registration of its own (or the
client metadata document, or the pre-registered credentials bound to that
issuer) and never receives another server's client identifier or secret. The
token and the token endpoint the manager refreshes with are replaced together
when the exchange at the new server succeeds; until then a refresh still goes
to the server that issued the token. A concurrent burst of requests that meet
the same challenge shares one flight, so the document is read once for the
burst. Authorization server metadata keeps the discovery cache.

.. _oauth-legacy-discovery:

Servers without protected-resource metadata
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A server written for revision 2025-03-26 publishes no protected-resource
metadata. When the challenge names no ``resource_metadata`` URL and every
well-known location answers ``404`` or ``410``, the manager takes the origin of
``server_url``, with the path dropped, for the authorization server, as that
revision does:

* It reads the authorization-server metadata from the origin's well-known
  locations. The issuer that metadata names may sit under a path of the origin,
  such as ``https://mcp.example.com/oauth``. An issuer on another origin is
  refused.
* When the origin publishes no metadata either, the endpoints are
  ``/authorize``, ``/token`` and ``/register`` at the origin. The authorization
  request carries an S256 PKCE challenge, although no metadata says the server
  supports it.

Every path on the origin then shares one authorization server:
``https://host.example.com/tenant-a/mcp`` and
``https://host.example.com/tenant-b/mcp`` both authorize at
``https://host.example.com``.

Any other miss is reported as an error: a server error, a ``404`` at a redirect
target, a refusal by the fetch policy, or a ``404`` at the URL the challenge
named. Nor does the manager fall back once it has accepted a
protected-resource metadata document for its server, or when its injected
credentials name an issuer on another origin. Nothing it reads or assumes on
this path enters the discovery cache, so the next challenge looks for the
protected-resource metadata again.

``AuthorizationRequest::legacy_discovery`` tells the consent callback that the
attempt took this path, and a callback that does not trust such servers throws
to refuse it. Set ``OAuthAuthorizationConfig::legacy_discovery_fallback`` to
``false`` to refuse them outright.

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
  to the URL it came from (for a server without protected-resource metadata,
  to a URL on the same origin; see :ref:`oauth-legacy-discovery`). A redirect
  or a substituted document cannot move the flow to a different issuer.
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

.. _oauth-scopes-and-step-up:

Scopes, step-up and refresh tokens
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The scope of an authorization request is chosen in this order: the
``scope`` set on ``OAuthAuthorizationConfig``, which wins and is used as it is;
else the ``scope`` of the challenge; else the ``scopes_supported`` of the
protected-resource metadata; else no ``scope`` parameter at all. The challenge
is authoritative for the operation it refused, and the SDK assumes no set
relationship between it and ``scopes_supported``.

A ``403`` whose challenge carries ``error="insufficient_scope"`` names only the
scopes the refused operation needs. When the scope came from the challenge or
from ``scopes_supported``, the manager sends the union of that scope and the
scope the previous authorization **granted**, so a step-up does not drop what
the user already approved. The union is built from what the authorization
server granted, not from what the client asked for: the ``scope`` of the token
response, or the requested scope when the response omits it. Scopes the server
refused are therefore not requested again on every step-up. The granted scope
is kept in memory with the issuer that granted it and is used only for that
issuer; a step-up that moves to another authorization server asks for the
challenge's scope alone. It is not persisted, so after a restart the first
step-up starts from the challenge's scope.

A challenge whose every scope is literally present in the granted scope of the
current issuer adds nothing a new authorization could give, so the manager does
not ask the user again: ``try_handle_challenge`` returns ``false`` and
``OAuthClientTransport`` reports the ``403`` to the caller. The comparison is
exact. A scope the server grants under another spelling, or through a scope
hierarchy, is not recognised and the challenge is acted on. Independently of
that, a request costs at most the authorization its ``401`` asks for and the
step-ups ``OAuthClientTransportOptions::max_step_ups_per_request`` allows.

Refresh tokens are the authorization server's to issue, and a client must not
assume it will. When the authorization server lists ``offline_access`` in its
``scopes_supported``, the manager adds it to the scope of the authorization
request, so the server may issue a refresh token. A server that does not list
it is never sent it. A scope set in ``OAuthAuthorizationConfig::scope`` is used
as it is, without ``offline_access``. Set
``OAuthAuthorizationConfig::request_offline_access`` to ``false`` to request
exactly the selected scope.

``OAuthClientTransport`` handles these challenges for a ``Client`` in every
protocol era: a ``401`` to the ``server/discover`` request that
``ClientProtocol::automatic()`` sends first is authorized and replayed like any
other, and so is a ``403`` on a request retried in a multi round trip
exchange.

Timeouts while the user decides
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The consent callback is the user's time, not the request's.
While ``OAuthClientTransport`` waits for the manager to handle a challenge,
which includes the callback and the discovery and token requests around it, the
deadline of the request being written does not run: neither
``ClientOptions::request_timeout`` nor, for the first request of
``ClientProtocol::automatic()``, ``ClientOptions::probe_timeout``. The deadline
resumes with the time that was left when the authorization ends, so a request
the server then never answers still times out, and a slow prompt never makes
``connect()`` fall back to ``initialize`` against a server that speaks
2026-07-28. Only the request whose write is waiting is affected. A refresh runs
no prompt and keeps the deadline running, and so does any transport other than
``OAuthClientTransport``; a transport of your own reports such a wait through
``ITransport::set_interaction_observer()``.

Nothing bounds the callback itself, so an application whose prompt may never
return must bound it, for example by throwing from the callback after its own
limit. Closing the client fails the waiting request at once.

Client identity
~~~~~~~~~~~~~~~

The manager picks the client identity in this order: credentials injected with
``client_id`` or ``client_identity.pre_registered``, a client ID metadata
document when the authorization server supports them, a registration already
stored for the issuer, and dynamic client registration. Dynamic registration is deprecated by the specification and kept
for compatibility, so prefer one of the first two. A client ID metadata
document URL, ``client_identity.client_metadata_url``, must be an ``https`` URL
with a path, and no fragment, username or password, or ``.`` or ``..`` path
segment; the manager's constructor throws
``std::invalid_argument`` for any other. The SDK neither hosts that document nor
checks that its ``client_id`` equals the URL, which stay the application's
responsibility. Stored registrations are kept per authorization server, so a change of
server never reuses another server's ``client_id`` or secret.

.. _oauth-machine-to-machine:

Machine-to-machine authorization
--------------------------------

Two optional extensions authorize a client without a person at a browser. Both
are implemented as ``Authenticator`` classes that take the place of
``OAuthAuthorizationManager`` under ``OAuthClientTransport``, so discovery, the
fetch policy, the token store and the replay of a refused request work as
described above. Both parse the challenge, read the protected resource metadata
it names (or the well-known location), require that the document's ``resource``
identify the configured ``server_url`` and that the authorization server's
metadata come from the issuer the document names, and request the token with
the RFC 8707 ``resource`` indicator, all before any credential is sent. They
store the token under ``server_url``, like the manager, and keep a token per
authorization server issuer for ``authorize()``. They run no consent step.

Each declares itself to the peer with a helper on the capabilities, as
:doc:`extensions` describes: ``declare_client_credentials_support()`` and
``declare_enterprise_authorization_support()`` accept a ``ClientCapabilities``
or a ``Server``.

Neither authenticator coalesces concurrent callers: two requests refused at the
same moment each run their own discovery and token request. Both leave the
fetch policy as the only gate on the origins contacted, so the policy has to
list every origin involved, including an identity provider's.

.. _oauth-client-credentials:

Client credentials
~~~~~~~~~~~~~~~~~~

``io.modelcontextprotocol/oauth-client-credentials`` uses the OAuth 2.0 client
credentials grant for automated systems. The client holds pre-registered
credentials and no dynamic client registration is involved.
``mcp::auth::ClientCredentialsAuthenticator`` runs the grant:

.. code-block:: cpp

   #include <mcp/core/async.hpp>
   #include <mcp/auth/client_credentials.hpp>

   mcp::auth::ClientCredentialsConfig config;
   config.server_url = "https://mcp.example.com/mcp";
   config.client.client_id = "ci-pipeline";
   config.client.client_secret = read_secret("ci-pipeline");
   config.policy.allowed_origins = {"https://mcp.example.com", "https://auth.example.com"};

   auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
   auto authenticator = std::make_shared<mcp::auth::ClientCredentialsAuthenticator>(
       executor, store, std::move(config));

   // Optional: obtain a token before the first request instead of after a 401.
   co_await authenticator->authorize();

   auto transport = std::make_shared<mcp::auth::OAuthClientTransport>(inner, authenticator);

The extension names two ways to authenticate at the token endpoint, both in
``ClientAuthentication``:

* ``private_key_jwt`` (recommended): set ``client.private_key_jwt`` to a PEM
  private key, an RSA key of at least 2048 bits (``RS256`` or ``PS256``) or an EC key on P-256
  (``ES256``). Each request carries a fresh assertion with the claims ``iss``
  and ``sub`` (the client identifier), ``aud`` (the authorization server's issuer
  unless ``audience`` is set), ``iat``, ``exp`` (60 seconds later unless ``lifetime``
  is set) and a random ``jti``, signed with an algorithm the key allows and the
  server's ``token_endpoint_auth_signing_alg_values_supported`` lists. The form
  holds ``client_assertion_type`` and ``client_assertion`` and no ``client_id``.
* A client secret, sent in the HTTP Basic header (``client_secret_basic``) or in
  the form body (``client_secret_post``).

When ``client.method`` is unset, the method follows from the credentials and
the ``token_endpoint_auth_methods_supported`` the server publishes:
``private_key_jwt`` first, then ``client_secret_basic``, then
``client_secret_post``. A server that publishes no list is assumed to accept
``client_secret_basic``, as RFC 8414 says. If the server lists
``grant_types_supported`` without ``client_credentials``, the grant is refused
before a credential is sent. The scope is ``config.scope``, else the
challenge's ``scope``, else every scope in the resource's
``scopes_supported``, the scope selection strategy of the core authorization
specification, which the extension defers to. A server that lists scopes it
only grants to some clients should therefore name the scope in its challenge or
the client should set ``config.scope``. A step-up
challenge (``insufficient_scope``) requests its scope together with what the
earlier grant covered.

The grant issues no refresh token, so ``try_refresh_token()`` requests a new
token from the authorization server of the last grant. It returns ``false``
when there was none or the request failed, and the transport then falls back to
the challenge.

.. _oauth-enterprise-managed-authorization:

Enterprise-managed authorization
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``io.modelcontextprotocol/enterprise-managed-authorization`` lets an
organization's identity provider (IdP) control which MCP servers its users'
clients may use, with no per-server consent. A client that signed the user in
through the IdP exchanges the identity assertion it holds for an Identity
Assertion JWT Authorization Grant (ID-JAG) at the IdP, then presents the ID-JAG
as a JWT bearer grant to the MCP server's authorization server.
``mcp::auth::EnterpriseAuthorizationAuthenticator`` runs both steps. Signing
the user in and keeping the ID token fresh are the application's work; it
supplies the assertion through a callback:

.. code-block:: cpp

   #include <mcp/core/async.hpp>
   #include <mcp/auth/enterprise_authorization.hpp>

   mcp::auth::EnterpriseAuthorizationConfig config;
   config.server_url = "https://mcp.chat.example/";
   config.idp_token_endpoint = "https://acme.idp.example/oauth2/token";
   config.idp_client.client_id = "2ec954a1d60620116d36d9ceb7";
   config.idp_client.client_secret = read_secret("idp");
   config.client.client_id = "https://client.example.com/client.json";  // a client ID metadata document
   config.policy.allowed_origins = {"https://mcp.chat.example", "https://auth.chat.example",
                                    "https://acme.idp.example"};

   auto authenticator = std::make_shared<mcp::auth::EnterpriseAuthorizationAuthenticator>(
       executor, store, std::move(config),
       []() -> mcp::Task<mcp::auth::IdentityAssertion> {
           co_return mcp::auth::IdentityAssertion{current_id_token()};  // an OpenID Connect ID token
       });

For each token the authenticator:

#. discovers the protected resource and its authorization server, and refuses a
   server whose ``authorization_grant_profiles_supported`` omits
   ``urn:ietf:params:oauth:grant-profile:id-jag``, before the callback runs (a
   server that publishes no such list is tried);
#. calls the callback, which returns the token and its type URN
   (``IdentityAssertion::token_type`` is an ID token by default; a refresh token
   obtained for a SAML assertion has ``g_REFRESH_TOKEN_TYPE``);
#. posts an RFC 8693 token exchange to ``idp_token_endpoint`` with
   ``requested_token_type=urn:ietf:params:oauth:token-type:id-jag``, the
   authorization server's issuer as ``audience``, the protected resource's
   identifier as ``resource``, and the scope, and requires that the IdP answer
   with an ``issued_token_type`` of ``id-jag``;
#. posts the ID-JAG as the ``assertion`` of a
   ``urn:ietf:params:oauth:grant-type:jwt-bearer`` request to the authorization
   server's token endpoint.

A token is cached per authorization server and per identity assertion (the
SHA-256 of the callback's token and type), so ``authorize()`` reuses it only
while the callback returns the assertion the token was issued for.

``idp_client`` is how the client authenticates at the IdP; with a secret and no
``method`` it is sent in the request body, as in the extension's example, and a
private key's assertion names the IdP's token endpoint as its audience unless
``audience`` is set.
``client`` is how it authenticates at the authorization server, with the same
choices as above; a client with neither a secret nor a key sends only its
``client_id``, which fits a client ID metadata document URL used as the
identifier. At the authorization server, ``client_secret_basic`` puts the
credentials in the header and nothing in the body, and ``private_key_jwt`` also
sends ``client_id``.

The access token the authorization server issues is bound to the resource the
ID-JAG names; the SDK does not inspect the ID-JAG. ``try_refresh_token()`` runs
the exchange again and so calls the callback again.

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

The same derivation is available to a caller serving the document itself:
:cpp:func:`mcp::protected_resource_metadata_path` returns the path,
:cpp:func:`mcp::protected_resource_metadata_url` the absolute URL to advertise
in the challenge, and :cpp:func:`mcp::format_protected_resource_metadata` the
JSON body. Both path functions throw ``std::invalid_argument`` when ``resource``
is not an absolute URL with an authority.

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
       [](std::string token) -> mcp::Task<mcp::TokenValidation> {
           co_return co_await introspect(std::move(token));
       });

The token is passed by value because the validator may suspend, and a view into
the request buffer is not guaranteed to survive that. A transport accepts one
validator: installing both throws ``std::logic_error``. A server that installs
only the synchronous one pays nothing for the asynchronous path.

A validator that returns ``false`` gets the client a ``401`` with the bearer
challenge. One that throws, because an introspection endpoint is down for
example, gets it a ``500`` with no challenge: the server failed to decide, and
a ``401`` would send the client into re-authorization for a token that may be
perfectly valid.

The two signatures are exported as :cpp:type:`mcp::BearerTokenValidator`
(``TokenValidation(std::string_view)``) and
:cpp:type:`mcp::AsyncBearerTokenValidator`
(``Task<TokenValidation>(std::string)``), so a validator can be stored and passed around
rather than written inline at the call. Neither is ever handed the
``Authorization`` header itself: the transports strip the scheme first with
:cpp:func:`mcp::http_bearer_token`, which returns the characters after
``Bearer`` and an empty view when the scheme is absent, and an empty token is
rejected before any validator runs.

A validator decides with a :cpp:struct:`mcp::TokenValidation`, which a
``bool`` converts to. To say who a token stands for, return the principal
with the decision, for example the token's subject:

.. code-block:: cpp

   manager.set_bearer_token_validator(
       [](std::string_view token) -> mcp::TokenValidation {
           const auto subject = lookup_subject(token);  // your own lookup
           if (!subject) {
               return false;
           }
           return {true, *subject};
       });

A handler reads it with ``Context::principal()``. Without one from the
validator, the principal is ``h:`` followed by the hex SHA-256 of the
request's bearer token, or of its ``Authorization`` header when that holds no
bearer token: it identifies the token without revealing it, and it changes
when the client refreshes its token, which a principal from the validator does
not. A request without an ``Authorization`` header
has an empty principal, and so do the requests of a
``StreamableHttpSessionManager`` session, which belong to the session.
A session is bound to the principal that created it only when a validator is
installed: without one the ``Authorization`` header is not checked, and a
client may change it within the session. A request naming a session of another
principal is answered ``404``, as for a session that does not exist, so a client
whose principal changed with its token starts a new session. On
``HttpServerTransport``, which holds one session, that new session can start
once the old one has been idle for the session idle timeout.

.. _oauth-required-scopes:

Requiring scopes
~~~~~~~~~~~~~~~~

A token the validator accepts may still lack what a request needs. Name the
scopes each request needs with ``set_scope_requirements``, and report the scopes
a token grants in ``TokenValidation::scopes``:

.. code-block:: cpp

   manager.set_bearer_token_validator(
       [](std::string_view token) -> mcp::TokenValidation {
           const auto grant = lookup_grant(token);  // your own lookup
           if (!grant) {
               return false;
           }
           mcp::TokenValidation validation(true, grant->subject);
           validation.scopes = grant->scopes;
           return validation;
       });
   manager.set_scope_requirements(
       [](const mcp::ScopeRequestContext& request) -> std::vector<std::string> {
           if (request.method == "tools/call" &&
               request.params.value("name", "") == "write_file") {
               return {"files:read", "files:write"};
           }
           return {};
       });

The provider is called for every POSTed JSON-RPC message that has a method,
after the token is accepted and before the message is dispatched or any response
stream is opened, on both ``HttpServerTransport`` and
``StreamableHttpSessionManager``. Return every scope the operation needs at
once. When the token lacks any of them the answer is ``403`` with

.. code-block:: text

   WWW-Authenticate: Bearer error="insufficient_scope", scope="files:read files:write",
                     resource_metadata="https://mcp.example.com/.well-known/oauth-protected-resource/mcp"

The ``scope`` parameter lists all the scopes the operation needs, not only the
missing ones, so a client authorizes once for the operation, and the realm and
metadata URL come from ``set_bearer_challenge`` and
``set_protected_resource_metadata``. ``OAuthClientTransport`` answers this
challenge with a step-up authorization (see
:ref:`oauth-scopes-and-step-up`). A scope is held only when it appears literally
in ``TokenValidation::scopes``; a validator that reports no scopes grants none,
and a scope hierarchy is expanded by the validator, which lists every scope the
token implies. A provider that throws is answered ``500`` with no challenge, as
a validator that throws is, and so is a provider that returns a scope containing
anything but printable ASCII, because it cannot be written into the challenge.
Scopes must be quotable header text. A provider needs a validator to report the
scopes a token holds, so ``listen()`` throws ``std::logic_error`` when
``set_scope_requirements`` is used without a token validator, whichever was set
first. Answers to the server's own requests, ``GET`` and
``DELETE`` carry no method and are not checked.

Health checks and other routes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Routes that must answer before a token exists are named individually:

.. code-block:: cpp

   manager.set_unauthenticated_paths({"/health"});

Each entry is matched for equality against the path component of the request
target, with any query string or fragment removed first, so ``/health`` also
exempts ``/health?probe=1``. :cpp:func:`mcp::http_request_path` performs that
split and is exported, so a custom request handler can key off exactly the same
path the exemption check used.

``HttpServerTransport`` has no custom request handler, so on that transport
the only exempt path that is answered with content is the protected-resource
metadata route; every other exempt path gets ``404 Not Found``. A health check
that has to answer without a token needs ``StreamableHttpSessionManager`` and
its custom request handler.

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

``examples/features/oauth_flow.cpp`` builds its challenge and its metadata route
from one ``ProtectedResourceMetadataConfig`` through these same functions, so it
shows the derivation running rather than restating its result.

Request body size
~~~~~~~~~~~~~~~~~

Both transports cap the HTTP request body they will read, answering
``413 Payload Too Large`` and closing the connection beyond it. The default is
:cpp:var:`mcp::constants::http_server::g_DEFAULT_MAX_REQUEST_BODY_BYTES`, 8 MiB, and
``set_max_request_body_bytes`` overrides it. A request's headers may take up to
8 KiB, the HTTP parser's limit: a request past it is answered
``431 Request Header Fields Too Large``, and one that is not valid HTTP
``400 Bad Request``, and either answer closes the connection.

The headers are read first, and the origin and bearer checks run on them, so
the body of a request they refuse is never read and the connection is closed.

``set_connection_limits()`` takes an :cpp:struct:`mcp::HttpConnectionLimits`.
By default a transport accepts connections up to three quarters of the
process's descriptor limit, closes a connection that takes more than
60 seconds to send a request or to read its answer, and handles at most 4096
requests at once holding at most 1 GiB together, a body counted by what has
arrived and a JSON-RPC message by what it takes parsed. A request past either is answered
``503 Service Unavailable``. These limits cover all sessions of a
``StreamableHttpSessionManager`` together.

A connection past the connection limit is closed as soon as it is accepted,
without an answer, so its client sees the connection end. The descriptor limit
is the soft ``RLIMIT_NOFILE`` (``ulimit -n``). systemd gives a service a soft
limit of 1024 unless its unit sets ``LimitNOFILE=``, and three quarters of that
is 768 connections, with each SSE stream on GET holding one. To serve more,
raise the soft limit, up to the hard limit with ``setrlimit()`` or in the unit
with ``LimitNOFILE=``, or set ``max_connections``.

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

``OAuthHttpClient`` reaches ``https://`` servers over TLS, checking their
certificates as :doc:`/guides/tls` describes; the ``tls`` and ``proxy`` fields of
each authenticator configuration change what it trusts and how it connects. The
metadata-fetch policy that every ``OAuthHttpClient`` request passes admits
``https://`` targets, and plain ``http://`` only to a loopback address when
``allow_plain_http_loopback`` is set. Private, link-local and other internal
addresses are always refused, so an authorization server on an internal network
cannot be reached directly; through a proxy, the proxy resolves names and the
policy checks only addresses written in a URL. Never log access tokens, refresh tokens,
authorization codes, client secrets, or PKCE verifiers.

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
metadata. Discovery results are cached for a configurable interval; the manager drops the
cached protected-resource document before each challenge it handles (see
:ref:`oauth-authorization-server-change`). Install a
:cpp:struct:`mcp::auth::MetadataFetchPolicy` on the ``OAuthHttpClient`` first,
or every fetch is refused.

.. code-block:: cpp

   auto oauth_http = std::make_shared<mcp::auth::OAuthHttpClient>(executor);
   // Without a policy every request is refused. Name the origins to reach.
   mcp::auth::MetadataFetchPolicy policy;
   policy.allowed_origins = {"http://127.0.0.1:9000"};
   policy.allow_plain_http_loopback = true;
   oauth_http->set_metadata_policy(std::move(policy));
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
place. Neither runs a background refresh task. After an HTTP 401 from
``HttpClientTransport``, ``OAuthClientTransport`` renews the token with the
stored refresh token once and retries; if renewal is not possible, or the
renewed token is refused too, it runs the authorization flow the challenge
names, once per request: a 401 after that new grant fails the request. A 403
whose challenge carries ``error="insufficient_scope"`` runs that flow
directly, for the scope already granted together with the one the challenge
asks for. Each such step-up asks for consent again, so a request gets one by
default and then fails with the 403;
``OAuthClientTransportOptions::max_step_ups_per_request`` changes that, and
zero leaves every such 403 to the caller. For non-HTTP transports it also
understands the legacy JSON-RPC
``g_UNAUTHORIZED`` path. ``OAuthAuthorizationManager`` can renew a grant only
after it has authorized in the same process, because the token endpoint and
client identity it renews with are held in memory: after a restart, a refresh
token kept in a persistent ``TokenStore`` is not used, and the next 401 starts
a new authorization. Because a request can be replayed, callers should
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

The client passes every OAuth scenario of the 2025-11-25 conformance suite that
CI runs: the protected-resource metadata variants, CIMD and pre-registration,
scope selection and step-up, and the ``basic``, ``post`` and ``none``
token-endpoint authentication scenarios. ``conformance/expected-failures.yml``
lists the scenarios the SDK does not pass yet. DPoP and workload identity
federation, open extension proposals, are not implemented (see
:doc:`/spec-support`).

Cross-references
----------------

* See :doc:`transports` for HTTP transport security and Origin validation.
* See :doc:`context` for request context and the legacy metadata path.
