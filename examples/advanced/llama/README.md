# llama.cpp MCP Server Adapter

This is an MCP server that acts as an adapter for llama.cpp's `llama-server` HTTP API. It exposes language model capabilities (chat, text completion, and embedding generation) as MCP tools, and the list of loaded models as an MCP resource. It supports both stdio and HTTP transports, selectable at runtime via a `--transport` CLI flag.

### Prerequisites

- A running instance of [llama.cpp](https://github.com/ggml-org/llama.cpp)'s `llama-server` (default: `http://127.0.0.1:8080`)
- The server must be reachable from where this adapter runs

### Building

```bash
# From the project root:
python scripts/build.py --examples
```

The `example-llama-mcp` binary is placed in `./build/release/`.

### Usage

**Stdio transport** (default — use with MCP clients that communicate over stdin/stdout):

```bash
./build/release/example-llama-mcp
# or explicitly:
./build/release/example-llama-mcp --transport=stdio
```

**HTTP transport** (starts an HTTP server that MCP clients can connect to):

```bash
./build/release/example-llama-mcp --transport=http --port=8808
```

### CLI Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--transport=<stdio|http>` | `stdio` | Transport to use |
| `--host=<addr>` | `127.0.0.1` | HTTP listen address (HTTP mode only) |
| `--port=<n>` | `8808` | HTTP listen port (HTTP mode only) |
| `--llama-host=<addr>` | `127.0.0.1` | llama-server address |
| `--llama-port=<n>` | `8080` | llama-server port |
| `--help` | — | Show usage information |

### Available Tools

- **`chat`** — Send a chat message to the model. Posts to `/v1/chat/completions`.
  Input: `messages` (array of `{role, content}`, required), `temperature` (number, optional), `max_tokens` (integer, optional)

- **`complete`** — Generate a text completion. Posts to `/v1/completions`.
  Input: `prompt` (string, required), `temperature` (number, optional), `max_tokens` (integer, optional)

- **`embed`** — Generate an embedding vector. Posts to `/v1/embeddings`.
  Input: `input` (string, required)

### Available Resources

- **`llama://models`** — List the models currently loaded in llama-server. Fetches `/v1/models`.

### Available Prompts

- **`summarize`** — Returns a summarization prompt for the given text.
  Argument: `text` (string, required)

### Example — HTTP Mode with curl

Each request below is a complete protocol revision 2026-07-28 request: it names its
version in the `MCP-Protocol-Version` header and in `_meta`, and needs no
`initialize`. The `Mcp-Method` header repeats the method, and `Mcp-Name` the tool
name of a `tools/call`; the server refuses a request whose headers are missing or
do not match its body.

```bash
# Start the adapter in HTTP mode
./build/release/example-llama-mcp --transport=http --port=8808

# List available tools
curl -X POST http://127.0.0.1:8808/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/list" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'

# Call the chat tool
curl -X POST http://127.0.0.1:8808/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: chat" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"chat","arguments":{"messages":[{"role":"user","content":"Hello!"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

A client of protocol revision 2025-11-25 or earlier starts a session instead:
`initialize`, then `notifications/initialized`, then its requests, each carrying the
`MCP-Session-Id` header that the `initialize` response set.

### Architecture Notes

The tool and resource handlers are coroutines that `co_await` llama-server without
blocking: while one waits for a model's reply, the server keeps reading and
answering other requests on the same thread. Each call to llama-server opens a
fresh TCP connection, with one 120-second deadline covering connecting, sending
and reading. A tool whose call fails (llama-server unreachable, an error status,
an unexpected reply) throws, and the SDK answers the call with an `isError`
result that carries the exception's message. `run_stdio()` and `run_http()` stop
the server on SIGINT and SIGTERM.
