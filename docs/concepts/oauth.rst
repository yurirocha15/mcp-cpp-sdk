OAuth
=====

OAuth 2.0 and 2.1 support in the MCP C++ SDK enables secure, delegated authentication for both clients and servers. This implementation provides comprehensive tools for discovery, token management, and automatic transport-level authentication.

The OAuth features are primarily designed for remote transports like HTTP or WebSockets, where persistent and secure identification is required.

OAuth Overview
--------------

The SDK implements the modern OAuth 2.1 authorization code flow with **PKCE** (Proof Key for Code Exchange) by default. This ensures high security for native applications and public clients without requiring client secrets.

Key components:

* **OAuthConfig**: Defines client identifiers and server endpoints.
* **OAuthDiscoveryClient**: Automatically resolves metadata from `.well-known` endpoints.
* **TokenStore**: Persistently stores access and refresh tokens.
* **OAuthAuthenticator**: Manages the lifecycle of tokens, including automatic background refresh.
* **OAuthClientTransport**: A transport wrapper that injects bearer tokens into outgoing requests.
* **Auth Middleware**: Server-side protection for MCP tools and resources.

Configuration (OAuthConfig)
---------------------------

The `OAuthConfig` structure holds the necessary parameters for interacting with an OAuth authorization server.

.. code-block:: cpp

   #include <mcp/auth/oauth.hpp>

   mcp::auth::OAuthConfig config;
   config.client_id = "my-mcp-client";
   config.token_endpoint = "https://auth.example.com/token";
   config.authorization_endpoint = "https://auth.example.com/authorize";
   config.redirect_uri = "http://localhost:8080/callback";
   config.scope = "mcp:full_access";

Discovery (OAuthDiscoveryClient)
--------------------------------

Instead of hardcoding endpoints, the `OAuthDiscoveryClient` can resolve authorization server details and protected resource metadata using standard `.well-known` discovery documents.

.. code-block:: cpp

   auto oauth_http = std::make_shared<mcp::auth::OAuthHttpClient>(executor);
   mcp::auth::OAuthDiscoveryClient discovery(oauth_http);

   // Discover which auth server protects a specific resource
   auto resource_meta = co_await discovery.discover_protected_resource("https://api.example.com/mcp");

   // Discover endpoints for an authorization server
   auto auth_meta = co_await discovery.discover_auth_server(resource_meta.authorization_servers.front());

Token Storage (TokenStore)
--------------------------

Tokens are managed through the `TokenStore` interface. The SDK provides a thread-safe `InMemoryTokenStore` for development and volatile sessions. For production, you can implement a custom `TokenStore` that persists tokens to a secure database or keychain.

.. code-block:: cpp

   auto token_store = std::make_shared<mcp::auth::InMemoryTokenStore>();

PKCE Flow
---------

For secure authorization code flows, use `generate_pkce_pair()` to create a code verifier and challenge.

.. code-block:: cpp

   auto pkce = mcp::auth::generate_pkce_pair();
   // Send pkce.code_challenge to the authorization endpoint
   // Send pkce.code_verifier to the token endpoint during exchange

Client-Side Authentication
--------------------------

The `OAuthAuthenticator` and `OAuthClientTransport` work together to automate authentication. The transport wrapper automatically injects the `auth_token` into the `_meta` field of every MCP request.

.. literalinclude:: ../../examples/features/oauth_flow.cpp
   :language: cpp
   :lines: 410-413
   :dedent: 8

Automatic Token Refresh
^^^^^^^^^^^^^^^^^^^^^^^

If a request fails with an authentication error (JSON-RPC codes -32001 or -32000), the `OAuthClientTransport` will:

1. Catch the error response.
2. Call `authenticator->try_refresh_token()` using the stored refresh token.
3. If successful, automatically replay the original request with the new token.

This ensures a seamless experience for the user even when access tokens expire.

Server-Side Protection
----------------------

On the server side, you can protect your tools and resources using `make_auth_middleware`. This middleware extracts the bearer token from incoming request metadata and validates it via a callback.

.. literalinclude:: ../../examples/features/oauth_flow.cpp
   :language: cpp
   :lines: 524-530
   :dedent: 8

Example Walkthrough
-------------------

The following example demonstrates a complete OAuth integration, from discovery to authenticated tool calls with automatic refresh.

.. literalinclude:: ../../examples/features/oauth_flow.cpp
   :language: cpp
   :lines: 378-474
   :dedent: 0

Cross-References
----------------

* See :doc:`transports` for more information on transport wrappers.
* See :doc:`context` for details on request metadata.
