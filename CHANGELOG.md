# Changelog

All notable changes are documented here. This project follows Semantic
Versioning; a `0.x` version is a stable release unless its version has an
`-rc.N` suffix.

## [Unreleased]

### Added

- The Chocolatey package has an icon.
- `Server::set_deprecation_handler()`, `ClientOptions::deprecation_handler` and
  `OAuthAuthorizationConfig::deprecation_handler` receive a notice when the
  object first uses a deprecated feature.
- `mcp::constants::include_context` names the values of `includeContext`;
  `g_THIS_SERVER` and `g_ALL_SERVERS` are deprecated.
- `Server::notify_elicitation_complete()` and
  `Context::notify_elicitation_complete()` tell the session that sent a URL
  elicitation that it completed; `Client::on_elicitation_complete()` reports it.
- A handler throws `UrlElicitationRequired` to answer a 2025-11-25 request with
  the -32042 error, which `url_elicitations_required()` reads on the client.
- `Client::subscribe_resource()`, `unsubscribe_resource()` and the deprecated
  `set_log_level()` send `resources/subscribe`, `resources/unsubscribe` and
  `logging/setLevel`.
- `Server::notify()` sends a notification of another method to sessions of
  2025-11-25 and earlier, and `Server::on_listen()` reports each listen stream
  that opens or closes.
- `OAuthAuthorizationManager` authorizes at a 2025-03-26 server that publishes
  no protected resource metadata, unless `legacy_discovery_fallback` is false.
- `Endpoint::legacy_sse()` and `StreamableHttpSessionManager::set_legacy_sse()`
  speak the deprecated HTTP+SSE transport of 2024-11-05 as client and server.
- `examples/features/` shows image, audio and embedded-resource content, binary
  resources, URL-mode elicitation, the HTTP+SSE transport, resource-template
  completion and roots changes.
- The documentation has a feature matrix that links each tier-assessment feature
  to its API reference, guide and example.
- `CONTRIBUTING.md` points to the contributing guide, and `MAINTENANCE.md`
  commits to an SDK release within 30 days of each specification release.

### Changed

- The documentation moved to [mcp-cpp-sdk.yurirocha.com](https://mcp-cpp-sdk.yurirocha.com/), and links to the old address redirect there.
- The installation guide has a page for each platform, each with its install, upgrade and uninstall steps.
- Without a handler, the first use of a deprecated protocol feature prints one
  line on standard error; setting `MCP_SILENCE_DEPRECATION_NOTICES` stops it.
- `Context::elicit()` throws `std::invalid_argument` for an `elicitationId` that
  another session has in progress.
- In a 2025-11-25 session, a POST answered as an SSE stream starts with a priming
  event; `set_legacy_stream_options()` turns it off or adds a `retry` field.
- Over Streamable HTTP, a message for a 2025-11-25 or earlier session that
  belongs to no request waits for the session's next GET stream when none is
  open and no POST waits, up to the queue limit.
- An open GET event stream gives back its `HttpConnectionLimits::max_requests`
  place once its response head is written; `max_connections` and the session
  limit bound open streams.

### Deprecated

- The APIs of Roots, Sampling, Logging, dynamic client registration and
  `includeContext` other than `"none"` carry `[[deprecated]]`, citing the
  proposal and earliest removal.
- The HTTP+SSE transport (`LegacySseClientTransport`, `Endpoint::legacy_sse()`
  and `set_legacy_sse()`) carries `[[deprecated]]`; the specification deprecates
  it.

### Fixed

- Questions a 2026-07-28 handler asks at the same time share one
  `input_required` result also on the handler executor, where one asked a moment
  later could miss the round.

### Migrating from 0.4.0

- A build that treats warnings as errors and uses a deprecated feature defines
  `MCP_DISABLE_DEPRECATION_ATTRIBUTES`, or moves to the migration path the
  warning names.

## [0.4.0] - 2026-11-26

### Highlights

- **Sync mode.** `Server` and `Client` run their own event loop, take
  synchronous handlers and make blocking calls, such as
  `client.sync().call_tool()`, with no Boost in the program. Async mode uses
  the same objects with coroutines.
- **Headers without Boost.** Only `<mcp/core/async.hpp>`,
  `<mcp/core/signal.hpp>`, `<mcp/transport/websocket.hpp>` and
  `<mcp/transport/http_beast.hpp>` include Boost; `<mcp/mcp.hpp>` and the other
  server, client, auth and transport headers do not. Code that defines or
  awaits a coroutine includes `<mcp/core/async.hpp>`.
- **Endpoints.** `mcp::Endpoint` connects a `Client` to a command it launches,
  to a Streamable HTTP URL or through the application's own transport, and a
  `StdioRestartPolicy` starts a launched server again when its process ends.
- **TLS and proxies.** Clients connect to `https://` servers, checked against
  the system's certificate authorities, and through HTTP proxies. Servers
  serve TLS and can require client certificates, and the WebSocket transports
  speak wss.
- **One HTTP server, many clients.** `Server::run_http()` serves any number of
  clients of 2025-11-25 and earlier, each in its own session.
- **JSON Schema without jsoncons.** The SDK's own engine validates every
  dialect it supports, and jsoncons is no longer compiled into the library.
- **Specification recommendations.** An opt-in client response cache,
  opt-in notification rate limits, tool name checks and `Host` checks on
  loopback connections; the documentation lists which parts of the
  specification the SDK implements.
- **HTTP performance.** `HttpClientTransport` sends requests on connections
  that earlier answers left open, instead of a new connection and TLS
  handshake per call, and HTTP servers allocate less for each request.
- **Typed transport errors.** The built-in transports throw `TransportError`,
  whose `kind()` tells closed, connect, I/O and protocol failures apart.

### Migrating from 0.3.0

Code written for 0.3.0 needs these changes to compile with 0.4.0:

- Write `mcp::Task<T>` for `boost::asio::awaitable<T>` in `ITransport` and `auth::Authenticator` overrides and in coroutine handlers given to the `add_*()` registrations.
- Include `<mcp/core/async.hpp>` where a coroutine is defined or awaited; `<mcp/core/core.hpp>` is gone.
- Include the Boost headers of the Asio and Beast types code uses, such as `<boost/asio/io_context.hpp>`; the SDK's headers no longer include them.
- A `StreamableHttpSessionManager::ServerFactory` takes a `const mcp::Executor&`; call `Executor::get()` for the `any_io_executor`.
- `mcp::set_custom_request_handler(manager, handler)` and `StringRequest`/`StringResponse` are in `<mcp/transport/http_beast.hpp>`.
- Construct transports with `Runtime::executor()` instead of `TransportFactory` or `make_*_transport()`.
- Call `make_memory_transport_pair()` where code called `create_memory_transport_pair()`.
- Pass the arguments of `get_prompt()` as a `std::map` instead of a `std::optional`; an empty map sends none.
- Include `<mcp/server/server.hpp>` where code uses `Server` after including only `<mcp/transport/http_session_manager.hpp>` or `<mcp/auth/oauth.hpp>`.
- Include `<mcp/transport/request_context.hpp>` where code included `<mcp/server/request_context.hpp>`.
- Include `<mcp/transport/transport.hpp>` where code uses `ITransport` after including only `<mcp/core/context.hpp>`.

These changes in behavior need no change to compile:

- Around `Client` calls, catch `McpError` where code caught `HttpStatusError` or `boost::system::system_error`; `std::rethrow_if_nested()` rethrows the transport's exception.
- Catch `TransportError` where code caught `boost::system::system_error` from a built-in transport's read or write; `code()` holds the error code.
- Under `run_http()`, reach a client through the handler's `Context`: the `Server`'s `send_request()`, `is_initialized()` and `get_log_level()` belong to no session.

### Added

- Sync mode: `Server` and `Client` run their own event loop, take synchronous handlers and make blocking calls, with no Boost in the program; async mode uses the same objects.
- `mcp::Endpoint` names the server a `Client(const Endpoint&)` connects to: a command to launch (`stdio()`), a Streamable HTTP URL (`http()`) or the application's transport (`custom()`).
- `Client::sync()`, `TaskHandle::sync()` and `Subscription::sync()` make blocking calls, and `Client::run()` runs a coroutine on the client's loop until it ends.
- `Endpoint::stdio()` takes a `StdioRestartPolicy`, under which the same `Client` starts the server again when its process ends and goes on with it.
- `RequestOptions::on_progress` receives the progress of one request, and `read_resource()`, `get_prompt()`, `complete()`, `start_tool()` and the `list_*()` members take `RequestOptions`.
- `Client::on_sampling()` registers a typed handler for `sampling/createMessage`.
- `Server(name, version)` constructs a server that advertises the capabilities its registrations imply.
- `mcp::tool_params()` writes a tool's input schema, and `add_tool()` takes a JSON handler with the request's `Context` and a `Tool` definition.
- `Server::add_text_resource()` and `Server::add_text_prompt()` register a resource or prompt whose text a function makes.
- `mcp::result_text()` joins the text blocks of a `CallToolResult`.
- `Server::run_http()` takes a callback that configures its `StreamableHttpSessionManager`, and `Server::bound_port()` returns the port it listens on.
- `Server::stop()` ends `run_stdio()`, `run_http()` and `run()` from any thread or handler.
- `Server::sync()` sends the `notify_*` notifications of `run_stdio()` and `run_http()` from threads other than the server's loop.
- `Runtime::executor()`, `spawn()`, `run(threads)` and `start(threads)`, so that a `Runtime` runs a program's loop or a pool for `set_handler_executor()`.
- `SyncTypeErasedHandler`, the synchronous form of `TypeErasedHandler`, and `Client::executor()`.
- `StreamableHttpSessionManager` takes a `Server` in place of a factory and serves it as `run_http()` does, on the application's event loop.
- `examples/sync/` and `examples/async/` hold a notes server and client, which the guide "Converting to Async Mode" converts, and a server and client with every feature, in each mode.
- `CompiledSchema::compile()` takes `SchemaDocuments`: schema documents, by
  absolute URI, that references may reach. Nothing is fetched.
- Clients connect to `https://` servers over TLS, checked against the system's certificate authorities, with `TlsClientOptions` for private ones and client certificates; OAuth reaches https servers.
- `HttpClientTransport::set_proxy()` and the OAuth configurations' `proxy` field send connections through an HTTP proxy; `ProxyOptions::from_environment()` reads `HTTPS_PROXY` and `NO_PROXY`.
- `HttpServerTransport` and `StreamableHttpSessionManager` serve TLS with `set_tls()` and can require client certificates; the WebSocket transports speak wss on both sides.
- `ClientOptions::response_cache` lets a `Client` answer list and `read_resource()` calls from earlier results while their `ttlMs` lasts; `RequestOptions::cache_mode` refreshes or bypasses it.
- `Server::set_notification_rate_limit()` and `ClientOptions::notification_rate_limit` cap the progress and log notifications each request sends or delivers; there is no limit by default.
- The documentation lists which parts of the specification the SDK implements and marks the features protocol revision 2026-07-28 deprecates.
- The built-in transports throw `TransportError` for their failures, network errors included; `kind()` tells closed, connect, I/O and protocol failures apart; `code()` holds a network error's code.
- `mcp::constants::methods` names the JSON-RPC methods the SDK sends and handles, such as `g_TOOLS_CALL`.
- `Icon`, `Implementation`, `ServerCapabilities` and `InitializeResult` compare with `==`.
- `mcp::constants` names the numeric defaults of the options, such as `client::g_DEFAULT_REQUEST_TIMEOUT` for `ClientOptions::request_timeout`.

### Changed

- `<mcp/mcp.hpp>` includes no Boost header; only `async.hpp`, `signal.hpp`, `websocket.hpp` and `http_beast.hpp` do, and coroutine code includes `<mcp/core/async.hpp>`.
- `mcp::Task<T>` is a class derived from `boost::asio::awaitable<T>`, declared without Boost in `<mcp/core/async_fwd.hpp>`; `co_spawn` takes a callable that returns one.
- Public APIs take `mcp::Executor`, which needs no Boost, instead of `boost::asio::any_io_executor`; an Asio executor converts to it.
- `Server::run_http()` serves any number of clients of 2025-11-25 and earlier, each in its own session, where it served one at a time.
- A server advertises the capability each registration implies, and `Server::capabilities()` returns a reference for the rest.
- A legacy `initialize` declares sampling and elicitation when the client has a handler for them, as it does for roots.
- `Server::add_raw_tool()` takes any coroutine JSON handler, with or without the request's `Context`.
- `set_custom_request_handler()` is a free function in `<mcp/transport/http_beast.hpp>`, where `StringRequest` and `StringResponse` moved from `http_types.hpp`.
- `ITransport` and `auth::Authenticator` are exported from the library.
- The OAuth, client and server types have headers of their own, such as `<mcp/client/subscription.hpp>`; `<mcp/auth/oauth.hpp>`, `<mcp/client/client.hpp>` and `<mcp/server/server.hpp>` include them all.
- `Client::get_prompt()` and its blocking twin take the arguments as a `std::map`, empty for none, so `{{"name", "Ada"}}` is accepted.
- A `Client` call whose transport fails throws `McpError` (`g_CONNECTION_CLOSED`), with an HTTP status in `data()` and the transport's exception nested unless a response stream broke off.
- `Client::call_tool()` with `nlohmann::json` arguments is an ordinary member function; the template overloads take other argument types.
- `Runtime::run()` runs the loop again after an earlier run ended; a `stop()` made while nothing runs the loop ends the next run instead.
- A closing `StdioClientTransport` keeps its event loop from running out of work until the process has ended and `on_exit` has run.
- On every platform, destroying the context a `Server` runs on waits for its handlers on the `set_handler_executor()` executor; `run_stdio()` and `run_http()` still return at the shutdown deadline.
- Every JSON Schema is validated by the SDK's own evaluation. jsoncons is no
  longer compiled into the library, and packages carry only the SDK's license.
- Schemas are checked against the published metaschemas: a draft-06 `enum` must
  be non-empty with unique values, and a draft-07 `writeOnly` must be a boolean.
- An empty `$ref` or `$id` is accepted. A draft-04 `$ref` that is not a string,
  and an anchor declared twice in one resource, make a schema invalid.
- `properties` may nest 31 levels deep in a schema, up from 26.
- A `$ref` to a location inside a metaschema, such as draft-07's
  `#/definitions/nonNegativeInteger`, is refused as unresolved.
- HTTP clients put the port in the `Host` header when it is not the scheme's default, as RFC 9110 requires.
- `HttpClientTransport` warns once through its warning handler when bearer tokens would go over `http://` to a host other than a loopback one.
- `HttpClientTransport` sends a request on a connection an earlier answer left open, keeping up to four, so calls no longer each make a connection and a TLS handshake.
- HTTP and WebSocket servers answer 403 to a request on a loopback connection whose `Host` names no loopback host; `set_allowed_hosts()` lists other names, as a same-host proxy needs.
- A `Client` pinned to 2026-07-28 sends `server/discover` once more when the server refuses that version while listing it as supported.
- With a `StdioRestartPolicy`, `Endpoint::stdio()` sends each open `subscriptions/listen` request to the restarted server, so its `Subscription` goes on.
- Tool registration throws for a name that is empty, longer than 128 characters or holds characters other than ASCII letters, digits, `_`, `-` and `.`.
- `Context::sample_llm()` refuses an `includeContext` other than "none" for a client that did not declare `sampling.context`.
- An OAuth metadata discovery that finds no document says in its error why each well-known URL it tried failed.
- `RequestContext` is declared in `<mcp/transport/request_context.hpp>`, and the transport, auth and `Context` headers no longer include server or transport headers they do not use.
- Each `Client` call and its `Client::sync()` twin take `RequestOptions` as a last parameter that defaults to none, in place of a second overload.
- `create_memory_transport_pair()` is named `make_memory_transport_pair()`, like the other factory functions.
- HTTP servers allocate less for each request, and `StreamableHttpSessionManager` sessions reach their `Server` without memory transports.
- The calls of `TaskHandle` and of its `sync()` form that return a value or a `Task` are `[[nodiscard]]`.

### Removed

- `<mcp/core/core.hpp>`; `Task` is in `<mcp/core/async.hpp>`.
- `mcp::TransportFactory` and the deprecated `make_stdio_transport()` and `make_http_client_transport()`; transports are constructed with `Runtime::executor()`.
- The `mcp::auth::net` alias of `boost::asio`.
- The installed headers no longer declare the JSON parser, the regular expression engine, the serialized transport writer, the executor access, the secure random helpers or the OAuth encoders.

### Fixed

- The documentation site fits phone and tablet screens: pages no longer grow wider
  than the screen, and long identifiers, signatures and parameter lists wrap.
- A list request whose cursor is not wholly a decimal offset within the list,
  such as `2abc`, `-1` or one past the end, is answered with -32602 instead of a
  page.
- Registering a resource template whose variable list ends with a comma, such as
  `{a,}`, throws `std::invalid_argument`, as one that starts with a comma does.
- An optional member a peer sends as `null` reads as absent in every protocol
  type: `"instructions": null` no longer fails a legacy `connect()`, and
  `"tools": null` no longer declares tools.
- A message for a stream of a 2025-11-25 or earlier session that comes while the
  stream writes is sent once the write ends, instead of with the next keep-alive
  comment.
- A client's request no longer fails when the server cancels a request of its
  own that has the same id, as it does for an elicitation it stops waiting for.
- A `completion/complete` request whose reference type is neither `ref/prompt`
  nor `ref/resource` is answered with -32602 instead of being read as a resource
  reference.
- `StreamableHttpSessionManager::listen()` called after `close()` no longer
  starts a timer that keeps the event loop running for 30 seconds.
- A schema `pattern` may write a `\u{...}` escape with leading zeros, such as
  `\u{0000041}`, as ECMAScript allows, instead of being refused.
- A client reads event streams as the SSE standard has it: a lone CR ends a
  line, and an event the stream ends inside of is dropped instead of being read
  as a message.
- A stream of a 2025-11-25 or earlier session that breaks inside an event
  resumes after the last whole event, so the broken event is sent again instead
  of lost.
- `HttpClientTransport` and `OAuthHttpClient` take a URL whose scheme is in
  uppercase, such as `HTTPS://`, which the metadata fetch policy already took.
- The metadata fetch policy refuses a URL with text other than a port after an
  IPv6 host, such as `https://[2001:db8::1]x/`, which the fetch itself refused.
- `OAuthClientTransport` sends a request with positional (array) params as
  written, instead of replacing the params with an object that holds the token.
- The friend declaration of `decode_elicitation_content()` is exported like its
  first declaration, so MSVC no longer reports warning C4273 for it.
- The AUR package's build no longer prints a gpg warning that the release key
  is not certified when it checks the source signature.
- A middleware may return `next(ctx, params)` without awaiting it.
- Closing a client ends a wait for a task at once, instead of after the pause
  before the next poll.
- A client sends `notifications/cancelled` for a request that times out in
  sessions of 2025-11-25 and earlier too, as the specification asks.
- The error for an HTTP response stream that breaks off names the reason without
  the source location that Boost adds to it.
- The LLDB example returns a command that LLDB fails as a tool error instead of
  as its output.

## [0.3.0] - 2026-11-17

Each entry is a summary; the full text is in the
[changelog of v0.3.0](https://github.com/yurirocha15/mcp-cpp-sdk/blob/v0.3.0/CHANGELOG.md#030---2026-11-17).

### Highlights

- **Protocol revision 2026-07-28.** Servers serve it alongside 2025-11-25 and
  earlier revisions. The client finds out which revision a server speaks
  (`ClientProtocol::automatic()`, the default), or keeps to one with
  `ClientProtocol::legacy()` or `pinned()`.
- **Multi round-trip requests and subscriptions.** The handler of a 2026-07-28
  `tools/call`, `prompts/get` or `resources/read` can ask the client to elicit,
  sample or list roots, and the request resumes with the answers.
  `subscriptions/listen` streams list-changed and resource-updated
  notifications.
- **Schema validation.** `CompiledSchema` validates JSON Schema 2020-12,
  2019-09, draft-07, draft-06 and draft-04. Tool arguments and structured
  results are checked against the tool's schemas, and elicitation answers
  against `requestedSchema`, on both sides; each check can be turned off.
- **Tasks.** The tasks extension (`io.modelcontextprotocol/tasks`) on the server
  and the client.
- **Cancellation.** `CancellationToken` cancels requests from any thread,
  without their ids.
- **Launching stdio servers.** `StdioClientTransport` starts an MCP server as a
  child process, and `StdioClientSession` can start it again when it ends
  unexpectedly.
- **Typed elicitation forms.** `ElicitationSchema` builds a form's
  `requestedSchema` from typed fields and decodes the answer into typed values.
- **Extensions.** MCP Apps, Skills, OAuth client credentials and
  enterprise-managed authorization.
- **Builds** with Boost 1.74 and 1.75, the documented minimum, and with Boost
  1.87 and later.

### Migrating from 0.2.0

Code written for 0.2.0 needs these changes to compile with 0.3.0:

- The public constants are grouped under `mcp::constants` with `g_UPPER_CASE`
  names, such as `mcp::constants::jsonrpc::g_INVALID_PARAMS`; the old names are
  gone.
- `Client::connect()` resolves to `ConnectResult` instead of `InitializeResult`,
  and its `serverInfo` is optional (`result.serverInfo->name`).
- `Server::set_cache_hints()` replaces `Server::set_discover_ttl_ms()` and
  `Server::set_discover_cache_scope()`.
- `PrimitiveSchemaDefinition` is a `std::variant` of `StringSchema`,
  `NumberSchema`, `BooleanSchema` and the enum schemas instead of one struct.
- `ElicitRequestURLParams::elicitationId` is optional, and
  `ElicitationCompleteNotificationParams` names the elicitation by
  `elicitationId`, not `requestId`.
- The task types follow the tasks extension: `GetTaskResult` is `DetailedTask`,
  requests name the task `taskId`, and `TaskData::ttlMs` is optional.
- A `RequestSender` that builds a `Context` takes a third argument, the key of
  the question.
- The enumerators of `AuthorizationResponseStatus`, `ClientIdentitySource`,
  `ClientIdentityDecision` and `MetadataUrlDecision` have the `eName` form, such
  as `eStateMissing`.
- `BearerTokenValidator` and `AsyncBearerTokenValidator` return
  `TokenValidation`; a validator returning `bool` still converts, and `co_return
  true;` still works.
- The `to_json` and `from_json` functions of the protocol types are compiled
  into the library and take `nlohmann::json` only.
- `make_stdio_transport()` and `make_http_client_transport()` warn as
  deprecated, which fails a `-Werror` build; use `TransportFactory`.

These changes in behavior and defaults need no change to compile:

- `Client::connect()` sends `server/discover` before `initialize`, one more
  round trip for a legacy server; `ClientProtocol::legacy()` sends only
  `initialize`.
- `g_LATEST_PROTOCOL_VERSION` is `2026-07-28`, and
  `g_LATEST_LEGACY_PROTOCOL_VERSION` is `2025-11-25`.
- `g_CONNECTION_CLOSED` is -31000; its old value, -32002, is now
  `g_RESOURCE_NOT_FOUND`.
- Tool schemas, tool arguments, structured tool results and elicitation answers
  are checked, as Changed lists; each check can be turned off.
- An unknown tool in `tools/call` or prompt in `prompts/get` is answered
  `g_INVALID_PARAMS` instead of `g_METHOD_NOT_FOUND`.
- New default limits: 1024 requests and 256 MiB in flight per session, 10,000
  HTTP sessions, sessions idle for five minutes end, and stdio and WebSocket
  messages of 16 MiB.
- `StdioTransport::write_message()` throws `std::invalid_argument` for a message
  that contains a newline.
- `HttpClientTransport` starts a new session and sends the request again when
  the server answers 404 for its session.
- OAuth asks for `offline_access` when the authorization server lists it, and
  for a wider scope once per request on an `insufficient_scope` challenge.
- `StreamableHttpSessionManager::set_tool_executor()` moves only handler calls
  to that executor, and the server factory receives the HTTP executor.
- `BUILD_TESTING`, `BUILD_EXAMPLES` and `BUILD_DOCS` default to `ON` only when
  the SDK is the top-level project.

### Added

- Protocol revision 2026-07-28: servers answer its requests without
  `initialize`, next to legacy sessions, and handlers read `_meta` with
  `Context::request_meta()`.
- `ServerProtocolPolicy` (`Server::set_protocol_policy()`) chooses whether a
  server serves 2026-07-28, legacy clients or both.
- `g_LEGACY_PROTOCOL_VERSIONS`, `g_MODERN_PROTOCOL_VERSIONS`,
  `is_legacy_protocol_version()` and `is_modern_protocol_version()`.
- Results of 2026-07-28 requests carry the server's name and version, and the
  cache hints set with `Server::set_cache_hints()`.
- Both HTTP server transports serve 2026-07-28 requests on the endpoint of the
  legacy sessions, check their `Mcp-*` headers and stream their notifications as
  SSE.
- `set_stream_options()` and `stream_stats()` bound and count the streamed
  answers of both HTTP server transports.
- `x-mcp-header` tool parameters: `add_tool()` checks the annotations, and the
  server checks the `Mcp-Param-*` headers of a `tools/call` over HTTP against
  its arguments.
- `ITransport::http_server_transport()` lets a transport that wraps an
  `HttpServerTransport` be served as one; it changes the layout of
  `ITransport`'s virtual table.
- `Context::principal()` names who sent a request, from the bearer token
  validator's `TokenValidation`.
- Multi round-trip requests: a 2026-07-28 handler can ask the client to elicit,
  sample or list roots, and the client's retry resumes it
  (`Server::set_mrtr_options()`).
- Sealed request state (`MrtrStateMode::eSealed`), so that any server instance
  with the key can serve the retry of a multi round-trip request.
- `InputOptions` names a question of a multi round-trip request;
  `Context::elicit()`, `sample_llm()` and `request_roots()` take it as an
  optional argument.
- `subscriptions/listen` on the server: the `notify_*()` calls also send
  list-changed and resource-updated notifications to each open subscription.
- The tasks extension on the server: `Server::enable_tasks()`,
  `set_tool_task_mode()`, and `TaskStore` with the default `InMemoryTaskStore`.
- `Server::set_tool_task_preparation()` runs a coroutine inside a `tools/call`
  before its task exists, which may ask the client for input.
- `Server::declare_extension()`, `Server::capabilities()` and
  `Server::add_extension_method()`, which serves an extension's request method
  to 2026-07-28 clients.
- `declare_extension()`, `declares_extension()` and `extension_settings()` in
  `mcp/protocol/extensions.hpp`.
- `Server::set_handler_executor()` sets where a server's handlers run.
- The client finds out whether a server speaks 2026-07-28
  (`ClientProtocol::automatic()`, the default), or keeps to one with `legacy()`
  or `pinned()`.
- `HttpClientTransport` sends 2026-07-28 requests as sessionless POSTs with the
  `Mcp-*` headers and reads streamed answers as they arrive.
- The client answers the questions of an `input_required` result with its
  registered handlers and retries until the result is complete.
- `Client::listen()` opens a `subscriptions/listen` stream and returns a
  `Subscription`.
- The tasks extension on the client: `ClientOptions::tasks` follows a task to
  its result, and `Client::start_tool()` returns a `TaskHandle`.
- `CancellationToken` and `RequestOptions::cancellation` cancel requests from
  any thread without their ids.
- Sessions of 2025-11-25 and earlier stream over HTTP: a handler's notifications
  and requests reach the client while the POST runs.
- `HttpClientTransport` sends each message of a legacy session on its own
  exchange, so requests overlap, and keeps the session's GET stream open.
- `CompiledSchema` validates values against a JSON Schema of 2020-12, 2019-09,
  draft-07, draft-06 or draft-04.
- `ElicitationSchema` builds a form's `requestedSchema` from typed fields and
  decodes the answer into typed values.
- The client and `Context::elicit()` check accepted form content against
  `requestedSchema` (`ElicitationContentError`).
- `StdioClientTransport` launches an MCP server as a child process and talks to
  it over its standard input and output.
- `StdioClientSession` connects a `Client` to a launched server and starts the
  server again when it ends unexpectedly.
- MCP Apps (`io.modelcontextprotocol/ui`): `mcp/server/apps.hpp` registers views
  and their tools, and `mcp/client/apps.hpp` reads them.
- Skills (`io.modelcontextprotocol/skills`): `skills::SkillsExtension` serves
  skills, and `mcp/client/skills.hpp` lists, gets and reads them.
- OAuth client credentials: `auth::ClientCredentialsAuthenticator` gets an
  access token for machine-to-machine use.
- Enterprise-managed authorization: `auth::EnterpriseAuthorizationAuthenticator`
  exchanges an identity assertion for an ID-JAG.
- `set_scope_requirements()` on both HTTP server transports names the scopes
  each request needs; a token without them is answered `403`.
- `ITransport::set_interaction_observer()`, through which a transport reports
  that a request waits for a person.
- `OAuthDiscoveryClient::forget_protected_resource()` drops the cached metadata
  of one resource.
- `TextResourceContents` and `BlobResourceContents` carry `_meta`.
- `examples/features/schema_validation.cpp` shows each tool schema check.
- Packages include the license of the vendored jsoncons library.
- The conformance fixtures cover the 2026-07-28 scenarios of the official
  runner's alpha channel, which a CI job runs without gating.

### Changed

- `Server::add_tool()` throws `SchemaError` for a schema that is not a valid
  JSON Schema object, or an input schema without `"type": "object"`.
- `tools/call` arguments that do not conform to the input schema are answered
  with an `isError` result, and the handler does not run.
- Structured tool content that does not conform to the output schema is answered
  `g_INTERNAL_ERROR` by the server and throws `StructuredContentError` in the
  client.
- `make_tool_structured_result()` accepts any JSON value; a value that is not an
  object is sent only to a 2026-07-28 request.
- `server/discover` is a 2026-07-28 request: without a protocol version in
  `_meta` it is answered `g_INVALID_PARAMS`.
- `notifications/cancelled` cancels any request the session serves, so
  `Context::is_cancelled()` turns true in resource and prompt handlers too.
- An unknown resource in `resources/read` is answered with its URI in the
  error's `data`, and with -32002 in sessions of 2025-11-25 and earlier.
- Middleware also runs for `resources/subscribe`, `resources/unsubscribe` and
  `completion/complete`.
- A GET within an HTTP session opens the session's SSE stream, or resumes one
  with `Last-Event-ID`, and a GET that does not accept `text/event-stream` is
  answered `406`.
- An HTTP session accepts an `MCP-Protocol-Version` header naming any version
  `initialize` can negotiate.
- An error response that answers no request it can name leaves out `id` instead
  of writing `"id": null`.
- Less copying, parsing and allocation per message: the SDK writes and parses
  JSON itself, shares compiled tool schemas, and writes in place where it can.
- The OAuth guides describe how the scope of an authorization request is chosen
  and stepped up, and the limits on repeated authorization.

### Deprecated

- `make_stdio_transport()` and `make_http_client_transport()`; use
  `TransportFactory::create_stdio()` and
  `TransportFactory::create_http_client()`.
- `Context::sample_llm()` and `Context::request_roots()`, in their documentation
  only, since 2026-07-28 deprecates sampling and roots.

### Removed

- The task types of the experimental shape of 2025-11-25, such as
  `GetTaskRequest`, `ListTasksRequest` and `Tool::execution`.
- `Server::set_discover_ttl_ms()` and `Server::set_discover_cache_scope()`,
  replaced by `Server::set_cache_hints()`.
- `g_DISCOVERABLE_PROTOCOL_VERSIONS`; `g_SUPPORTED_PROTOCOL_VERSIONS` lists the
  same versions.

### Fixed

- The SDK builds with Boost 1.74 and 1.75, the documented minimum, and with
  Boost 1.87 and later.
- Built with GCC 12, the auth middleware no longer hangs and
  `OAuthClientTransport` reads no longer crash.
- With Boost 1.77 to 1.84, cancelling a coroutine that awaits an SDK operation
  no longer ends the process.
- With Boost 1.75 and older, an OAuth metadata response larger than
  `max_response_bytes` is refused instead of read in full.
- With Boost older than 1.82, Beast errors that the shared library throws
  compare equal to the caller's `boost::beast` enumerators.
- With libc++, `nlohmann_json` exceptions that the shared library throws can be
  caught by their type.
- On platforms other than Windows and macOS, the shared library no longer
  exports the symbols of statically linked dependencies, such as OpenSSL.
- With `BUILD_DOCS=ON`, configuring a project that adds the SDK as a subproject
  no longer fails.
- The Chocolatey package downloads the Windows archive at install time and
  requires a `vcredist140` at least as new as the tools that built it.
- Examples and documentation that did not work as written, among them the
  getting-started client and the LLDB, llama.cpp and stdio client examples.
- `from_json` overwrites its whole target, so `nlohmann::json::get_to()` gives
  the same value as `get()`.
- A replay for `Last-Event-ID` carries only the stream of the named event, not
  every later event of the session.
- Requests a server sends while it handles a legacy HTTP POST, such as
  `elicitation/create`, reach the client at once instead of with the POST's
  response.
- A `StreamableHttpSessionManager` handler that blocks its thread no longer
  holds up the other sessions on its keep-alive connection.
- A throw that does not derive from `std::exception`, in middleware or a
  resource or prompt handler, is answered `-32603` instead of not at all.
- A server session limits its requests in flight, their memory and its resource
  subscriptions, and a request it sends fails after ten minutes.
- Both HTTP server transports check the origin and token before they read a
  body, and bound connections, requests and memory (`set_connection_limits()`).
- `HttpServerTransport` ends a session idle for five minutes when a new
  `initialize` arrives, and the SSE replay store keeps at most 32 MiB.
- `StdioTransport` and both WebSocket transports bound message sizes, and
  `WebSocketServerTransport` has handshake and idle timeouts.
- `Server::run_stdio()` stops on SIGTERM and SIGINT, ends when standard output
  closes, and writes the queued answers when standard input ends.
- `HttpClientTransport` starts a new session when the server answers 404 for the
  current one, and reconnects when the server closed its idle connection.
- `HttpClientTransport` names the requested version in the
  `MCP-Protocol-Version` header of `initialize`, and reports the JSON-RPC error
  of a 400 or 404 answer.
- Both HTTP server transports answer `431` or `400` to a request they cannot
  parse, and `100 Continue` to `Expect: 100-continue`.
- Both HTTP accept loops try again when the process runs out of file
  descriptors.
- An answer to a server request is accepted only from the session it was sent
  in.
- A `Client` handler's answer that cannot be encoded as JSON is sent as an
  error, so the server's request no longer waits out its timeout.
- A message holding a number past the range of a double is answered as a parse
  error instead of ending a `MemoryTransport` server's run.
- A request id above INT64_MAX is refused as an invalid request.
- An error `code` that is not an `int` decodes to `-32603`, and a reply to
  `elicitation/create` with an unknown action no longer decodes as `accept`.
- A new client on a reused `Server` starts at the default log level on every
  transport.
- `http_bearer_token()` skips every space after `Bearer`.
- `OAuthAuthorizationManager` checks the authorization endpoint and the client
  metadata URL before it uses them.
- OAuth step-ups: one per request by default (`max_step_ups_per_request`), none
  for scopes the issuer already granted, and granted scopes count only for their
  issuer.
- `OAuthAuthorizationManager` reads the protected-resource metadata for every
  challenge, so it notices a move to another authorization server.
- Request timeouts no longer run while `OAuthClientTransport` waits for the user
  to answer an authorization prompt.

## [0.2.0] - 2026-10-21

Each entry is a summary; the full text is in the
[changelog of v0.2.0](https://github.com/yurirocha15/mcp-cpp-sdk/blob/v0.2.0/CHANGELOG.md#020---2026-10-21).

### Added

- Shared and static library variants with installed CMake and pkg-config
  metadata.
- A release pipeline for the GitHub Release, ConanCenter, APT, RPM, AUR,
  Homebrew and Chocolatey, with deterministic, checksummed and signed assets.
- Client errors and options: `McpError`, `ClientOptions`, `RequestOptions` and
  `RequestId::correlation_key()`.
- Tool result helpers `make_tool_text_result()`, `make_tool_error_result()` and
  `make_tool_structured_result()`, and `Server::add_raw_tool()`.
- `Server::add_resource_template()` routes `resources/read` requests whose URI
  matches an RFC 6570 template.
- Bearer authentication over HTTP: a token provider on `HttpClientTransport`,
  token validators on the HTTP servers, and `HttpStatusError`.
- `set_async_bearer_token_validator()`, for a token check that needs I/O.
- Server-side OAuth challenges: `set_bearer_challenge()`,
  `set_protected_resource_metadata()` and `set_unauthenticated_paths()` on the
  HTTP servers.
- `OAuthAuthorizationManager`, which runs the MCP authorization flow from a
  `WWW-Authenticate` challenge.
- `MetadataFetchPolicy`, which every `OAuthHttpClient` request passes, with
  allowed and denied origins, address checks and size limits.
- `OAuthHttpClient::make_scope()`, `post_json()` and `abort_pending()`, and the
  challenge, discovery and client identity types of the authorization flow.
- `OAuthClientTransportOptions`, which bounds the requests kept for a replay
  after a token refresh.
- The `server/discover` method, with `Server::set_instructions()` and the
  `DiscoverRequest` and `DiscoverResult` types.
- `set_max_request_body_bytes()` on the HTTP servers, 8 MiB by default.
- `WebSocketServerTransport::set_allowed_origins()` and
  `set_allow_all_origins()`.
- A `connect_timeout` on `WebSocketClientTransport`, 30 seconds by default.
- `StdioTransport::create_owning_stdout()`, which keeps stray output of the
  application off the protocol stream.
- `ClientOptions::on_protocol_error`, called when the client drops an incoming
  message.
- `Context` takes a `MessageSender`, and `MemoryTransport` is exported from the
  shared library.
- The JSON-RPC error codes `g_HEADER_MISMATCH`,
  `g_MISSING_REQUIRED_CLIENT_CAPABILITY` and `g_UNSUPPORTED_PROTOCOL_VERSION`,
  and `g_PROTOCOL_VERSION_2026_07_28`.
- An optional `extensions` member on `ClientCapabilities` and
  `ServerCapabilities`.
- Fixtures and a pinned harness for the official MCP conformance runner, with a
  CI workflow that fails on any change of the results.
- Cross-SDK benchmark tooling and results.
- `ROADMAP.md`, `MAINTENANCE.md`, `VERSIONING.md` and `DEPENDENCY_POLICY.md`,
  issue templates and a label manifest.
- `scripts/build.py --tsan` and `--msan` build and run the tests under
  ThreadSanitizer and MemorySanitizer, and CI runs every sanitizer.
- CI compiles the README snippets, checks the stdio examples' output, and checks
  the peer-input coverage of every protocol type's `from_json`.

### Changed

- Client requests time out after 30 seconds by default and fail with
  `g_REQUEST_TIMEOUT` (`ClientOptions::request_timeout`).
- Over HTTP, `OAuthClientTransport` sends the access token in the
  `Authorization` header instead of `_meta.auth_token`.
- `OAuthClientTransport` follows a `401` or an `insufficient_scope` `403` into
  the authorization flow its challenge names.
- The HTTP and WebSocket servers answer `403` to a request whose `Origin` header
  names an origin that is not allowed.
- A server session enforces the initialization sequence, and a session the
  client ended with `DELETE` is over.
- Tool results are always sent as a `CallToolResult`, and a `std::exception`
  from any tool handler is answered with an `isError` result.
- Middleware runs outside the tool handler's exception guard; its exceptions are
  answered `-32603`.
- Params that fail to decode are answered `-32602`, and a message the server
  cannot parse is answered `-32700` without ending the session.
- An incoming message the client cannot decode is dropped and reported instead
  of ending the session.
- An optional member sent as an explicit `null` reads as absent in every
  protocol type.
- Registering a second tool, resource, prompt or resource template under the
  same name or URI throws `std::invalid_argument`.
- The HTTP servers accept configuration only before `listen()`, and a second
  concurrent `read_message()` on a transport throws `std::logic_error`.
- `Server::run()` waits for its handlers before it returns, and a `Server` may
  be destroyed while a session still has handlers running.
- `Client::connect()` throws when called twice, and the client rejects a message
  without `"jsonrpc": "2.0"`.
- A response is matched to its request by id value and type.
- Resource templates are matched in time linear in the URI's length, and an
  ambiguous template is refused at registration.
- Messages nested more than 128 levels deep and `resources/read` URIs longer
  than 512 characters are refused.
- OAuth protected-resource metadata must name the configured server, and an
  `OAuthHttpClient` without a `MetadataFetchPolicy` refuses every request.
- Closing a client transport, `OAuthHttpClient` or `OAuthClientTransport` from
  another thread takes effect at once.
- The project version is read from the root `VERSION` file, and the minimum
  toolchains and dependency versions are documented.
- Implementation moved out of the headers into compiled translation units.

### Fixed

- `HttpClientTransport` opens a new connection after a response with
  `Connection: close`.
- `HttpServerTransport` no longer takes a request the server sends for the
  response to a client request with the same id.
- `HttpClientTransport` and `OAuthHttpClient` no longer end the process when a
  request's deadline passes between two socket operations.

### Security

- HTTP session ids are generated with OpenSSL's `RAND_bytes`.
- Text a peer controls is cleaned before it appears in an OAuth exception
  message.
