# LLDB Debugger MCP Server

This MCP server exposes LLDB (Low Level Debugger) capabilities through MCP tools, resources, and prompts. It gives access to debug sessions, breakpoints, threads, stack frames, and expression evaluation. It supports both stdio and HTTP transports, selectable at runtime with a `--transport` CLI flag.

### Prerequisites

- LLVM/LLDB development libraries (`liblldb-dev` on Ubuntu, `llvm` on macOS via Homebrew)

The example is built with the other examples. It links LLDB when CMake finds it (an
LLDB CMake package, or `liblldb` together with `lldb/API/LLDB.h`); otherwise it is
built against stubs, and its tools answer that the debugger is not valid.

### Building

```bash
# From the project root:
python scripts/build.py --examples
```

The `example-debugger-server` binary is placed in `./build/release/`.

### Usage

**Stdio transport** (default; use with MCP clients that communicate over stdin/stdout):

```bash
./build/release/example-debugger-server
# or explicitly:
./build/release/example-debugger-server --transport=stdio
```

**HTTP transport** (starts an HTTP server that MCP clients can connect to):

```bash
./build/release/example-debugger-server --transport=http --port=9695
```

### CLI Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--transport=<stdio|http>` | `stdio` | Transport to use |
| `--host=<addr>` | `127.0.0.1` | HTTP listen address (HTTP mode only) |
| `--port=<n>` | `9695` | HTTP listen port (HTTP mode only) |
| `--help` | none | Show usage information |

### Available Tools

- **`lldb_command`**: Run raw LLDB command
  Input: `command` (string, required)

- **`launch`**: Launch program under LLDB
  Input: `program` (string, required), `args` (array of strings, optional), `stop_at_entry` (boolean, optional)

- **`attach`**: Attach to existing process
  Input: `pid` (integer, optional), `name` (string, optional)

- **`set_breakpoint`**: Set file/line or function breakpoint
  Input: `file` (string, optional), `line` (integer, optional), `function_name` (string, optional), `condition` (string, optional)

- **`continue_process`**: Continue current process
  Input: (no parameters)

- **`step`**: Step selected thread
  Input: `thread_index` (integer, optional), `type` (string `over|into|out`, optional)

- **`evaluate`**: Evaluate expression
  Input: `expression` (string, required), `thread_index` (integer, optional), `frame_index` (integer, optional)

- **`backtrace`**: Collect thread backtrace
  Input: `thread_index` (integer, optional), `count` (integer, optional)

### Available Resources

- **`lldb://targets`**: All debugger targets
- **`lldb://threads`**: Threads in current process
- **`lldb://breakpoints`**: Breakpoints in current target

### Available Resource Templates

- **`lldb://thread/{id}`**: Thread Details (discovery only)
- **`lldb://frame/{thread_id}/{frame_index}`**: Stack Frame (discovery only)

### Available Prompts

- **`debug_session`**: Generate a debugging session prompt
  Arguments: `program` (required, string), `issue` (optional, string)

### Example — HTTP Mode with curl

Each request below is a complete protocol revision 2026-07-28 request: it names its
version in the `MCP-Protocol-Version` header and in `_meta`, and needs no
`initialize`. The `Mcp-Method` header repeats the method, and `Mcp-Name` the tool
name of a `tools/call` or the URI of a `resources/read`. The debugger keeps its
state (the current target and process) between requests.

```bash
# Start the adapter in HTTP mode
./build/release/example-debugger-server --transport=http --port=9695

META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}'
HEADERS=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream"
         -H "MCP-Protocol-Version: 2026-07-28")

# List available tools
curl -X POST http://127.0.0.1:9695/mcp "${HEADERS[@]}" -H "Mcp-Method: tools/list" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{$META}}"

# Launch a program
curl -X POST http://127.0.0.1:9695/mcp "${HEADERS[@]}" -H "Mcp-Method: tools/call" -H "Mcp-Name: launch" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"launch\",\"arguments\":{\"program\":\"/path/to/executable\"},$META}}"

# Read targets resource
curl -X POST http://127.0.0.1:9695/mcp "${HEADERS[@]}" -H "Mcp-Method: resources/read" -H "Mcp-Name: lldb://targets" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\",\"params\":{\"uri\":\"lldb://targets\",$META}}"
```

A client of protocol revision 2025-11-25 or earlier starts a session instead:
`initialize`, then `notifications/initialized`, then its requests, each carrying the
`MCP-Session-Id` header that the `initialize` response set.

### Architecture Notes

The server uses one SBTarget at a time. LLDB lifecycle is managed via RAII with the
LLDBGuard struct, which calls `SBDebugger::Initialize()` on construction and
`SBDebugger::Terminate()` on destruction. The debugger session keeps its state across
tool calls, so a client can launch a program, set breakpoints, step through code, and
inspect state in sequence.

LLDB calls block: `continue_process` and `step` wait up to 30 seconds for the process
to stop. `Server::set_handler_executor()` runs every tool handler on one dedicated
thread, and the resource handlers hop to the same thread, so the thread that reads and
answers requests keeps serving while a handler waits, and only that one thread touches
the LLDB objects.
