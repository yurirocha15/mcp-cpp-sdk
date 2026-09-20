# Changelog

All notable changes are documented here. This project follows Semantic
Versioning; a `0.x` version is a stable release unless its version has an
`-rc.N` suffix.

## [Unreleased]

### Added

- Fixtures and a pinned harness for the official MCP conformance runner
  (`@modelcontextprotocol/conformance@0.1.16`, spec revision `2025-11-25`),
  with a regression baseline in `conformance/expected-failures.yml`, a
  `--conformance` build flag, and a CI workflow that fails on any drift.
- Secure random generation and serialized transport-write helpers under
  `mcp::detail`.
- Reproducible cross-SDK benchmark tooling (order counterbalancing,
  environment/container capture, resource-headroom validation, protocol
  verification) and audited benchmark results.
- Project policy documents: `ROADMAP.md` (tier gates), `MAINTENANCE.md`
  (triage SLAs), `VERSIONING.md` (compatibility surface), and
  `DEPENDENCY_POLICY.md` (runtime dependency floors).
- GitHub issue templates and a label manifest (`.github/labels.yml`).

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
- Documentation guides and feature examples refreshed to match the compiled
  runtime split.
- OAuth protected-resource metadata that omits the RFC 9728 `resource`
  member is now rejected instead of falling back to the configured server
  URL; a `resource` value must identify the configured server (exact match
  or an origin/segment-boundary prefix).
- `OAuthHttpClient` constructed without a `MetadataFetchPolicy` now refuses
  every request (deny-all default) instead of allowing any target; callers
  must supply an explicit policy.
- A `MetadataFetchPolicy::denied_origins` entry that is not a bare origin — it
  carries a path (a lone trailing `/` included), a query or a fragment, or its
  port is not a plain in-range decimal number — is now rejected instead of
  silently ignored. `validate_metadata_url` throws `MetadataPolicyError` with
  the new `MetadataUrlDecision::denied_origin_entry_malformed`, naming the
  offending entry, and refuses every target until the policy is corrected.
  Previously such an entry denied nothing, so `denied_origins` of
  `{"https://evil.example/"}` admitted `https://evil.example`. A configuration
  that relied on that silence is now a hard failure. `allowed_origins` is
  unchanged: an entry that is not a bare origin still matches nothing, because
  dropping an allow entry grants nothing and so fails closed.

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
