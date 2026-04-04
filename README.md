# mcp-cpp-sdk

A modern C++20 implementation of the Model Context Protocol (MCP), enabling seamless integration between LLM applications and external tools/resources.

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Build Status](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml)

## Features

- **MCP 2025-11-25 Protocol**: Full implementation of the latest Model Context Protocol specification
- **Triple Transport Support**: Stdio, WebSocket, and Streamable HTTP transports for flexible deployment scenarios
- **Streamable HTTP**: Streamable HTTP transport (MCP 2025-11-25) for REST-style MCP communication
- **Server & Client**: Complete implementations for both server and client roles
- **Modern C++20**: Leverages coroutines via Boost.Asio for clean asynchronous code
- **Type-Safe Protocol**: Uses nlohmann/json with strong typing for all MCP messages
- **Cross-Platform**: Works on Linux, macOS, and Windows

## Installation

### Prerequisites

- **C++20 Compiler**: GCC 10+, Clang 11+, or MSVC 2019+
- **CMake**: 3.25 or later
- **Boost**: 1.82 or later (managed via Conan)
- **Conan**: 2.x package manager
- **Python**: 3.8+ (for build scripts)

### Quick Start

```bash
# Clone the repository
git clone https://github.com/yurirocha15/mcp-cpp-sdk.git
cd mcp-cpp-sdk

# Install dependencies (cross-platform)
make init

# Build the project
make build

# Run tests
make test
```

For development with additional tools (clang-format, clang-tidy, pre-commit):
```bash
make init-dev
```

For documentation generation:
```bash
make init-docs
make docs
```

## Quick Start Examples

### Minimal MCP Server (Stdio)

```cpp
#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>

mcp::Task<void> run_server() {
    auto transport = std::make_unique<mcp::StdioServerTransport>();
    mcp::Server server(std::move(transport));

    // Register a simple tool
    server.add_tool("hello", "Greets the user",
        [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
            co_return nlohmann::json{{"message", "Hello, World!"}};
        });

    co_await server.run();
}
```

### Minimal MCP Client (Stdio)

```cpp
#include <mcp/client.hpp>
#include <mcp/transport/stdio.hpp>

mcp::Task<void> run_client() {
    auto transport = std::make_unique<mcp::StdioClientTransport>(
        "./server", std::vector<std::string>{});
    mcp::Client client(std::move(transport));

    // Initialize connection
    co_await client.initialize({{"name", "my-client"}});

    // List available tools
    auto tools = co_await client.list_tools();

    // Call a tool
    auto result = co_await client.call_tool("hello", {});
}
```

## API Overview

The SDK is organized into the following headers:

- **`mcp/core.hpp`**: Core types (`Task<T>`, executor infrastructure)
- **`mcp/concepts.hpp`**: C++20 concepts for transport and message handling
- **`mcp/protocol.hpp`**: Complete MCP protocol types and JSON serialization
- **`mcp/transport.hpp`**: Abstract transport interface (`ITransport`)
- **`mcp/transport/stdio.hpp`**: Stdio transport implementation
- **`mcp/transport/websocket.hpp`**: WebSocket transport implementation
- **`mcp/transport/http_server.hpp`**: Streamable HTTP server transport
- **`mcp/transport/http_client.hpp`**: Streamable HTTP client transport
- **`mcp/context.hpp`**: Context for handling server-side requests
- **`mcp/server.hpp`**: MCP server implementation
- **`mcp/client.hpp`**: MCP client implementation

Full API documentation is available at [https://yurirocha15.github.io/mcp-cpp-sdk](https://yurirocha15.github.io/mcp-cpp-sdk) (or generate locally with `make docs`).

## Examples

The repository includes several complete examples:

- **`examples/server_stdio.cpp`**: Full-featured stdio server with tools, resources, and prompts
- **`examples/client_stdio.cpp`**: Stdio client demonstrating initialization and tool calls
- **`examples/server_with_sampling.cpp`**: Server showcasing LLM sampling integration
- **`examples/echo_websocket.cpp`**: WebSocket echo server for testing transport
- **`examples/http_loopback.cpp`**: HTTP transport loopback demonstrating Streamable HTTP
- **`examples/llama/llama_server.cpp`**: MCP adapter for llama.cpp's llama-server (chat, completion, embedding)
- **`examples/debugger/debugger_server.cpp`**: LLDB debugger MCP server (requires `-DBUILD_LLDB_EXAMPLE=ON`)

Build and run examples:
```bash
make build
./build/example-server-stdio
```

## Building & Testing

### Build Commands

```bash
make build        # Release build with optimizations
make debug        # Debug build with symbols
make test         # Run all tests
make coverage     # Generate coverage report
make clean        # Clean build artifacts
```

### Running Tests

```bash
# Run all 160 tests
make test

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
make init-docs

# Generate HTML documentation
make docs

# Open in browser
open build/docs/html/index.html
```

Documentation includes:
- Getting Started Guide
- Full API Reference (auto-generated from code)
- Architecture Overview
- Examples Walkthrough
- Contributing Guidelines

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
