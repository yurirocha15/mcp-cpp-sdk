# llama.cpp MCP Server Adapter

This is an MCP server that acts as an adapter for llama.cpp's `llama-server` HTTP API. It exposes language model capabilities (chat, text completion, and embedding generation) as MCP tools, and the list of loaded models as an MCP resource. It supports both stdio and HTTP transports, selectable at runtime via a `--transport` CLI flag.

### Prerequisites

- A running instance of [llama.cpp](https://github.com/ggml-org/llama.cpp)'s `llama-server` (default: `http://127.0.0.1:8080`)
- The server must be reachable from where this adapter runs

### Building

```bash
# From the project root:
make build
```

The `example-llama-mcp` binary will be placed in `./build/`.

### Usage

**Stdio transport** (default — use with MCP clients that communicate over stdin/stdout):

```bash
./build/example-llama-mcp
# or explicitly:
./build/example-llama-mcp --transport=stdio
```

**HTTP transport** (starts an HTTP server that MCP clients can connect to):

```bash
./build/example-llama-mcp --transport=http --port=8808
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

```bash
# Start the adapter in HTTP mode
./build/example-llama-mcp --transport=http --port=8808

# List available tools
curl -X POST http://127.0.0.1:8808/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'

# Call the chat tool
curl -X POST http://127.0.0.1:8808/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"chat","arguments":{"messages":[{"role":"user","content":"Hello!"}]}}}'
```

### Architecture Notes

Each call to llama-server opens a fresh TCP connection — there is no connection pooling. This keeps the example simple and matches the patterns used in the MCP C++ SDK's other examples.
