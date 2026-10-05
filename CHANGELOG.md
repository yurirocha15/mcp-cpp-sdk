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
  validator throws `std::logic_error`.
- `set_max_request_body_bytes()` on `HttpServerTransport` and
  `StreamableHttpSessionManager`, together with
  `mcp::constants::g_default_max_request_body_bytes` (8 MiB), the new default.
  A body over the cap is answered `413 Payload Too Large` and its connection
  closed before MCP dispatch; zero is rejected with `std::invalid_argument`.
  The default replaces Beast's own 1 MB limit, which admitted only about
  750 KB of raw content once base64 inflation inside the JSON body is
  accounted for.
- `ClientOptions::on_protocol_error`, invoked when the client discards an
  incoming message instead of dispatching it: a message the peer sent that
  could not be decoded, reported as `g_PARSE_ERROR`, or an exception thrown by
  an application notification callback, reported as `g_INTERNAL_ERROR` with
  the notification method in the message. It runs on the read loop and must
  not block.
- Documentation for the server-side OAuth challenge work: the OAuth guide now
  names the protected-resource metadata helpers and the exported bearer and
  request-path helpers, and renders its example challenge through the
  server-side API rather than hand-writing the header. The client's default
  30-second request timeout is documented for the first time, including that
  the typed helpers such as `call_tool()` and `read_resource()` accept no
  per-request override, that a request outliving it fails with
  `g_REQUEST_TIMEOUT` while the peer may still be running it, and that no
  value disables the deadline.

### Changed

- Implementation moved out of oversized headers into compiled translation
  units (OAuth, client runtime, protocol tools, memory transport, HTTP
  types); protocol models and typed handler templates remain header-based.
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
- Injected OAuth client credentials that carry a `client_secret` must now name
  the authorization server they are bound to, via the new
  `OAuthAuthorizationConfig::client_issuer` or
  `ClientIdentityConfig::pre_registered`'s `issuer`. A secret that names no
  issuer is refused at the point of use: `select_client_identity` returns
  `ClientIdentityDecision::unavailable` and the authorization attempt fails
  with a message naming the missing binding, rather than presenting the secret.
  Previously an empty `issuer` fell through to `use_pre_registered` for every
  authorization server, so the existing misbinding guard was inert on both
  paths the SDK itself constructs, and a hostile-but-policy-allowed
  authorization server named in a protected-resource document received the
  application's secret at its token endpoint. "Bound to no issuer" is not
  "bound to every issuer". Construction is unchanged, so a caller breaks only
  when it actually attempts the affected flow; a public client (a `client_id`
  with no secret) is unaffected and still authorizes against any issuer.
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
- A `resources/read` URI longer than 512 characters is now answered
  `-32602` naming the limit, instead of being handed to the regular-expression
  template matcher, whose stack use grows with the subject and can overrun the
  smallest thread stack the SDK runs on. Exact resource lookups happen first
  and are unaffected by the limit.
- A resource URI template with two expressions and no literal between them
  (`{a}{b}`) is now rejected at registration with `std::invalid_argument`.
  Such a template compiled to adjacent unbounded runs that the matcher could
  only resolve by backtracking over every split of the input, and the boundary
  between the two variables is undecidable in any case.
- An exception from a tool handler is reported as a `CallToolResult` carrying
  `isError`, with the message sanitized before it reaches the peer, and the
  guard now covers a throw that does not derive from `std::exception`. Such a
  throw previously escaped every guard on the request path — the tool
  invocation caught only `std::exception`, and `dispatch_request_wire` has no
  catch-all — so no response was written at all: a session client waited out
  its own request timeout, and a stateless connection was closed with nothing
  sent. It is now reported as a tool error with the fixed text
  `Tool handler failed`, because such a throw carries no message the SDK can
  quote. This is the behavior the error-handling guide already documented for
  handler exceptions, "typed, asynchronous or raw". The change is specific to
  tool handlers: a throw that does not derive from `std::exception` escaping a
  resource or prompt handler, or middleware, still drops the response.
- Middleware now runs outside the tool handler's exception guard, so a
  middleware that throws surfaces as a JSON-RPC error (`-32603`) rather than
  as a tool error result; middleware decides whether a call may proceed at
  all, which is a protocol-level answer rather than a tool outcome. Previously
  any exception from the middleware chain became a tool error result. The
  bundled auth middleware no longer throws on a missing or invalid bearer
  token — it returns a tool error result — so that rejection still reaches
  the caller as a tool result rather than becoming `-32603`.
- A handler result that is already a serialized `CallToolResult` is now passed
  through unwrapped instead of being nested inside a text block. Domain data
  that merely carries a `content` key fails the full `CallToolResult` check
  and is still wrapped.
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
- A `Server` may now be destroyed while handlers are still in flight. The
  implementation outlives the `Server` until that work finishes, so a handler
  no longer runs against freed state, and a reverse request issued from a
  handler whose `Server` is gone throws `std::runtime_error` naming that cause
  instead of faulting — distinct from the failure reported for a session that
  is merely closing. Destroying a `Server` from a thread other than the one
  running its session no longer races on that session's request maps either:
  those entries are abandoned on the session strand, which is the only
  executor permitted to touch them.
- A session is now unregistered even when teardown throws — a transport that
  throws from `close()`, or a drain wait that fails for any reason other than
  cancellation. Previously such a failure left the session registered and
  every later `run()` was refused for the lifetime of the `Server`.
- Stateless and discover dispatch in the HTTP session manager is spelled as a
  named coroutine rather than a lambda handed to `co_spawn`, so the request
  moves into that coroutine's own frame. This avoids the GCC 11 defect that
  corrupts an object living in a coroutine frame across a suspension.
- The peer-input matrix build check now fails when
  `test/core/json_peer_input_matrix_test.cpp` differs from what
  `scripts/gen_json_matrix.py` generates, and exits 2 for a row whose decoder
  now throws on an explicit null or an absent key. The generator refuses to
  write such a row unless it is named with `--accept-regression`. The check is
  governed by the new `MCP_CPP_SDK_CHECK_JSON_MATRIX` option: it defaults to
  `ON` in a checkout, where a `BUILD_TESTING=ON` build now needs Python 3.9+,
  and to `OFF` in source archives, which do not ship the scripts. Previously a
  missing Python skipped the check with a warning, and a source-archive build
  with tests failed because the scripts were absent.
- Closing an OAuth client transport from a thread that does not run the
  `io_context` could be lost while a metadata lookup was resolving. The close
  found no socket to shut, and the flow then went on to connect and blocked
  until the HTTP timeout. The abort latch is now checked again just before the
  connection is opened, so such a close ends the flow promptly. This covers an
  `io_context` run by a single thread.

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
