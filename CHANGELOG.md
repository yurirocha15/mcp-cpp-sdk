# Changelog

All notable changes are documented here. This project follows Semantic
Versioning; a `0.x` version is a stable release unless its version has an
`-rc.N` suffix.

## [Unreleased]

### Added

- `CompiledSchema` (`mcp/protocol/schema.hpp`): compiles a JSON Schema and
  validates values against it, reporting violations with instance paths.
  A schema without `$schema` is read as 2020-12; 2019-09, draft-07, draft-06
  and draft-04 may be named, and any other `$schema` is refused with
  `SchemaError`. The schema itself is checked against its dialect's
  metaschema. `SchemaOptions` bounds the nesting of a validated value
  (64 levels by default), the number of violations reported and the pattern
  matching steps of one validation. JSON cannot
  represent NaN or Infinity: a schema holding one is refused, and a value
  holding one is checked as the `null` it would be sent as.
  References resolve only within the document and to the supported
  metaschemas; any other `$ref` is refused with
  `SchemaError::Category::eUnresolvedRef`, and nothing is fetched. Evaluation
  is bounded in path length, work and pattern matching steps (the latter in
  proportion to the text in the value): a validation that exceeds them reports
  the value as invalid, and a schema that exceeds them for any value, such as
  `{"$ref": "#"}`, is refused when compiled.
- `examples/features/schema_validation.cpp`, which shows each tool schema
  check on the server and the client and exits with a failure status if one
  does not hold.
- Requests of protocol revision 2026-07-28, which carry their protocol version,
  client capabilities and optionally client info and log level in `_meta`, are
  served without `initialize`, alongside a legacy session, by `Server::run()`
  on stdio, WebSocket and in-memory transports and by
  `Server::dispatch_request_direct()`. A version the server does not serve, including a legacy one, is
  answered with `g_UNSUPPORTED_PROTOCOL_VERSION` whose `data` lists the served
  versions (`UnsupportedProtocolVersionData`); a missing or malformed field
  with `g_INVALID_PARAMS`; and a method 2026-07-28 does not have (`ping`,
  `logging/setLevel`, `resources/subscribe`, `resources/unsubscribe`,
  `shutdown`) with `g_METHOD_NOT_FOUND`. These requests never change a
  session's lifecycle or log level: a handler logs only at the request's own
  `io.modelcontextprotocol/logLevel`, and nothing without one, and cannot send
  requests to the client. `notifications/cancelled` cancels any of them that
  was read from the session, and the server then sends nothing more for it,
  not even its response; a 2026-07-28 request given to
  `Server::dispatch_request_direct()` never sends notifications through the
  session's transport. `Context::request_meta()` gives handlers the
  parsed fields (`RequestMetadata`, in `mcp/protocol/request_meta.hpp`, with the
  `g_META_*` key names) and the whole `_meta`.
- `StreamableHttpSessionManager` serves 2026-07-28 requests on the same
  endpoint as legacy sessions. Such a request needs no session: `Mcp-Session-Id`
  and `Last-Event-ID` are ignored and never sent back, and one Server answers
  all of them (`modern_server()`, built from the factory on first use, or
  `set_modern_server()`). Its `MCP-Protocol-Version`, `Mcp-Method` and, for
  `tools/call`, `prompts/get` and `resources/read`, `Mcp-Name` headers must be
  present and match the body (an `Mcp-Name` in the `=?base64?...?=` form is
  decoded first); otherwise it is answered `400` with `g_HEADER_MISMATCH`,
  counted by `header_mismatch_rejections()`. Errors use the statuses the
  specification gives them: `400` for invalid params, unsupported version,
  header mismatch and missing client capability, `404` for an unknown method.
  `GET` and `DELETE` with a version header that `initialize` does not
  negotiate and no session are answered `405`. `initialize` with a 2026-07-28
  header is answered `404` with `g_METHOD_NOT_FOUND`, naming the versions
  served in the message and in `data.supported`. `server/discover` is always
  served this way. `set_protocol_policy()` applies a `ServerProtocolPolicy` to
  every Server the manager builds; without legacy clients, `initialize`
  creates no session and every `GET` and `DELETE` is answered `405`.
- `ServerProtocolPolicy` and `Server::set_protocol_policy()` choose whether a
  server serves 2026-07-28 requests, legacy clients, or both (the default),
  and which legacy versions `initialize` may negotiate. A server serving only
  2026-07-28 answers `initialize` with `g_METHOD_NOT_FOUND` naming the
  versions it serves in the message and in `data.supported`.
  `Server::protocol_rejection_stats()` (`ProtocolRejectionStats`) counts the
  requests refused for their version or `_meta` fields, separately counting
  refusals of requests without a protocol version in `_meta`.
  `ServerProtocolPolicy::validate()` checks a policy, and
  `negotiate_protocol_version(requested, policy)` gives the version
  `initialize` negotiates under it, and `served_protocol_versions(policy)`
  the versions it serves.
- `Server::dispatch_request_direct()` overload taking a `RequestContext`,
  whose `cancelled` flag cancels the request. Direct calls are never cancelled
  by `notifications/cancelled`, so calls that reuse an id stay independent.
- `g_LEGACY_PROTOCOL_VERSIONS` and `g_LATEST_LEGACY_PROTOCOL_VERSION` for the
  revisions that start a session with `initialize` (2025-11-25 and earlier),
  `g_MODERN_PROTOCOL_VERSIONS` for those served per request (2026-07-28), and
  `is_legacy_protocol_version()` and `is_modern_protocol_version()`.
  `negotiate_protocol_version()` and every `initialize`-time version check
  use the legacy list, so `initialize` never selects 2026-07-28.
- Results of 2026-07-28 requests carry `resultType: "complete"` and the
  server's name and version under `_meta`
  (`io.modelcontextprotocol/serverInfo`; `Server::set_report_server_info(false)`
  leaves it out). Results of `server/discover`, `tools/list`, `prompts/list`,
  `resources/list`, `resources/templates/list` and `resources/read` also carry
  the `ttlMs` and `cacheScope` hints that `Server::set_cache_hints()` sets per
  `CacheTarget` as a `CacheHint` (by default `0` and `private`). A member a handler's result
  already has is kept. Results of legacy requests are unchanged.
  `ListToolsResult`, `ListPromptsResult`, `ListResourcesResult`,
  `ListResourceTemplatesResult` and `ReadResourceResult` gain optional `ttlMs`
  and `cacheScope`, and `DiscoverResult::serverInfo` is optional. A
  `cacheScope` value other than `public` or `private` is read as unset.
- `x-mcp-header` tool parameters (2026-07-28). `add_tool()` and
  `add_raw_tool()` refuse, with `SchemaError`, a tool whose input schema has an
  `x-mcp-header` annotation that is not a string, empty, not an HTTP token,
  repeated ignoring case, on a property without a `string`, `integer` or
  `boolean` type, or on a property not reached from the root through
  `properties` alone. `StreamableHttpSessionManager` passes a 2026-07-28 request's
  `Mcp-Param-*` headers to the server (`RequestContext::param_headers` for a
  direct dispatch), and a `tools/call` is answered `400` with
  `g_HEADER_MISMATCH` unless each annotated argument that is present and not
  null has exactly one header matching it once decoded: integers compare
  numerically and must lie within ±(2^53 − 1). A header the tool names with
  characters a header may not carry, or with a malformed Base64 sentinel, is
  refused the same way even when its argument is absent. A 2026-07-28 request that
  repeats its `MCP-Protocol-Version`, `Mcp-Method` or `Mcp-Name` header is
  also answered `g_HEADER_MISMATCH`.
- `HttpServerTransport` serves 2026-07-28 requests alongside its legacy
  session, with the same header checks, error statuses and `405` for a
  sessionless `GET` or `DELETE` as `StreamableHttpSessionManager`, counted by
  its own `header_mismatch_rejections()`. Each is carried under a
  transport-private id, so clients may reuse ids, and is answered with plain
  JSON that never carries `Mcp-Session-Id` and never enters the replay store;
  notifications a handler sends while serving one are dropped. Ending the
  legacy session does not end them. At most 256 may wait at once; more are
  answered `503`. On both HTTP transports, a request whose
  `MCP-Protocol-Version` header names a version that `initialize` does not
  negotiate (2026-07-28 or any other) but whose `_meta` names no version is
  answered `400` with `g_INVALID_PARAMS`, and an `initialize` with a
  2026-07-28 header is answered `404` with `g_METHOD_NOT_FOUND` naming the
  versions the server serves.
- `ITransport::http_server_transport()`, which a transport wrapping an
  `HttpServerTransport` overrides to return it, so that `Server::run()` serves
  the wrapper as an `HttpServerTransport`: it ends sessions on `DELETE`,
  checks `Mcp-Param-*` headers and drops the notifications of 2026-07-28
  requests. It returns null by default. Adding it changes the layout of
  `ITransport`'s virtual table.
- `StreamableHttpSessionManager` streams the answer to a 2026-07-28 request:
  when a handler sends a notification (a log message at the request's own
  `logLevel`, or progress) and the client accepts `text/event-stream` over
  HTTP/1.1, the answer becomes a chunked SSE stream without event ids that
  carries the notifications and then the response, with
  `X-Accel-Buffering: no` and periodic keep-alive comments. Otherwise it is
  plain JSON as before, except that a client that accepts SSE and half-closes
  its connection gets a stream too, and the notifications are dropped. A
  client that resets its connection while its request runs cancels the
  request, before or after a half-close; after a half-close the stream is
  probed with comments until a write fails, which cancels it too, and
  closing the manager cancels it. Requests pipelined before a half-close are
  all answered, and the connection then ends. `set_stream_options()` (`StreamOptions`) sets the keep-alive
  and probe intervals and the queued-notification limit, and
  `stream_stats()` (`StreamStats`) counts streams, cancellations, probes and
  dropped notifications. Bytes of a pipelined request that arrive while a
  request runs are kept for the next one. `RequestContext::notifications`
  receives a 2026-07-28 request's notifications on a direct dispatch.
- `Context::principal()` says who sent a request. Over HTTP it is the
  principal a bearer token validator names in its new `TokenValidation`
  result, or else `h:` and the hex SHA-256 of the request's `Authorization`
  header, or empty without one; the requests of a
  `StreamableHttpSessionManager` session carry none, and stdio, WebSocket and
  memory transports give none. `ITransport::read_incoming()` returns a
  message with its sender (`IncomingMessage`); `HttpServerTransport` keeps
  each queued message with its own sender, and `RequestContext::principal`
  sets it for a direct dispatch. A transport that wraps another must forward
  `read_incoming()` as well as `read_message()`, or its messages arrive
  without a principal.

### Changed

- `BUILD_TESTING`, `BUILD_EXAMPLES` and `BUILD_DOCS` default to `ON` only when
  the SDK is the top-level CMake project. Added with FetchContent or
  `add_subdirectory`, the SDK no longer builds its tests and examples or needs
  GTest; setting an option still overrides the default.
- `server/discover` is a 2026-07-28 request: without
  `io.modelcontextprotocol/protocolVersion` in `_meta` it is answered with
  `g_INVALID_PARAMS`.
- `StreamableHttpSessionManager` answers `server/discover` from
  `modern_server()`, like every 2026-07-28 request, instead of from a separate
  server, and `HttpServerTransport` counts a waiting discover against the 256
  waiting 2026-07-28 requests instead of a limit of 16 for discovers.
- `initialize` negotiation uses `g_LEGACY_PROTOCOL_VERSIONS` instead of
  `g_SUPPORTED_PROTOCOL_VERSIONS`, which keeps its value.
- Before `notifications/initialized`, a session serves a request that carries
  a 2026-07-28 protocol version in `_meta` instead of answering it with
  `g_INVALID_REQUEST`; `server/discover` is exempt from the initialization
  sequence only as such a request.
- `BearerTokenValidator` returns a `TokenValidation` and
  `AsyncBearerTokenValidator` a `Task<TokenValidation>`. A synchronous
  validator returning `bool` still compiles; an asynchronous one returning
  `Task<bool>` must change its return type (`co_return true;` still works).
- `tools/call` naming an unknown tool and `prompts/get` naming an unknown
  prompt are answered with `g_INVALID_PARAMS` (`-32602`), as the tools and
  prompts specifications list, instead of `g_METHOD_NOT_FOUND` (`-32601`).
- `Server::add_tool()` and `add_raw_tool()` compile the tool's `inputSchema`
  and `outputSchema` and throw `SchemaError` when either is not a JSON Schema
  object that `CompiledSchema` accepts, or when the input schema does not
  declare `"type": "object"`, which the 2025-11-25 and 2026-07-28 schemas
  require. Previously any JSON value was advertised as the schema.
- `tools/call` arguments are validated against the tool's input schema, as
  the 2025-11-25 and 2026-07-28 revisions require of servers. Arguments that
  do not conform are answered with an `isError` result listing the
  violations, and the handler does not run; middleware still sees the call.
  The check also applies to `Server::invoke_tool()`. `Server::set_tool_validation()` with
  `ToolValidationOptions::validate_arguments = false` turns it off, and
  `Server::tool_validation_stats()` counts refused calls.
- The `structuredContent` of a successful tool result is checked against the
  tool's `outputSchema`, which the 2025-11-25 and 2026-07-28 revisions require
  it to conform to. A result that does not conform is answered with
  `g_INTERNAL_ERROR` naming the violations
  (`ToolValidationOptions::validate_structured_content` turns the check off).
- `Client::list_tools()` compiles each listed tool's schemas and leaves out a
  tool whose schema cannot be used (invalid, an unsupported dialect, or a
  `$ref` outside the document), reporting it through
  `ClientOptions::on_tool_schema_error`. `Client::call_tool()` checks the
  `structuredContent` of a successful result against the tool's listed
  `outputSchema` and throws `StructuredContentError` when it does not conform.
  `ClientOptions::validate_tool_schemas = false` turns both off.
- `notifications/cancelled` marks any request the session is still serving as
  cancelled, so `Context::is_cancelled()` turns true in resource and prompt
  handlers too, not only in tools. The response of a request of
  2025-11-25 or earlier is still sent.

### Removed

- `Server::set_discover_ttl_ms()` and `set_discover_cache_scope()`, replaced
  by `Server::set_cache_hints()`.

### Fixed

- With `BUILD_DOCS=ON` and Doxygen installed, configuring a project that adds
  the SDK as a subproject failed: the SDK looked for `docs/Doxyfile.in` in the
  parent project's source directory.
- The SDK builds with Boost 1.74 and 1.75 (Ubuntu 22.04, Debian 12, RHEL 9),
  the documented minimum, and with Boost 1.87 and later, which removed the
  Asio timer overloads it called.
- With Boost 1.75 and older, an OAuth metadata response larger than
  `max_response_bytes` was read in full instead of being refused.
- Built with GCC 12 (Debian 12), the auth middleware hung and
  `OAuthClientTransport` reads crashed: GCC 12.2 miscompiles a `co_await`
  inside an `if` condition, and the SDK no longer writes one.
- With Boost 1.77 to 1.84, cancelling the coroutine awaiting an SDK
  operation, such as an OAuth HTTP request, could end the process: Asio's
  `co_spawn()` threw from its own completion.
- With Boost older than 1.82, an error the shared library threw with a Beast
  error code, such as a WebSocket connect timeout, did not compare equal to the
  same `boost::beast` enumerator in the caller.

## [0.2.0] - 2026-10-21

### Added

- Shared and static library variants with installed CMake and pkg-config
  metadata.
- A manually dispatched, fail-closed release pipeline for GitHub Release,
  ConanCenter contribution, APT/DEB, RPM, AUR, Homebrew, and Chocolatey.
- Deterministic source assets, checksums, signatures, SBOMs, provenance, and
  a signed release manifest anchored in an immutable GitHub Release.
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
  a secret bound to no issuer is never presented, and a config that sets the
  `client_id` shorthand alongside `client_identity.pre_registered` is refused
  with `std::invalid_argument`. The built-in client has no
  TLS support, so in this release it can reach only an authorization server
  on a loopback address. It renews a grant only after it has authorized in
  the same process; a refresh token loaded from a persistent `TokenStore`
  after a restart is not used. Its `close()` aborts the work in flight and
  refuses later attempts; a challenge on a closed manager fails when the
  returned task is awaited, not when `try_handle_challenge()` is called.
- `MetadataFetchPolicy`, which every `OAuthHttpClient` request passes: allowed
  and denied origins, an `origin_allowance` hook, address classification that
  refuses private, link-local and reserved targets and loopback unless
  `allow_plain_http_loopback` is set (an IPv4-mapped, NAT64 or 6to4 IPv6
  address is judged by the IPv4 address it carries), and redirect and
  response-size limits.
  A `denied_origins` entry that is not a bare origin is rejected with
  `MetadataPolicyError` (`denied_origin_entry_malformed`), and every target is
  refused until it is corrected. `OAuthHttpClient::set_metadata_policy()` and
  `set_host_resolver()` configure a client, and `configure()` installs both as
  one change, so no request can run with the new resolver and the old policy.
  A request keeps the policy and resolver it started with across every
  redirect hop.
- `OAuthHttpClient::make_scope()` and `OAuthHttpClientScope`, a view of a
  shared client whose requests `abort()` ends without touching other scopes or
  the client's own requests. `abort_pending()` still ends everything.
- A `discover_protected_resource()` overload taking a
  `ProtectedResourceAcceptor`, called with the document before it is cached or
  returned, including on a cache hit. Throwing rejects the document, and
  nothing is cached.
- `parse_www_authenticate()`, `select_bearer_challenge()` and
  `BearerChallenge` for reading `WWW-Authenticate` headers.
- The `server/discover` method, answered over HTTP before and after a session
  exists, with `Server::set_instructions()`, `set_discover_ttl_ms()` and
  `set_discover_cache_scope()`, and the `DiscoverRequest`, `DiscoverResult`
  and `CacheScope` types. `StreamableHttpSessionManager` answers sessionless
  discovers from one server built on the first, and `HttpServerTransport`
  answers `503` to a sessionless discover while 16 are already waiting.
- `AuthorizationRequest`, `AuthorizationResponse` and
  `parse_authorization_response()` for the consent callback, and the client
  identity types `OAuthClientMetadata`, `OAuthClientInformation`,
  `ClientIdentityConfig` and `InMemoryClientCredentialStore`.
- `OAuthHttpClient::post_json()` and `abort_pending()`, an
  `OAuthDiscoveryClient::discover_protected_resource()` overload that takes
  the `resource_metadata` URL from a challenge,
  `OAuthConfig::token_endpoint_auth_method`, and the
  `authorization_response_iss_parameter_supported`,
  `client_id_metadata_document_supported` and
  `token_endpoint_auth_methods_supported` members of `AuthServerMetadata`.
  The new members change the layout of `OAuthConfig` and `AuthServerMetadata`.
- The conformance harness runs the client authorization scenarios, and all 14
  of them pass.
- `StdioTransport::create_owning_stdout()`, which writes the protocol to a
  private copy of the process's standard output and points standard output at
  standard error while the transport lives, so stray output from the
  application or a dependency cannot corrupt the message stream. With standard
  error closed, standard output points at the null device instead. Standard
  output is restored when the transport is destroyed, and only one such
  transport may exist at a time. A process launched with standard error sent
  to the same place as standard output (`2>&1`) is not protected.
- CI compiles the README code snippets and checks that the stdio examples
  write nothing but protocol messages to standard output.
- Server-side OAuth challenge support on `HttpServerTransport` and
  `StreamableHttpSessionManager`. `set_bearer_challenge()` sets the parameters
  sent in the `WWW-Authenticate` header of every 401, rendered as RFC 7235
  quoted-strings in the order `realm`, `error`, `scope`, `resource_metadata`;
  `set_protected_resource_metadata()` serves an RFC 9728 protected-resource
  metadata document, answered without an `Authorization` header so a client
  holding no token can read it; and `set_unauthenticated_paths()` exempts
  request paths from the bearer check while also removing them from MCP
  dispatch, so an exempt path that no other route claims is answered
  `404 Not Found` rather than serving MCP without authentication. A challenge
  that carries no `resource_metadata` is filled in with the metadata
  document's URL. The document's path defaults to the RFC 9728 3.1 derivation
  — a resource at `https://host/mcp` is described at
  `/.well-known/oauth-protected-resource/mcp` — and its URL is built from the
  configured `resource`, never from the address the listener is bound to.
  `<mcp/transport/http_types.hpp>` gains `BearerChallengeConfig`,
  `ProtectedResourceMetadataConfig`, `format_www_authenticate()`,
  `protected_resource_metadata_path()`, `protected_resource_metadata_url()`,
  `format_protected_resource_metadata()` and `http_request_path()`.
- `set_async_bearer_token_validator()` on `HttpServerTransport` and
  `StreamableHttpSessionManager`, taking the new `AsyncBearerTokenValidator`
  (`std::function<Task<bool>(std::string)>`), so a token decision that needs
  I/O — introspection, a JWKS fetch — suspends instead of blocking the
  executor serving MCP traffic. Installing it alongside the synchronous
  validator throws `std::logic_error`. A validator of either kind that throws
  is answered `500 Internal Server Error` without a challenge, so a failing
  introspection endpoint does not send clients into re-authorization.
- `set_max_request_body_bytes()` on `HttpServerTransport` and
  `StreamableHttpSessionManager`, together with
  `mcp::constants::g_default_max_request_body_bytes` (8 MiB), the new default.
  A body over the cap is answered `413 Payload Too Large` and its connection
  closed before MCP dispatch; zero is rejected with `std::invalid_argument`.
  The default replaces Beast's own 1 MB limit, which admitted only about
  750 KB of raw content once base64 inflation inside the JSON body is
  accounted for. The body is read up to the cap before the bearer token is
  checked, and parsed as JSON only after the origin and bearer checks pass.
- `ClientOptions::on_protocol_error`, invoked when the client discards an
  incoming message instead of dispatching it: a message the peer sent that
  could not be decoded, reported as `g_PARSE_ERROR`, or an exception thrown by
  an application notification callback, reported as `g_INTERNAL_ERROR` with
  the notification method in the message. It runs on the read loop and must
  not block.
- A build check that every protocol type with a `from_json` is either covered
  by the peer-input matrix test (`test/core/json_peer_input_matrix_test.cpp`)
  or excluded with a stated reason, and that the test matches what
  `scripts/gen_json_matrix.py` generates. It exits 2 for a row whose decoder
  now throws on an explicit null or an absent key, and the generator refuses
  to write such a row unless it is named with `--accept-regression`. The
  check is governed by the new `MCP_CPP_SDK_CHECK_JSON_MATRIX` option: `ON` in
  a checkout, where a `BUILD_TESTING=ON` build needs Python 3.9+, and `OFF` in
  source archives, which do not ship the scripts.
- Documentation for the server-side OAuth challenge work: the OAuth guide now
  names the protected-resource metadata helpers and the exported bearer and
  request-path helpers, and renders its example challenge through the
  server-side API rather than hand-writing the header. The client guide now
  covers the default 30-second request timeout, including that the typed
  helpers such as `call_tool()` and `read_resource()` accept no
  per-request override, that a request outliving it fails with
  `g_REQUEST_TIMEOUT` while the peer may still be running it, and that no
  value disables the deadline.
- `python scripts/build.py --tsan --test` builds and runs the tests under
  ThreadSanitizer (CMake option `ENABLE_TSAN`), and `--msan --test` under
  MemorySanitizer, against a libc++ and dependencies it builds instrumented.
  `--compiler clang` compiles the AddressSanitizer and ThreadSanitizer builds
  with Clang. CI runs all of them on pushes to `main` and on pull requests
  into it, and the AddressSanitizer run now also checks for stack use after
  return.
- `WebSocketClientTransport` takes a `connect_timeout` (default 30 seconds)
  that bounds the TCP connect and the WebSocket handshake together. A peer
  that accepts the connection and then stalls now fails the pending call with
  a timeout instead of holding it forever. Zero, a negative value, or one too
  long for the clock to represent, such as `std::chrono::milliseconds::max()`,
  disables the limit.

### Changed

- The canonical project version is now read from the root `VERSION` file.
- The documented minimum toolchains and dependency versions are explicit.
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
  the message is dropped and reported through `ClientOptions::on_protocol_error`.
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
  `notifications/initialized` arrives, every request other than `initialize`,
  `ping` and `server/discover` is answered with `g_INVALID_REQUEST`, and a
  second `initialize` within a session is rejected. When `Server::run()` is given an
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
  included, is answered with a tool error result (`isError: true`) carrying
  the exception message. Previously only the synchronous JSON `add_tool()`
  overload did this, and other tool handlers' exceptions were answered with
  `-32603`. Resource and prompt handler exceptions are still answered with
  `-32603`.
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
  must supply an explicit policy. No policy lets it reach a host other than
  a loopback address: it refuses plain `http` to any other host and has no
  TLS support for `https`, so token refresh against such a host no longer
  works.
- `OAuthClientTransport` follows an HTTP `401` into the authorization flow its
  `WWW-Authenticate` challenge names when renewing the access token is not
  possible or the renewed token is refused too, renewing at most once and
  authorizing for a `401` at most once per request; a `403` whose challenge
  carries `error="insufficient_scope"` goes straight to that flow. Previously an authentication failure got one refresh
  and one retry. `Authenticator` gains `try_handle_challenge()`,
  `cancel_pending()` and `close()`, which changes its layout.
- Closing an `OAuthClientTransport` cancels the authorization work its
  authenticator has in flight, through `cancel_pending()`, and leaves the
  authenticator usable, so a new transport over the same `OAuthAuthenticator`
  or `OAuthAuthorizationManager` can still refresh and authorize. A write still
  retrying on the closed transport fails without asking the authenticator for a
  new refresh or authorization; one it started while the transport was closing
  is cancelled through `cancel_pending()` and waited for. Calling `close()` on
  the authenticator itself is permanent.
- `OAuthAuthenticator` rejects a null `OAuthHttpClient` with
  `std::invalid_argument` when constructed, and its `close()` ends only the
  requests it issued, leaving the client usable by anyone else sharing it.
- Challenges that arrive while `OAuthAuthorizationManager` is already
  authorizing wait for that attempt, and each of them reports its outcome,
  rethrowing the same exception when it fails. Refreshes that overlap share
  one refresh grant in the same way, and only the refresh that leads it reads
  the refresh token, so none sends a token another refresh already replaced in
  the store.
- `scripts/build.py --sanitize --test` runs with LeakSanitizer enabled.
- An optional member serialized as an explicit `null` is now read the same way
  as an absent one. The protocol `from_json` overloads test presence through
  the new `detail::has_json_value` helper instead of `contains()`, which
  previously accepted the null and then threw when the value was extracted;
  this covers `Error::data`, `RelatedTaskMetadata::title`, `TaskMetadata`'s
  `ttl` and `relatedTasks`, request and notification `params`, response `id`
  and `error`, and the notification `_meta`, `reason`, `total`, `message`,
  `logger` and `metadata` members. An `Error` whose `message` is missing or
  null now decodes to an empty message rather than throwing, so the `code` a
  caller acts on survives. The server reads a null `error` member as absent
  when validating request, notification and response envelopes and when
  dispatching a response, and a null notification `params` as no params. The
  client is asymmetric on purpose: a null `error` is the absence of an error,
  while a null `result` is a legitimate empty result and still counts as
  present. The same treatment covers the optional members of the protocol
  types a peer's payload reaches: `description`, `mimeType`, `size`, `title`
  and `icons` on `Resource` and `ResourceTemplate`; `description`, `title`,
  `icons` and `execution` on `Tool`; and `isError` on both `CallToolResult`
  and `ToolResultContent`.
- A peer whose serializer writes absent optionals as explicit nulls could not
  complete initialization at all. `clientInfo` is an `Implementation`, whose
  `from_json` guarded `title`, `description`, `websiteUrl` and `icons` with a
  bare presence test, so `"clientInfo": {"name": "x", "version": "1",
  "title": null}` failed to decode and the `initialize` request was answered
  `-32602`. Because `initialize` is the peer's first message, the failure was
  unconditional: no such client could connect at all. Those members are now
  read as absent when they arrive as null.
- Explicit nulls in request parameters that previously drew `-32602` are now
  read as absent: `arguments` on `prompts/get`, `context.arguments` on
  `completion/complete`, and the pagination `cursor`, where `"cursor": null`
  on a list request was rejected as an invalid cursor instead of returning the
  first page. `CallToolParams::arguments` is deliberately unchanged: it is a
  required member carrying a shape rule rather than an optional one, so
  rejecting a null there remains correct.
- An explicit null `_meta` or `annotations` no longer decodes to a value the
  peer never sent. Unlike the members above these never threw: the guard
  accepted the null and produced an engaged optional, so the SDK re-emitted
  `"_meta": null` and turned `"annotations": null` into `"annotations": {}` —
  a structurally present `Annotations` that a consumer reads as "annotations
  supplied, carrying no constraints" rather than as none at all. Neither side
  saw anything wrong, which is what made this worse than a rejection. The
  affected members are `_meta` and `annotations` on `Resource`,
  `ResourceTemplate`, `Tool`, `CallToolResult` and `ToolResultContent`,
  `Tool::outputSchema`, `structuredContent` on `CallToolResult` and
  `ToolResultContent`, and `CallToolParams::_meta` on the `tools/call` path.
- The remaining optional members of the protocol types read an explicit null
  as absent too: the capability flags (`listChanged`, `subscribe` and
  `extensions` on both sides), `cursor` and `nextCursor` on every list request
  and result, and the optional members of `Annotations`, `Icon`, `Prompt`,
  `PromptArgument`, `ResourceLink`, `Root`, `ToolAnnotations`, the sampling and
  elicitation types, completion results and tasks. Before this, `initialize`
  was answered `-32602` when a client sent `"roots": {"listChanged": null}`,
  and `connect()` or a `list_*` call threw when a server sent
  `"listChanged": null` or `"nextCursor": null`.
- A `resources/read` URI longer than 512 characters is now answered
  `-32602` naming the limit instead of being matched against the resource
  templates. Exact resource lookups happen first and are unaffected by the
  limit.
- Resource templates are matched without `std::regex`, in time linear in the
  URI's length. A URI that nearly matches a template whose expressions are
  separated by a literal their values may also contain, such as
  `metrics://{year}-{month}-{day}-{hour}`, is answered at once instead of
  holding the server's executor while a backtracking matcher tries every split.
- Messages from a peer that nest arrays and objects more than 128 levels deep
  are refused before they are decoded: the server answers `-32700`, the client
  drops the message and reports it through `ClientOptions::on_protocol_error`,
  and the HTTP transports answer `400`. Copying a decoded document recurses
  once per level, so a request of a few hundred kilobytes made of nothing but
  brackets could exhaust the stack and crash the process.
- A resource URI template with two adjacent expressions is rejected at
  registration with `std::invalid_argument` when the boundary between them is
  ambiguous: the second expression has no leading literal (`{a}{b}`,
  `{a}{+b}`), or its literal can occur in the first expression's value
  (`{name}{.ext}`, `{+path}{/segment}`). Any split of a URI between two such
  expressions would match, so their values would be ambiguous. Adjacent
  expressions with a decidable boundary, such as
  `{+path}{?query}` and `{id}{#section}`, are accepted.
- A tool handler that throws something not derived from `std::exception` is
  answered with a tool error result carrying the fixed text
  `Tool handler failed`, and every tool exception message is flattened and
  length-bounded before it reaches the peer. Such a throw from a resource or
  prompt handler, or from middleware, still ends the request without a
  response.
- Middleware runs outside the tool handler's exception guard. An exception
  thrown by middleware is answered with `-32603` for every request kind,
  `tools/call` included, since middleware decides whether a call may proceed
  at all. A tool handler's exception reaches middleware as an `isError`
  result rather than as an exception. `make_auth_middleware()` answers a
  missing or refused token with a tool error result on `tools/call` and with
  `-32603` on `resources/read` and `prompts/get`, without invoking the
  handler.
- An incoming message the client cannot decode no longer ends the session.
  Only a failure of `read_message()` — the peer hung up, the socket died, the
  client was closed — fails pending requests and closes the transport;
  anything that goes wrong after the bytes are off the wire is reported
  through `ClientOptions::on_protocol_error` and the message is dropped.
- An explicit `ProtectedResourceMetadataConfig::path` is now validated rather
  than concatenated onto the origin verbatim. A path that does not begin with
  `/` is rejected with `std::invalid_argument`, as is one carrying a `.` or
  `..` whole segment. Previously `path = "evil"` produced `https://hevil` — a
  corrupted authority rather than merely a bad path — and `path = "/../../x"`
  was published unresolved. Only whole segments count, so a path such as
  `/.well-known/a..b/c.d` is still accepted; dots inside a segment are
  ordinary characters, and refusing them would refuse the well-known prefix
  the derivation itself produces. The function does not normalize, and its
  result is published to clients as the authoritative location of the
  document, so an unusable path is refused rather than advertised. The
  transport and session-manager setters reject it at configuration time and
  leave the server unchanged.
- A `Server` may now be destroyed while a session started with `run()` still
  has handlers in flight. The implementation outlives the `Server` until that
  work finishes, so such a handler no longer runs against freed state. Work
  started through `dispatch()`, `dispatch_request_direct()` or the
  `notify_*()` calls does not extend it, so the `Server` must outlive those. A
  reverse request issued from a handler whose `Server` is gone throws
  `std::runtime_error` naming that cause instead of faulting — distinct from
  the failure reported for a session that is merely closing. Destroying a
  `Server` from a thread other than the one running its session no longer
  races on that session's request maps either: those entries are abandoned on
  the session strand, which is the only executor permitted to touch them.
- A session is now unregistered even when teardown throws — a transport that
  throws from `close()`, or a drain wait that fails for any reason other than
  cancellation. Previously such a failure left the session registered and
  every later `run()` was refused for the lifetime of the `Server`.
- Closing an OAuth client transport from a thread that does not run the
  `io_context` could be lost while a metadata lookup was resolving: the close
  found no socket to shut, and the flow went on to connect and blocked until
  the HTTP timeout. The abort latch is now checked again just before the
  connection is opened. `OAuthHttpClient` also keeps every request on the
  client's one strand, so this holds when several threads run the `io_context`.
  A request used to leave that strand at its first suspension, so a close could
  run on another thread at the same time: a data race on the socket that could
  crash, or a close lost in the instant before the socket opened. Callers are
  still resumed on their own executor and never on the client's strand,
  including a caller that has been cancelled. An installed `HostResolver` is
  called on that strand too, so one that blocks holds up every other request
  on the client and the closes that end them; it must return promptly.
- Closing `HttpClientTransport` or `WebSocketClientTransport` from a thread
  that does not run the `io_context` could be lost while the transport was
  resolving its server's address: the close found no socket to act on, and the
  transport went on to connect. The HTTP write blocked until the HTTP timeout,
  and the WebSocket connect waited on its handshake with no limit. Both
  transports now check for the close again once the address is known and fail
  the pending call instead of connecting. For `WebSocketClientTransport` this
  holds however many threads run the `io_context`.
- `HttpClientTransport::close()` now closes the socket of a write that is in
  flight instead of cancelling its pending operation once. A close that arrived
  between two socket operations, or just after one had completed, cancelled
  nothing and the write carried on until the HTTP timeout; with several threads
  running the `io_context` that included a close racing the connect. The write
  now fails promptly in each of these cases, however many threads run the
  `io_context`, and reports `operation_aborted` on every platform, whatever the
  closed socket itself reported. The session `DELETE` that `close()` sends
  afterwards uses a new connection. It already did after a cancelled write;
  when the response had already arrived as `close()` ran, it used to reuse the
  write's connection and now opens a new one as well.

### Fixed

- `HttpClientTransport` opens a new connection after a response with
  `Connection: close`; previously the next request was written to the closed
  connection and failed.
- `HttpServerTransport` no longer takes a request the server sends to the
  client for the response to a client request that has the same id.
- `HttpClientTransport` and `OAuthHttpClient` no longer end the process when
  a request's 30-second deadline passes between two of its socket operations,
  for example while the io thread is busy as the response arrives; Boost.Beast
  1.86 aborts when an operation starts after its stream's deadline. The
  deadline now closes the socket, and the request fails with
  `boost::beast::error::timeout`.

### Security

- HTTP session ids for `HttpServerTransport` and
  `StreamableHttpSessionManager` are generated with OpenSSL's `RAND_bytes`
  instead of `std::mt19937`, which is not a cryptographic generator.
- Text a peer controls (URLs, hosts, issuers, metadata fields, response
  bodies) is cleaned before it appears in an OAuth exception message: cut to
  256 bytes, with line breaks and bidirectional controls replaced by spaces and
  ill-formed UTF-8 replaced by `?`, so it cannot forge or reorder log lines.
  The helper is `mcp::detail::sanitize_for_diagnostics()`.
