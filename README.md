# mcp-cpp-sdk

A modern C++20 implementation of the Model Context Protocol (MCP), enabling seamless integration between LLM applications and external tools/resources.

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Build Status](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml)

## Features

- **MCP 2025-11-25 Protocol**: Full implementation of the latest Model Context Protocol specification
- **Triple Transport Support**: Stdio, WebSocket, and Streamable HTTP transports for flexible deployment scenarios
- **Streamable HTTP**: Client transport, server transport, and multi-session session manager for MCP over HTTP
- **OAuth 2.1 Support**: PKCE, token refresh, discovery, and transport wrapping for authenticated MCP clients
- **Server & Client**: Complete implementations for both server and client roles
- **Modern C++20**: Leverages coroutines via Boost.Asio for clean asynchronous code
- **Type-Safe Protocol**: Uses nlohmann/json with strong typing for all MCP messages
- **Cross-Platform**: Works on Linux, macOS, and Windows

## Installation

### Prerequisites

- **C++20 Compiler**: GCC 10+, Clang 11+, or MSVC 2019+
- **CMake**: 3.20 or later
- **Boost**: Managed via Conan for source builds
- **Conan**: 2.x package manager
- **Python**: 3.8+ (for build scripts)

### Quick Start

```bash
# Clone the repository
git clone https://github.com/yurirocha15/mcp-cpp-sdk.git
cd mcp-cpp-sdk

# Install dependencies (cross-platform)
python scripts/init.py

# Build the project
python scripts/build.py build

# Run tests
python scripts/build.py test
```

For development with additional tools (clang-format, clang-tidy, pre-commit):
```bash
python scripts/init.py --dev
```

For documentation generation:
```bash
python scripts/init.py --docs
python scripts/build.py docs
```

## Quick Start Examples

### Minimal MCP Server (Stdio)

```cpp
#include <mcp/mcp.hpp>

int main() {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};

    mcp::Implementation info{"hello-server", "1.0.0"};
    mcp::Server server(std::move(info), std::move(caps));

    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"name"})}};

    server.add_tool("hello", "Greets the user", std::move(schema),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return {{"message", "Hello, " + args.at("name").get<std::string>() + "!"}};
                    });

    server.run_stdio();
}
```

`server.run_stdio()` blocks until the client closes the connection.
The runtime, transport, and event loop are all managed internally.

<details>
<summary>Advanced: manual transport setup</summary>

```cpp
#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

int main() {
    boost::asio::io_context io_ctx;

    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};

    mcp::Implementation info;
    info.name = "hello-server";
    info.version = "1.0.0";

    mcp::Server server(std::move(info), std::move(caps));

    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"name"})},
    };

    server.add_tool<nlohmann::json, nlohmann::json>(
        "hello", "Greets the user", std::move(schema),
        [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
            co_return nlohmann::json{{"message", "Hello, " + args.at("name").get<std::string>() + "!"}};
        });

    auto transport = std::make_unique<mcp::StdioTransport>(io_ctx.get_executor());
    boost::asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            co_await server.run(std::move(transport), io_ctx.get_executor());
        },
        boost::asio::detached);

    io_ctx.run();
    return 0;
}
```

</details>

### Minimal MCP Client (Stdio)

```cpp
#include <mcp/client.hpp>
#include <mcp/transport/stdio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

int main() {
    boost::asio::io_context io_ctx;
    auto transport = std::make_unique<mcp::StdioTransport>(io_ctx.get_executor());
    mcp::Client client(std::move(transport), io_ctx.get_executor());

    boost::asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::Implementation client_info;
            client_info.name = "my-client";
            client_info.version = "1.0.0";

            mcp::ClientCapabilities caps;
            auto init = co_await client.connect(std::move(client_info), std::move(caps));

            auto tools = co_await client.list_tools();
            auto result = co_await client.call_tool("hello", nlohmann::json{{"name", "World"}});
            (void)init;
            (void)tools;
            (void)result;
        },
        boost::asio::detached);

    io_ctx.run();
    return 0;
}
```

## API Overview

The SDK is organized into the following headers:

- **`mcp/mcp.hpp`**: Umbrella header — includes everything needed for common use cases
- **`mcp/core.hpp`**: Core types (`Task<T>`, `LogLevel`, `LogHandler`)
- **`mcp/runtime.hpp`**: `mcp::Runtime` — manages the event loop for `run_stdio()` / `run_http()`
- **`mcp/concepts.hpp`**: C++20 concepts for transport and message handling
- **`mcp/protocol.hpp`**: MCP protocol types umbrella — includes all protocol sub-headers:
  - `protocol/base.hpp`: JSON-RPC wire types (`RequestId`, error codes, `JSONRPCRequest`/`JSONRPCResponse`)
  - `protocol/capabilities.hpp`: Client/server capabilities, `Implementation`, initialization handshake
  - `protocol/content.hpp`: Content types (`TextContent`, `ImageContent`, `AudioContent`, `ContentBlock`)
  - `protocol/tools.hpp`: Tool definitions and call types (`Tool`, `ToolCall`, `ListToolsResult`)
  - `protocol/resources.hpp`: Resource and resource template types (`Resource`, `ResourceTemplate`)
  - `protocol/prompts.hpp`: Prompt definitions and argument types (`Prompt`, `GetPromptResult`)
  - `protocol/completion.hpp`: Completion request/result types (`CompleteRequest`, `CompleteResult`)
  - `protocol/notification.hpp`: Notification and logging types (`LoggingLevel`, notification structs)
  - `protocol/roots.hpp`: Root directory/file definitions (`Root`, `ListRootsResult`)
  - `protocol/tasks.hpp`: Task CRUD operations (`TaskData`, `CreateTaskResult`, `ListTasksResult`)
  - `protocol/sampling.hpp`: LLM sampling message types (`SamplingMessage`, `CreateMessageRequest`)
  - `protocol/elicitation.hpp`: User elicitation request/result types (`ElicitRequest`, `ElicitResult`)
- **`mcp/transport.hpp`**: Abstract transport interface (`ITransport`)
- **`mcp/transport/stdio.hpp`**: Stdio transport implementation
- **`mcp/transport/websocket.hpp`**: WebSocket transport implementation
- **`mcp/transport/http_server.hpp`**: Streamable HTTP server transport
- **`mcp/transport/http_client.hpp`**: Streamable HTTP client transport
- **`mcp/transport/http_session_manager.hpp`**: Multi-session Streamable HTTP endpoint manager
- **`mcp/auth/oauth.hpp`**: OAuth 2.1 helpers and authenticated transport wrapper
- **`mcp/context.hpp`**: Context for handling server-side requests
- **`mcp/server.hpp`**: MCP server implementation
- **`mcp/client.hpp`**: MCP client implementation

Full API documentation is available at [https://yurirocha15.github.io/mcp-cpp-sdk](https://yurirocha15.github.io/mcp-cpp-sdk) (or generate locally with `make docs`).

## Examples

The repository includes several complete examples:

- **`examples/server_simple.cpp`**: Minimal stdio server using the convenience API (zero Boost headers)
- **`examples/server_stdio.cpp`**: Full-featured stdio server with tools, resources, and prompts
- **`examples/client_stdio.cpp`**: Stdio client demonstrating initialization and tool calls
- **`examples/server_with_sampling.cpp`**: Server showcasing LLM sampling integration
- **`examples/echo_websocket.cpp`**: WebSocket echo server for testing transport
- **`examples/http_loopback.cpp`**: HTTP transport loopback demonstrating Streamable HTTP
- **`examples/llama/llama_server.cpp`**: MCP adapter for llama.cpp's llama-server (chat, completion, embedding)
- **`examples/debugger/debugger_server.cpp`**: LLDB debugger MCP server (requires `-DBUILD_LLDB_EXAMPLE=ON`)

Build and run examples:
```bash
python scripts/build.py build
./build/example-server-simple
```

## Building & Testing

### Build Commands

```bash
python scripts/build.py build     # Release build with optimizations
python scripts/build.py debug     # Debug build with symbols
python scripts/build.py test      # Run all tests
python scripts/build.py sanitize  # Build with ASan + UBSan, run tests
python scripts/build.py coverage  # Generate coverage report
python scripts/build.py clean     # Clean build artifacts
```

### Running Tests

```bash
# Run the full test suite
python scripts/build.py test

# Run with verbose output
ctest --preset conan-release --verbose
```

### Code Quality

```bash
# Format code
make format

# Run static analysis (requires clang-tidy)
make lint
```

## Documentation

Documentation is built using Doxygen + Sphinx + Breathe:

```bash
# Install documentation dependencies
python scripts/init.py --docs

# Generate HTML documentation
python scripts/build.py docs

# Open in browser
open build/docs/html/index.html
```

Documentation includes:
- Getting Started Guide
- Full API Reference (auto-generated from code)
- Architecture Overview
- Examples Walkthrough
- Contributing Guidelines

## Authentication

The SDK includes OAuth 2.1 support for authenticated MCP clients in
`mcp/auth/oauth.hpp`.

```cpp
#include <mcp/auth/oauth.hpp>
#include <mcp/transport/http_client.hpp>

auto inner = std::make_unique<mcp::HttpClientTransport>(executor, "http://localhost:8080/mcp");
auto token_store = std::make_shared<mcp::auth::InMemoryTokenStore>();
auto oauth_http = std::make_shared<mcp::auth::OAuthHttpClient>(executor);

mcp::auth::OAuthConfig config{
    .client_id = "my-client",
    .token_endpoint = "https://issuer.example/token",
    .redirect_uri = "http://localhost/callback",
};

auto authenticator = std::make_shared<mcp::auth::OAuthAuthenticator>(
    token_store, oauth_http, std::move(config), "http://localhost:8080/mcp");

auto transport = std::make_unique<mcp::auth::OAuthClientTransport>(
    std::move(inner), authenticator);
```

For multi-session HTTP servers, use `mcp/transport/http_session_manager.hpp`.
The canonical session header name used throughout the docs is `MCP-Session-Id`.

## Contributing

Contributions are welcome! Please see [docs/contributing.rst](docs/contributing.rst) for:
- Code style guidelines
- Testing requirements
- Pull request process
- Development workflow

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built on the [Model Context Protocol](https://modelcontextprotocol.io/) specification
- Uses [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/) for async I/O
- JSON handling via [nlohmann/json](https://github.com/nlohmann/json)
- Testing with [GoogleTest](https://github.com/google/googletest)

## Support

- **Issues**: [GitHub Issues](https://github.com/yurirocha15/mcp-cpp-sdk/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yurirocha15/mcp-cpp-sdk/discussions)
- **Documentation**: [https://yurirocha15.github.io/mcp-cpp-sdk](https://yurirocha15.github.io/mcp-cpp-sdk)
