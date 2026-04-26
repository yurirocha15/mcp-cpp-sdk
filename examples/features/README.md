# MCP C++ SDK Feature Examples

This directory contains feature examples demonstrating advanced capabilities of the MCP C++ SDK. Each example is self-contained, non-interactive, and uses the loopback pattern (in-process server+client) for fast, deterministic testing.

## Quick Start

### Build All Examples

```bash
python scripts/build.py --examples
```

### Run All Examples

```bash
cd build/release
for exe in example-feature-*; do
    echo "Running $exe..."
    ./$exe
done
```

### Run Individual Examples

```bash
./build/release/example-feature-progress-cancellation
./build/release/example-feature-middleware
./build/release/example-feature-oauth-flow
# ... etc
```

## Feature Examples

### 1. Progress & Cancellation (`progress_cancellation.cpp`)

**What it demonstrates:**
- `ctx.report_progress()` for real-time progress updates
- `ctx.is_cancelled()` for cancellation detection
- Client-side cancellation requests

**Key APIs:**
- `Context::report_progress(current, total, message)`
- `Context::is_cancelled()`

**Run:** `./build/release/example-feature-progress-cancellation`

**Expected output:** Progress percentages from 10% to 100%, then cancellation handling

---

### 2. Notifications & Subscriptions (`notifications_subscriptions.cpp`)

**What it demonstrates:**
- `server.notify_tools_list_changed()` - notify clients of tool changes
- `server.notify_resources_list_changed()` - notify of resource changes
- `server.on_subscribe()` / `on_unsubscribe()` - subscription callbacks
- Client-side notification handling

**Key APIs:**
- `Server::notify_tools_list_changed()`
- `Server::notify_resources_list_changed()`
- `Server::on_subscribe()` / `Server::on_unsubscribe()`
- `Client::on_notification()`

**Run:** `./build/release/example-feature-notifications-subscriptions`

**Expected output:** Resource registration, subscription callbacks, list change notifications

---

### 3. Middleware (`middleware.cpp`)

**What it demonstrates:**
- `server.use()` for middleware registration
- Middleware chain execution order
- Short-circuiting requests in middleware
- Composing multiple middleware layers

**Key APIs:**
- `Server::use(middleware)`

**Run:** `./build/release/example-feature-middleware`

**Expected output:** Middleware A → B → C execution order, short-circuit demonstration

---

### 4. Completions (`completions.cpp`)

**What it demonstrates:**
- `server.set_completion_provider()` for argument completion
- `client.complete()` for requesting completions
- CompleteResult handling

**Key APIs:**
- `Server::set_completion_provider()`
- `Client::complete()`

**Run:** `./build/release/example-feature-completions`

**Expected output:** Completion suggestions for tool arguments

---

### 5. Pagination (`pagination.cpp`)

**What it demonstrates:**
- `server.set_page_size()` for controlling result pagination
- Cursor-based pagination handling
- `nextCursor` in list results

**Key APIs:**
- `Server::set_page_size()`

**Run:** `./build/release/example-feature-pagination`

**Expected output:** Paginated tool list with cursors (3 items per page)

---

### 6. Elicitation (`elicitation.cpp`)

**What it demonstrates:**
- `ctx.elicit()` for server-initiated information requests
- `client.on_elicitation()` for handling elicitation requests
- Form-based elicitation patterns

**Key APIs:**
- `Context::elicit()`
- `Client::on_elicitation()`

**Run:** `./build/release/example-feature-elicitation`

**Expected output:** Elicitation request sent from server, client responds with user info

---

### 7. Roots (`roots.cpp`)

**What it demonstrates:**
- `client.set_roots()` for declaring client root URIs
- `ctx.request_roots()` for server to discover client roots
- `client.on_roots_list()` for responding to roots/list

**Key APIs:**
- `Client::set_roots()`
- `Context::request_roots()`
- `Client::on_roots_list()`

**Run:** `./build/release/example-feature-roots`

**Expected output:** Client sets roots, server requests and lists them

---

### 8. Error Handling (`error_handling.cpp`)

**What it demonstrates:**
- Exception throwing from tool handlers
- Returning explicit error JSON
- Client-side error catching
- Graceful degradation patterns

**Key APIs:**
- Exception handling in async tool handlers
- `CallToolResult` with `isError` flag

**Run:** `./build/release/example-feature-error-handling`

**Expected output:** Caught exceptions, error JSON responses, graceful handling

---

### 9. Memory Transport (`transport_memory.cpp`)

**What it demonstrates:**
- `create_memory_transport_pair()` for in-process testing
- `TransportFactory` with `Runtime` for transport creation
- In-memory bidirectional communication

**Key APIs:**
- `create_memory_transport_pair()`
- `TransportFactory::create_stdio()`
- `TransportFactory::create_http_client()`
- `Runtime` class

**Run:** `./build/release/example-feature-transport-memory`

**Expected output:** MemoryTransport demo, TransportFactory overview

---

### 10. Graceful Shutdown (`graceful_shutdown.cpp`)

**What it demonstrates:**
- Signal handling with `io_context`
- `graceful_shutdown()` for clean server shutdown
- Timer-based forced shutdown
- `server.run_stdio()` convenience method

**Key APIs:**
- `mcp::detail::graceful_shutdown()`
- `mcp::detail::trigger_shutdown_signal()`
- `Server::run_stdio()`

**Run:** `./build/release/example-feature-graceful-shutdown`

**Expected output:** Signal handler ready, auto-triggered shutdown, clean exit

---

### 11. HTTP Server Convenience (`http_server_convenience.cpp`)

**What it demonstrates:**
- `server.run_http(host, port)` convenience API
- Comparison with manual HTTP setup
- HTTP transport for client-server communication

**Key APIs:**
- `Server::run_http()`
- `HttpClientTransport`

**Run:** `./build/release/example-feature-http-server-convenience`

**Expected output:** HTTP server started on port 18100, client connects and calls tool

---

### 12. OAuth Flow (`oauth_flow.cpp`)

**What it demonstrates:**
- Full OAuth flow with mock auth server
- `OAuthAuthenticator` with `InMemoryTokenStore`
- `OAuthClientTransport` for token injection
- Token refresh on auth failures
- `make_auth_middleware()` for server-side validation

**Key APIs:**
- `OAuthAuthenticator`
- `InMemoryTokenStore`
- `OAuthClientTransport`
- `make_auth_middleware()`
- `generate_pkce_pair()`

**Build:** `cmake -DBUILD_OAUTH_EXAMPLE=ON -B build && cmake --build build`

**Run:** `./build/example-feature-oauth-flow`

**Expected output:** OAuth discovery, token acquisition, injection, refresh on expiry

**Note:** This example is gated behind `BUILD_OAUTH_EXAMPLE` CMake option (OFF by default).

---

## Common Patterns

### Loopback Pattern (Non-Interactive Testing)

All examples use the loopback pattern for fast, deterministic testing:

```cpp
asio::io_context io_ctx;

// Create bidirectional memory transport pair
auto [server_transport, client_transport] = create_memory_transport_pair(io_ctx.get_executor());

// Run server and client in the same process
asio::co_spawn(io_ctx, server.run(server_transport, io_ctx.get_executor()), asio::detached);
asio::co_spawn(io_ctx, client_task(client_transport), asio::detached);

io_ctx.run();
```

This approach:
- ✅ Is fast (no network overhead)
- ✅ Is deterministic (no timing issues)
- ✅ Requires no external services
- ✅ Can run in CI without network access

### Async Handler Pattern

Async tool handlers with Context:

```cpp
server.add_tool("my_tool", "Description", schema,
    [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
        ctx.log_info("Starting work...");

        if (ctx.is_cancelled()) {
            co_return make_error_result("Cancelled");
        }

        co_await ctx.report_progress(50, 100);
        co_return make_success_result("Done");
    });
```

## Building Notes

### Build with Examples

```bash
python scripts/build.py --examples
```

### Build with OAuth Example

```bash
cmake -DBUILD_OAUTH_EXAMPLE=ON -B build && cmake --build build
```

### Build with Tests and Examples

```bash
python scripts/build.py --examples --test
```

## Troubleshooting

### Example hangs or times out

All examples have auto-exit timers (typically 2-5 seconds). If an example hangs:
- Check for missing server/client initialization
- Verify transport is properly connected
- Check for uncaught exceptions

### OAuth example not building

Ensure you're building with `BUILD_OAUTH_EXAMPLE=ON`:

```bash
cmake -DBUILD_OAUTH_EXAMPLE=ON -B build && cmake --build build
```

### Link errors

Ensure the mcp-cpp-sdk library is built first:

```bash
python scripts/init.py  # Install dependencies
python scripts/build.py --examples
```

## Next Steps

- See `docs/concepts/` for detailed explanations of each feature
- See `docs/guides/` for integration and deployment guides
- See `test/` for E2E tests using the same patterns
