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
- `Context` takes a `MessageSender`, so handlers can write to the peer through
  the session's serialized writer. `MemoryTransport` is exported from the
  shared library and is no longer copyable.

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
- A server session enforces the initialization sequence: until
  `notifications/initialized` arrives, every request other than `initialize`
  and `ping` is answered with `g_INVALID_REQUEST`, and a second `initialize`
  within a session is rejected. When `Server::run()` is given an
  `HttpServerTransport` (not a transport that wraps one), a session the client
  ended with `DELETE` is over, and the next client must send `initialize` to
  start a new one.
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
