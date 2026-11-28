Specification Support
=====================

This page lists what the Model Context Protocol specification defines and which
parts the SDK implements, as a server and as a client. It follows protocol
revision 2026-07-28. The older revisions 2025-11-25, 2025-06-18, 2025-03-26 and
2024-11-05 are served on the same transports; :doc:`guides/older-protocols`
explains how a client and a server choose between them.

The status column says where a feature is defined: "Core" for the core
specification, where many features are still optional for an implementation
(authorization, for one, is optional as a whole); "Deprecated" for a core
feature the revision deprecates; "Extension" for a feature specified outside
the core specification.

Base Protocol
-------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 10 10 38

   * - Feature
     - Status
     - Server
     - Client
     - Notes
   * - Per-request ``_meta`` (version, client info, capabilities, log level)
     - Core
     - Yes
     - Yes
     - :doc:`concepts/protocol`
   * - ``server/discover`` and version negotiation
     - Core
     - Yes
     - Yes
     - A client retries with a version both sides support when the server
       refuses its first choice.
   * - Older revisions with ``initialize``
     - Core
     - Yes
     - Yes
     - :doc:`guides/older-protocols`
   * - Cancellation
     - Core
     - Yes
     - Yes
     - A request that times out is cancelled on the wire.
       :doc:`concepts/context`
   * - Progress
     - Core
     - Yes
     - Yes
     - Rate limiting is opt-in on both sides
       (``Server::set_notification_rate_limit()``,
       ``ClientOptions::notification_rate_limit``).
   * - Multi round-trip requests (``input_required``)
     - Core
     - Yes
     - Yes
     - Request state can be sealed. :doc:`concepts/protocol`
   * - ``subscriptions/listen``
     - Core
     - Yes
     - Yes
     - :doc:`concepts/subscriptions`
   * - JSON Schema 2020-12 for tool schemas
     - Core
     - Yes
     - Yes
     - Other dialects are listed in :doc:`concepts/tools`.
   * - Pagination
     - Core
     - Yes
     - Yes
     - :doc:`concepts/pagination`

Transports
----------

.. list-table::
   :header-rows: 1
   :widths: 30 12 10 10 38

   * - Transport
     - Status
     - Server
     - Client
     - Notes
   * - stdio
     - Core
     - Yes
     - Yes
     - A client restarts a server that fails when a ``StdioRestartPolicy``
       asks it to (off by default), and opens its ``subscriptions/listen``
       streams again. :doc:`concepts/transports`
   * - Streamable HTTP
     - Core
     - Yes
     - Yes
     - TLS, mutual TLS and HTTP proxies are covered in :doc:`guides/tls`.
   * - HTTP+SSE (revision 2024-11-05)
     - Deprecated
     - Yes
     - Yes
     - The specification asks new implementations not to adopt it. Revisions
       before 2026-07-28 only. :ref:`transport-http-sse`
   * - Custom transports
     - Core
     - Yes
     - Yes
     - ``ITransport``. :doc:`concepts/transports`

Server Features
---------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 10 10 38

   * - Feature
     - Status
     - Server
     - Client
     - Notes
   * - Tools, with output schemas and ``x-mcp-header``
     - Core
     - Yes
     - Yes
     - Tool names are checked against the specification's naming rules.
       :doc:`concepts/tools`
   * - Resources and resource templates
     - Core
     - Yes
     - Yes
     - :doc:`concepts/resources`
   * - ``resources/subscribe`` (revisions before 2026-07-28)
     - Core
     - Yes
     - Yes
     - A client sends it with ``send_request()``.
   * - Prompts
     - Core
     - Yes
     - Yes
     - :doc:`concepts/prompts`
   * - Completion
     - Core
     - Yes
     - Yes
     - :doc:`concepts/completions`
   * - Cache hints (``ttlMs``, ``cacheScope``)
     - Core
     - Yes
     - Yes
     - The client cache is opt-in and serves one client only.
       :doc:`concepts/protocol`
   * - Logging
     - Deprecated
     - Yes
     - Yes
     - See `Deprecated Features`_.

Client Features
---------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 10 10 38

   * - Feature
     - Status
     - Server
     - Client
     - Notes
   * - Elicitation, form and URL modes
     - Core
     - Yes
     - Yes
     - Answers are checked against the requested schema on both sides.
       :doc:`concepts/elicitation`
   * - Sampling, with tools
     - Deprecated
     - Yes
     - Yes
     - See `Deprecated Features`_.
   * - Roots
     - Deprecated
     - Yes
     - Yes
     - See `Deprecated Features`_.

Authorization
-------------

.. list-table::
   :header-rows: 1
   :widths: 30 12 10 10 38

   * - Feature
     - Status
     - Server
     - Client
     - Notes
   * - OAuth 2.1 with PKCE, discovery and RFC 9207 ``iss`` checks
     - Core
     - n/a
     - Yes
     - Includes step-up for missing scopes and token refresh.
       :doc:`concepts/oauth`
   * - Discovery for 2025-03-26 servers without protected resource metadata
     - Core
     - n/a
     - Yes
     - The server's origin is taken for its authorization server, with default
       endpoints when it publishes no metadata. On by default.
       :ref:`oauth-legacy-discovery`
   * - Client ID metadata documents and pre-registered clients
     - Core
     - n/a
     - Yes
     - :doc:`concepts/oauth`
   * - Protected resource metadata and bearer validation
     - Core
     - Yes
     - n/a
     - The application validates tokens with its own callback.
   * - Authorization server
     - Core
     - No
     - n/a
     - Issuing tokens is left to a separate authorization server.
   * - Client credentials, enterprise-managed authorization
     - Extension
     - n/a
     - Yes
     - Off until enabled. :doc:`concepts/extensions`
   * - DPoP, workload identity federation
     - Extension
     - No
     - No
     - Open proposals; not implemented.

Extensions
----------

.. list-table::
   :header-rows: 1
   :widths: 30 12 10 10 38

   * - Extension
     - Status
     - Server
     - Client
     - Notes
   * - Tasks
     - Extension
     - Yes
     - Yes
     - :doc:`concepts/tasks`
   * - MCP Apps
     - Extension
     - Yes
     - Yes
     - :doc:`concepts/apps`
   * - Skills
     - Extension
     - Yes
     - Yes
     - :doc:`concepts/skills`

.. _spec-deprecated:

Deprecated Features
-------------------

The specification deprecates these features and lists them in its registry of
deprecated features. Peers of earlier revisions still use them, so the SDK
keeps them. Their APIs warn at compile time, and the SDK reports their first
use at run time; :doc:`guides/deprecations` shows both and how to turn them off.

.. list-table::
   :header-rows: 1
   :widths: 22 14 32 32

   * - Feature
     - Deprecated by
     - Migration path the specification names
     - In the SDK
   * - Roots
     - SEP-2577, 2026-07-28
     - Pass directories or files through tool parameters, resource URIs or
       server configuration.
     - ``Context::request_roots()``, ``Client::set_roots()``,
       ``Client::on_roots_list()``, ``Server::on_roots_list_changed()``
   * - Sampling
     - SEP-2577, 2026-07-28
     - Integrate directly with LLM provider APIs.
     - ``Context::sample_llm()``, ``Client::on_sampling()``
   * - Logging
     - SEP-2577, 2026-07-28
     - Log to standard error on stdio; use OpenTelemetry for observability.
     - ``Context::log()``, ``Context::log_info()``,
       ``Server::get_log_level()``, ``RequestOptions::log_level``
   * - Dynamic client registration
     - PR #2858, 2026-07-28
     - Client ID metadata documents.
     - Used last, after pre-registered credentials, a client ID metadata
       document and a stored registration; ``build_registration_request()``.
       :doc:`concepts/oauth`
   * - ``includeContext`` values ``thisServer`` and ``allServers``
     - SEP-2596, 2025-11-25
     - Omit the field or use ``"none"``.
     - ``mcp::constants::include_context::g_THIS_SERVER`` and
       ``g_ALL_SERVERS``. A server sends them only to a client that declares
       ``sampling.context``.

   * - HTTP+SSE transport
     - SEP-2596, 2025-03-26
     - Streamable HTTP.
     - ``LegacySseClientTransport``, ``Endpoint::legacy_sse()``,
       ``StreamableHttpSessionManager::set_legacy_sse()``.
       :ref:`transport-http-sse`

The earliest removal of Roots, Sampling, Logging and dynamic client
registration is the first revision released on or after 2027-07-28; the
``includeContext`` values go with Sampling, and the HTTP+SSE transport goes three
months after SEP-2596 reaches Final.
