OAuth Security and Deployment
==============================

Deploying OAuth with the mcp-cpp-sdk requires careful attention to token management, network security, and issuer binding. This guide covers the operational controls and design rationale that developers and operators must understand to deploy OAuth safely.

Token Persistence and Storage
------------------------------

The :cpp:class:`mcp::auth::InMemoryTokenStore` is thread-safe but volatile: tokens are lost when the process exits. Production deployments **must not** rely on it.

Implementing a Persistent TokenStore
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Implement :cpp:class:`mcp::auth::TokenStore` with encryption and OS-appropriate secret storage:

.. code-block:: cpp

   class MyTokenStore : public mcp::auth::TokenStore {
   public:
       void store(const std::string& server_url,
                  mcp::auth::TokenResponse token) override {
           // Encrypt token and persist to secure storage
           // (e.g., OS keychain, encrypted file, secret manager)
           auto encrypted = encrypt_token(token);
           persist_to_secure_storage(server_url, encrypted);
       }

       std::optional<mcp::auth::TokenResponse> load(
           const std::string& server_url) const override {
           // Retrieve and decrypt from secure storage
           auto encrypted = retrieve_from_secure_storage(server_url);
           if (!encrypted) return std::nullopt;
           return decrypt_token(*encrypted);
       }

       void remove(const std::string& server_url) override {
           // Delete from secure storage
           delete_from_secure_storage(server_url);
       }
   };

Never log access tokens, refresh tokens, authorization codes, client secrets, or PKCE verifiers.

Credential-to-Issuer Binding
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The :cpp:class:`mcp::auth::ClientCredentialStore` is **deliberately separate** from :cpp:class:`mcp::auth::TokenStore` because they use different storage keys:

- **TokenStore** is keyed by MCP server URL.
- **ClientCredentialStore** is keyed by authorization-server issuer.

This separation (SEP-2352) prevents a vulnerability where a single MCP server protected by different authorization servers over time could accidentally present one server's credentials to another. One authorization server can also protect multiple MCP server URLs, so server-URL keying would eventually misattribute credentials.

**Do not merge these stores.** Use separate persistent implementations, keyed correctly, and validate that a stored credential's issuer matches the issuer being contacted before presenting the credential. The SDK's :cpp:func:`mcp::auth::select_client_identity` function validates the issuer binding automatically when reusing stored credentials; operators who implement custom storage must enforce the same check.

Metadata-Fetch Policy and Origin Validation
--------------------------------------------

Every OAuth metadata fetch — challenge-supplied `resource_metadata` URLs, protected-resource discovery, authorization-server discovery, and token-endpoint requests derived from discovered metadata — is an outbound-request primitive controlled by the :cpp:struct:`mcp::auth::MetadataFetchPolicy`.

Policy Default Behavior
~~~~~~~~~~~~~~~~~~~~~~~~

A default-constructed policy **denies every origin**: :cpp:member:`mcp::auth::MetadataFetchPolicy::allowed_origins` is empty, and an origin not explicitly listed is refused **before host resolution**. When the URL is refused, no socket is opened and no DNS lookup occurs.

.. code-block:: cpp

   mcp::auth::MetadataFetchPolicy policy;
   // Every origin is denied; no metadata fetch succeeds without configuration.

Configuring Allowed Origins
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

List the exact origins your deployment intends to contact:

.. code-block:: cpp

   mcp::auth::MetadataFetchPolicy policy;
   policy.allowed_origins = {
       "https://accounts.example.com",
       "https://auth.internal:8443",
       "https://protected-resource.example.org"
   };

Comparison is **exact**: no normalization of case, default ports, or trailing slashes is applied. An allow-list entry must be written exactly as it appears in the URL.

Deny List Always Wins
~~~~~~~~~~~~~~~~~~~~~

The deny list is consulted **before** the allow list. An origin in both lists is refused:

.. code-block:: cpp

   policy.allowed_origins = {"https://auth.example.com"};
   policy.denied_origins = {"https://auth.example.com"};
   // Result: origin is DENIED
   // Use this to temporarily block a server without removing it from the allow list.

Runtime Override with origin_allowance Callback
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Applications that cannot enumerate all authorization servers in advance can implement the :cpp:member:`mcp::auth::MetadataFetchPolicy::origin_allowance` callback. It is consulted only when an origin is not in the allow list, and its return value determines whether the origin is allowed:

.. code-block:: cpp

   policy.origin_allowance = [](const std::string& origin) {
       // Custom rule: allow any HTTPS origin from internal domain
       if (origin.find("https://") == 0 &&
           origin.find(".internal:") != std::string::npos) {
           return true;
       }
       // Otherwise deny
       return false;
   };

The callback:

- **Cannot relax scheme validation**: it cannot permit plain `http://` when the policy enforces HTTPS.
- **Cannot relax address validation**: it cannot permit loopback, private ranges, link-local, multicast, or reserved addresses after they have been rejected by address classification.
- Runs **after** the deny list, so denied origins stay denied.
- Runs **before** host resolution, so a rejected origin never results in a socket opening.

Metadata URLs named by a protected resource are attacker-influenced input; the callback implements the rule that would otherwise be written as a complete allow list. Do not use it as a blanket escape hatch.

Issuer Comparison and Canonical Form
-------------------------------------

When an authorization server advertises support for RFC 9207 `iss`, the SDK validates that the issuer in the authorization response matches the issuer recorded from the selected authorization server's metadata. This comparison is **a plain byte-for-byte string equality check**: no canonicalization is performed.

Why No Canonicalization
~~~~~~~~~~~~~~~~~~~~~~~~

String equality without URL normalization might seem unsafe. Consider this threat:

1. Attacker registers `https://attacker.evil` as an authorization server in operator's metadata-fetch policy.
2. Attacker directs victim to `https://attacker-evil.com` (note the difference).
3. The two domains resolve to the same IP address or are aliased via DNS.
4. If the SDK normalized URLs or resolved hostnames to compare, `https://attacker.evil` and `https://attacker-evil.com` might appear equivalent and bypass the issuer check.
5. Attacker's server could then issue tokens claiming to be from a trusted issuer, or replace legitimate issuer URLs with attacker-controlled variants.

By comparing only the byte sequence as written, the SDK ensures that:

- An issuer URL is only accepted if it matches **exactly** what was advertised in the trusted metadata document.
- Hostname canonicalization, default-port elision, trailing-slash normalization, or percent-encoding variations cannot defeat the binding.
- No DNS-like resolution step can make two different issuer URLs appear equivalent.

The issuer binding is only as strong as the metadata document it comes from. The SDK validates the metadata URL itself (origin, address, redirect path) before fetching; that separate validation is your defense against SSRF attacks on the metadata endpoint.

Localhost Plain-HTTP Loopback Opt-Out
--------------------------------------

The :cpp:member:`mcp::auth::MetadataFetchPolicy::allow_plain_http_loopback` flag permits plain `http://` URLs and loopback addresses. This is a narrow opt-out intended **only for loopback development and test fixtures** such as the official MCP conformance runner.

.. code-block:: cpp

   // Development / test fixture only
   mcp::auth::MetadataFetchPolicy policy;
   policy.allow_plain_http_loopback = true;  // Narrow opt-out

**Production deployments must NOT enable this flag.** HTTPS is the default and the security boundary for OAuth.

Once enabled:

- Plain `http://` schemes are accepted for loopback addresses (127.0.0.1, ::1).
- Loopback addresses themselves are unblocked (they are otherwise refused as private ranges).
- This flag does **not** relax any other control: link-local, private non-loopback, multicast, reserved, or address-based denials still apply. Redirect chains, response sizes, and origin checks remain enforced.
- It is never enabled implicitly; an operator must explicitly set it to `true`.

If you are integrating with the MCP conformance suite or a local test server, enable this flag only in the test or development configuration, never in production deployments.

SSRF and DNS-Rebinding Prevention
----------------------------------

Metadata URLs are attacker-influenced input when they come from a challenge or a protected resource's `authorization_servers` list. The :cpp:struct:`mcp::auth::MetadataFetchPolicy` includes controls to prevent Server-Side Request Forgery (SSRF) and DNS-rebinding attacks.

Blocked Address Classes
~~~~~~~~~~~~~~~~~~~~~~~

Before connecting to a resolved address, the SDK classifies it and refuses several ranges:

- **Link-local** (169.254.0.0/16 for IPv4, fe80::/10 for IPv6): typically used for local-network autoconfiguration, not safe routing targets.
- **RFC 1918 private ranges** (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 for IPv4) and IPv6 unique-local (fc00::/7): internal network addresses.
- **Loopback** (127.0.0.0/8 for IPv4, ::1 for IPv6): unless explicitly permitted by `allow_plain_http_loopback`.
- **Multicast and broadcast**: not unicast-routable.
- **Reserved, unspecified, or non-routable**: e.g., 0.0.0.0, 255.255.255.255, documentation prefixes.

Addresses written as IP literals in the URL are classified without any DNS lookup. A URL naming `169.254.169.254` or a private-range IP is refused immediately.

Resolve-Then-Pin
~~~~~~~~~~~~~~~~

When a hostname must be resolved, the SDK performs a single lookup, classifies each resolved address, and connects only to addresses that pass the check. Critically, **addresses obtained from that single resolution are pinned for the connection**: a second resolution of the same hostname cannot redirect an established fetch, preventing DNS-rebinding attacks where:

1. First lookup of `attacker.com` returns a public IP.
2. Metadata fetch begins to that IP.
3. Second lookup of `attacker.com` returns a private IP.
4. Attacker tries to pivot the connection to the private IP.

This cannot happen: the first resolution's results are used for the entire fetch.

Bounded and Re-Validated Redirects
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

HTTP redirects are followed up to a configurable limit (default 3):

.. code-block:: cpp

   policy.max_redirects = 3;  // Default; adjust as needed

Each redirect target URL is validated afresh against the scheme, origin, and address controls. A redirect chain cannot escape the policy by landing on a forbidden origin or address.

Response Size Caps
~~~~~~~~~~~~~~~~~~

Metadata and token responses are capped to prevent unbounded buffering:

.. code-block:: cpp

   policy.max_response_bytes = 256 * 1024;  // Default: 256 KiB

A response exceeding the cap is rejected without being fully buffered, protecting against slowloris or resource-exhaustion attacks.

Example: Hardened Policy Configuration
---------------------------------------

A production authorization-server integration might configure:

.. code-block:: cpp

   mcp::auth::MetadataFetchPolicy policy;

   // Explicit allow list of trusted authorization servers
   policy.allowed_origins = {
       "https://accounts.example.com",
       "https://auth-backup.example.com"
   };

   // Temporary block during incident
   policy.denied_origins = {"https://accounts.example.com:8080"};

   // For discovered protected-resource metadata that names
   // authorization servers not in the allow list, require explicit
   // operator approval via a callback (optional; safer to omit)
   policy.origin_allowance = [](const std::string& origin) {
       // Only allow explicit HTTPS from trusted domain
       return origin.find("https://trusted-domain.internal") == 0;
   };

   // Response limits
   policy.max_response_bytes = 256 * 1024;
   policy.max_redirects = 3;

   // Never set allow_plain_http_loopback in production
   policy.allow_plain_http_loopback = false;  // Explicitly documented

Pass this policy to every :cpp:class:`mcp::auth::OAuthHttpClient`:

.. code-block:: cpp

   auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(executor);
   http_client->set_metadata_policy(policy);

Cross-References
----------------

- See :doc:`/concepts/oauth` for OAuth flow overview and API reference.
- See :doc:`/concepts/transports` for general HTTP transport security.
- See :doc:`deployment` for overall production deployment patterns.
