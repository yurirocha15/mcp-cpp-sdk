# Changelog

All notable changes are documented here. This project follows Semantic
Versioning; a `0.x` version is a stable release unless its version has an
`-rc.N` suffix.

## [Unreleased]

### Added

- Client error and request options: `McpError` (thrown for error responses,
  timeouts and closed connections, carrying the JSON-RPC code),
  `ClientOptions`, `RequestOptions` with a `send_request()` overload that
  takes it, the `g_CONNECTION_CLOSED` code, and `RequestId::correlation_key()`.
- Tool result helpers `make_tool_text_result()`, `make_tool_error_result()`
  and `make_tool_structured_result()`, and `Server::add_raw_tool()` for a
  handler that builds the whole `CallToolResult` itself.
- `Server::add_resource_template(tmpl, handler)`: `resources/read` requests
  whose URI matches the template's RFC 6570 pattern are routed to the handler.
- Bearer authentication over HTTP: `HttpClientTransport::set_bearer_token_provider()`,
  `set_bearer_token_validator()` on `HttpServerTransport` and
  `StreamableHttpSessionManager` (with the `BearerTokenValidator` type and the
  `http_bearer_token()` helper), and `HttpStatusError` for a non-success HTTP
  status seen by the client.
- `OAuthClientTransportOptions` and an `OAuthClientTransport` constructor
  that takes it, bounding how many requests, and how many bytes of them, are
  kept for a replay after a token refresh, and for how long.
- `WebSocketServerTransport::set_allowed_origins()` and
  `set_allow_all_origins()`.
- `Context` takes a `MessageSender`, so handlers can write to the peer through
  the session's serialized writer. `MemoryTransport` is exported from the
  shared library and is no longer copyable.
- Fixtures and a pinned harness for the official MCP conformance runner
  (`@modelcontextprotocol/conformance@0.1.16`, spec revision `2025-11-25`),
  with a regression baseline in `conformance/expected-failures.yml`, a
  `--conformance` build flag, and a CI workflow that fails on any drift.
- Reproducible cross-SDK benchmark tooling (order counterbalancing,
  environment/container capture, resource-headroom validation, protocol
  verification) and audited benchmark results.
- Project policy documents: `ROADMAP.md` (tier gates), `MAINTENANCE.md`
  (triage SLAs), `VERSIONING.md` (compatibility surface), and
  `DEPENDENCY_POLICY.md` (runtime dependency floors).
- GitHub issue templates and a label manifest (`.github/labels.yml`).
- JSON-RPC error code constants from the spec-reserved band:
  `g_HEADER_MISMATCH` (`-32020`), `g_MISSING_REQUIRED_CLIENT_CAPABILITY`
  (`-32021`) and `g_UNSUPPORTED_PROTOCOL_VERSION` (`-32022`).
- An optional `extensions` member on `ClientCapabilities` and
  `ServerCapabilities`, keyed by extension name. Adding it changes the layout
  of both structs.
- `g_PROTOCOL_VERSION_2026_07_28` and `g_DISCOVERABLE_PROTOCOL_VERSIONS`, the
  version list for `server/discover`. `g_SUPPORTED_PROTOCOL_VERSIONS`, which
  governs `initialize` negotiation, is unchanged.
- `OAuthAuthorizationManager`, an `Authenticator` that runs the MCP
  authorization flow from a `WWW-Authenticate` challenge: protected-resource
  and authorization-server discovery, client identity selection
  (pre-registered credentials, a client ID metadata document, a stored
  registration or dynamic registration, with `ClientCredentialStore` keyed by
  issuer), PKCE, issuer binding, the authorization-code exchange, refresh, and
  scope step-up. The application supplies consent as an
  `AuthorizationCallback`. An authorization server whose metadata does not
  list `S256` in `code_challenge_methods_supported` is refused, as the MCP
  authorization specification requires. Injected credentials that carry a
  `client_secret` must name their authorization server
  (`OAuthAuthorizationConfig::client_issuer` or the pre-registered `issuer`);
  a secret bound to no issuer is never presented. The built-in client has no
  TLS support, so in this release it can reach only an authorization server
  on a loopback address.
- `MetadataFetchPolicy`, which every `OAuthHttpClient` request passes: allowed
  and denied origins, an `origin_allowance` hook, address classification that
  refuses private, link-local and reserved targets and loopback unless
  `allow_plain_http_loopback` is set, and redirect and response-size limits.
  A `denied_origins` entry that is not a bare origin is rejected with
  `MetadataPolicyError` (`denied_origin_entry_malformed`), and every target is
  refused until it is corrected. `OAuthHttpClient::set_metadata_policy()` and
  `set_host_resolver()` configure a client.
- `parse_www_authenticate()`, `select_bearer_challenge()` and
  `BearerChallenge` for reading `WWW-Authenticate` headers.
- The `server/discover` method, answered over HTTP before and after a session
  exists, with `Server::set_instructions()`, `set_discover_ttl_ms()` and
  `set_discover_cache_scope()`.
- The conformance harness runs the client authorization scenarios, and all 14
  of them pass.

### Changed

- Implementation moved out of oversized headers into compiled translation
  units (OAuth, client runtime, protocol tools, memory transport, HTTP
  types); protocol models and typed handler templates remain header-based.
- Client requests now time out after 30 seconds by default and fail with
  `g_REQUEST_TIMEOUT` (`ClientOptions::request_timeout`, or
  `RequestOptions::timeout` for one `send_request()` call). Previously a
  request waited until the peer answered.
- Over an `HttpClientTransport`, `OAuthClientTransport` sends the access
  token in the HTTP `Authorization: Bearer` header and no longer adds
  `_meta.auth_token` to requests, so a server that checks tokens with
  `make_auth_middleware()` rejects them; such a server should use
  `set_bearer_token_validator()`. Other transports still carry the token in
  `_meta.auth_token`. The transport refreshes the token on an HTTP 401 or a
  `-32000` error response; `-32001` is now the client's request timeout code
  and no longer triggers a refresh.
- `Client::connect()` throws `McpError` when called a second time, and
  `send_request()` throws when called before `connect()`.
- The client rejects an incoming message whose `jsonrpc` member is missing or
  not `"2.0"` (`ClientOptions::strict_protocol_validation`, on by default):
  the read loop stops, pending requests fail with `g_CONNECTION_CLOSED`, and
  the transport is closed.
- `HttpServerTransport` and `StreamableHttpSessionManager` answer `403` to a
  request that carries an `Origin` header unless that origin was added with
  `set_allowed_origins()` or `set_allow_all_origins(true)` was called.
  Requests without an `Origin` header are unaffected. `Server::run_http()`
  uses these defaults; to accept browser clients, configure an
  `HttpServerTransport` and pass it to `Server::run()`.
- `WebSocketServerTransport` answers `403` to an upgrade request that carries
  an `Origin` header unless that origin was added with `set_allowed_origins()`
  or `set_allow_all_origins(true)` was called. Upgrades without an `Origin`
  header are unaffected. Previously every upgrade was accepted.
- A server session enforces the initialization sequence: until
  `notifications/initialized` arrives, every request other than `initialize`
  and `ping` is answered with `g_INVALID_REQUEST`, and a second `initialize`
  within a session is rejected. When `Server::run()` is given an
  `HttpServerTransport` (not a transport that wraps one), a session the client
  ended with `DELETE` is over, and the next client must send `initialize` to
  start a new one. Nothing of the ended session reaches the next one: its
  events are not replayed, its resource subscriptions deliver no updates, its
  handlers' responses, notifications and requests are not sent, and the log
  level is reset to the default. Its requests still waiting for an answer are
  answered with `404`.
- `HttpServerTransport` and `StreamableHttpSessionManager` answer a request
  whose id is neither a string nor an integer with `400` and a `-32600`
  error response.
- Tool results are always sent as a `CallToolResult`. A value returned by a
  JSON tool handler, or by a typed handler whose result type is not
  `CallToolResult`, is sent unchanged only when it is an object made of
  `CallToolResult` members (`content`, `structuredContent`, `isError`,
  `_meta`) that decodes as one; an object whose only member is an empty
  `content` array is not. Any other value is wrapped: an object becomes
  `structuredContent` with its serialization as the text block, a string
  becomes the text block, and any other value is serialized into the text
  block. Previously a value was sent as the result unchanged, except that a
  tool with an `outputSchema` also had it copied into `structuredContent`.
  `add_raw_tool()` handlers must return a valid `CallToolResult`, and a tool
  with an `outputSchema` must return `structuredContent`.
- A `std::exception` thrown by any tool handler, typed and asynchronous ones
  included, or by middleware during `tools/call`, is answered with a tool
  error result (`isError: true`) carrying the exception message. Previously
  only the synchronous JSON `add_tool()` overload did this, and other tool
  handlers' exceptions were answered with `-32603`. Resource and prompt
  handler exceptions are still answered with `-32603`.
- Request params that fail to decode are answered with `-32602` (previously
  `-32603`). A message the server cannot parse is answered with `-32700` and
  the session stays open; previously the session ended.
- Registering a second tool, resource, prompt or resource template with the
  same name, URI or URI template throws `std::invalid_argument`.
- `HttpServerTransport` and `StreamableHttpSessionManager` accept
  configuration only before `listen()`, and `listen()` only once; later calls
  throw `std::logic_error`. A second concurrent `read_message()` on the
  stdio, WebSocket or HTTP transports throws `std::logic_error`.
- `Server::run()` waits for in-flight handlers before it returns, and throws
  if the server already has an active session.
- The client closes the connection when the server picks a protocol version
  outside the client's supported list. It advertises the `roots` capability
  on its own once a `roots/list` handler is registered, and
  `set_roots(..., true)` sends `notifications/roots/list_changed` only when
  the client's own `roots.listChanged` capability is set.
- A response is matched to its request by id value and type, as JSON-RPC
  requires: a server that answers a string id `"1"` with the number `1` is
  no longer matched, and the request runs into its timeout.
- `Client::close()` and `~Client()` mark the client closed at once and close
  the transport on the client's strand, so the transport closes when the
  `io_context` next runs rather than inside the call.
- `StdioTransport::read_message()` still returns lines that were read before
  `close()`; it throws only once they are consumed.
- Decoding a JSON-RPC message requires `"jsonrpc": "2.0"`, decoding a content
  block requires the matching `type`, and an error response without an id is
  written with `"id": null`.
- `HttpClientTransport::session_id()` and `last_event_id()` return
  `std::string` by value.
- Documentation guides and feature examples refreshed to match the compiled
  runtime split.
- Less work per request in `StreamableHttpSessionManager` sessions: responses
  are routed by their id without parsing the whole message, the session's
  read loops and response waits end without throwing, and `MemoryTransport`
  writes go straight to the peer's strand. Tool results and typed tool
  registrations are moved rather than copied, and a `tools/list` page is
  serialized from the registered tools without first copying them.
- OAuth protected-resource metadata must carry the RFC 9728 `resource`
  member, and it must identify the configured server (an exact match or an
  origin/segment-boundary prefix); a document that fails either check is
  rejected. Previously any document was accepted.
- `OAuthHttpClient` constructed without a `MetadataFetchPolicy` now refuses
  every request (deny-all default) instead of allowing any target; callers
  must supply an explicit policy.
- `OAuthClientTransport` follows an HTTP `401` into the authorization flow its
  `WWW-Authenticate` challenge names when renewing the access token is not
  possible or the renewed token is refused too, renewing at most once per
  request; a `403` whose challenge carries `error="insufficient_scope"` goes
  straight to that flow. Previously an authentication failure got one refresh
  and one retry. `Authenticator` gains `try_handle_challenge()` and `close()`,
  which changes its layout.

### Fixed

- `HttpClientTransport` opens a new connection after a response with
  `Connection: close`; previously the next request was written to the closed
  connection and failed.
- `HttpServerTransport` no longer takes a request the server sends to the
  client for the response to a client request that has the same id.

### Security

- HTTP session ids for `HttpServerTransport` and
  `StreamableHttpSessionManager` are generated with OpenSSL's `RAND_bytes`
  instead of `std::mt19937`, which is not a cryptographic generator.

## [0.2.0] - TBD

### Added

- Shared and static library variants with installed CMake and pkg-config
  metadata.
- A manually dispatched, fail-closed release pipeline for GitHub Release,
  ConanCenter contribution, APT/DEB, RPM, AUR, Homebrew, and Chocolatey.
- Deterministic source assets, checksums, signatures, SBOMs, provenance, and
  release-ledger support.

### Changed

- The canonical project version is now read from the root `VERSION` file.
- The documented minimum toolchains and dependency versions are explicit.
