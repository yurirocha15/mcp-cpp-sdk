# LLDB Debugger MCP Server

This is an MCP server that exposes LLDB (Low Level Debugger) capabilities through MCP tools, resources, and prompts. It provides programmatic access to debug sessions, breakpoints, threads, stack frames, and expression evaluation. It supports both stdio and HTTP transports, selectable at runtime via a `--transport` CLI flag.

### Prerequisites

- LLVM/LLDB development libraries (`liblldb-dev` on Ubuntu, `llvm` on macOS via Homebrew)
- CMake option `BUILD_LLDB_EXAMPLE=ON` to enable building this example (disabled by default)

### Building

```bash
# From the project root:
cmake -DBUILD_LLDB_EXAMPLE=ON -B build && cmake --build build
```

The `example-debugger-server` binary will be placed in `./build/`.

### Usage

**Stdio transport** (default — use with MCP clients that communicate over stdin/stdout):

```bash
./build/example-debugger-server
# or explicitly:
./build/example-debugger-server --transport=stdio
```

**HTTP transport** (starts an HTTP server that MCP clients can connect to):

```bash
./build/example-debugger-server --transport=http --port=9695
```

### CLI Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--transport=<stdio|http>` | `stdio` | Transport to use |
| `--host=<addr>` | `127.0.0.1` | HTTP listen address (HTTP mode only) |
| `--port=<n>` | `9695` | HTTP listen port (HTTP mode only) |
| `--help` | — | Show usage information |

### Available Tools

- **`lldb_command`** — Run raw LLDB command
  Input: `command` (string, required)

- **`launch`** — Launch program under LLDB
  Input: `program` (string, required), `args` (array of strings, optional), `stop_at_entry` (boolean, optional)

- **`attach`** — Attach to existing process
  Input: `pid` (integer, optional), `name` (string, optional)

- **`set_breakpoint`** — Set file/line or function breakpoint
  Input: `file` (string, optional), `line` (integer, optional), `function_name` (string, optional), `condition` (string, optional)

- **`continue_process`** — Continue current process
  Input: (no parameters)

- **`step`** — Step selected thread
  Input: `thread_index` (integer, optional), `type` (string `over|into|out`, optional)

- **`evaluate`** — Evaluate expression
  Input: `expression` (string, required), `thread_index` (integer, optional), `frame_index` (integer, optional)

- **`backtrace`** — Collect thread backtrace
  Input: `thread_index` (integer, optional), `count` (integer, optional)

### Available Resources

- **`lldb://targets`** — All debugger targets
- **`lldb://threads`** — Threads in current process
- **`lldb://breakpoints`** — Breakpoints in current target

### Available Resource Templates

- **`lldb://thread/{id}`** — Thread Details (discovery only)
- **`lldb://frame/{thread_id}/{frame_index}`** — Stack Frame (discovery only)

### Available Prompts

- **`debug_session`** — Generate a debugging session prompt
  Arguments: `program` (required, string), `issue` (optional, string)

### Example — HTTP Mode with curl

```bash
# Start the adapter in HTTP mode
./build/example-debugger-server --transport=http --port=9695

# Initialize the connection
curl -X POST http://127.0.0.1:9695/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2025-11-25" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"curl-client","version":"1.0"}}}'

# List available tools
curl -X POST http://127.0.0.1:9695/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2025-11-25" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

# Launch a program
curl -X POST http://127.0.0.1:9695/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2025-11-25" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"launch","arguments":{"program":"/path/to/executable"}}}'

# Read targets resource
curl -X POST http://127.0.0.1:9695/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2025-11-25" \
  -d '{"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"lldb://targets"}}'
```

### Architecture Notes

The server uses a single-threaded design with one SBTarget at a time. LLDB lifecycle is managed via RAII with the LLDBGuard struct, which calls `SBDebugger::Initialize()` on construction and `SBDebugger::Terminate()` on destruction. This ensures proper initialization and cleanup of the LLDB framework. The debugger session maintains state across tool calls, allowing users to launch a program, set breakpoints, step through code, and inspect state in sequence.
