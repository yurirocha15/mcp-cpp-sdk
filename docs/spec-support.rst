Specification Support
=====================

The SDK follows protocol revision 2026-07-28. The tables list what the Model
Context Protocol specification defines and which parts the SDK implements, as a
server and as a client. The older revisions 2025-11-25, 2025-06-18, 2025-03-26 and
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
     - No
     - No
     - The specification asks new implementations not to adopt it.
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

Revision 2026-07-28 deprecates these features. They stay in the specification
until at least the first revision released on or after 2027-07-28, and peers of
earlier revisions still use them, so the SDK keeps them. Their APIs are marked
deprecated in the API reference without a compiler warning.

.. list-table::
   :header-rows: 1
   :widths: 25 40 35

   * - Feature
     - Migration path the specification names
     - In the SDK
   * - Roots
     - Pass directories or files through tool parameters, resource URIs or
       server configuration.
     - ``Context::request_roots()``, ``Client::set_roots()``,
       ``Client::on_roots_list()``
   * - Sampling
     - Integrate directly with LLM provider APIs.
     - ``Context::sample_llm()``, ``Client::on_sampling()``
   * - Logging
     - Log to standard error on stdio; use OpenTelemetry for observability.
     - ``Context::log()``, ``Context::log_info()``,
       ``RequestOptions::log_level``
   * - Dynamic client registration
     - Client ID metadata documents.
     - Used last, after pre-registered credentials, a client ID metadata
       document and a stored registration. :doc:`concepts/oauth`
   * - ``includeContext`` values ``thisServer`` and ``allServers``
     - Omit the field or use ``"none"``.
     - A server sends them only to a client that declares
       ``sampling.context``.
