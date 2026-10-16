# Changelog

All notable changes are documented here. This project follows Semantic
Versioning; a `0.x` version is a stable release unless its version has an
`-rc.N` suffix.

## [Unreleased]

### Added

- Secure random generation and serialized transport-write helpers under
  `mcp::detail`.

### Changed

- Implementation moved out of oversized headers into compiled translation
  units (OAuth, client runtime, protocol tools, memory transport, HTTP
  types); protocol models and typed handler templates remain header-based.
- Client requests now time out after 30 seconds by default and fail with
  `g_REQUEST_TIMEOUT` (`ClientOptions::request_timeout`, or
  `RequestOptions::timeout` for one `send_request()` call). Previously a
  request waited until the peer answered.
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
  is rejected.
- Tool results are always sent as a `CallToolResult`. The value returned by a
  JSON tool handler, or by a typed handler whose result type is not
  `CallToolResult`, is wrapped: an object becomes `structuredContent` with its
  serialization as the text block, a string becomes the text block, and any
  other value is serialized into the text block. Previously the value was sent
  as the result unchanged. `add_raw_tool()` handlers must return a valid
  `CallToolResult`, and a tool with an `outputSchema` must return
  `structuredContent`.
- Decoding a JSON-RPC message requires `"jsonrpc": "2.0"`, decoding a content
  block requires the matching `type`, and an error response without an id is
  written with `"id": null`.
- `HttpClientTransport::session_id()` and `last_event_id()` return
  `std::string` by value.

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
