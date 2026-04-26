# mcp-cpp-sdk

A modern C++20 implementation of the Model Context Protocol (MCP), enabling seamless integration between LLM applications and external tools, resources, and prompts.

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Build Status](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml)

## Why mcp-cpp-sdk?

- **Modern C++20**: Asynchronous first, leveraging coroutines (via Boost.Asio) for high-performance I/O.
- **Type-Safe Protocol**: Strong typing for all MCP messages using `nlohmann/json`.
- **Flexible Transports**: Native support for Stdio, WebSocket, and Streamable HTTP.
- **Full Specification**: Complete implementation of the latest MCP protocol (2025-11-25).

## Quick Start

### 1. Installation

The easiest way to use the SDK is via CMake's `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/yurirocha15/mcp-cpp-sdk.git
    GIT_TAG main
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-cpp-sdk)
```

### 2. Create a Minimal Server

```cpp
#include <mcp/mcp.hpp>

int main() {
    mcp::Server server({"hello-server", "1.0.0"}, {});

    server.add_tool("hello", "Greets the user",
        {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}},
        [](const nlohmann::json& args) {
            return {{"message", "Hello, " + args["name"].get<std::string>() + "!"}};
        });

    server.run_stdio(); // Blocks until connection closes
}
```

### 3. Create a Minimal Client

```cpp
#include <mcp/client.hpp>
#include <mcp/transport/stdio.hpp>

boost::asio::co_spawn(executor, [&]() -> mcp::Task<void> {
    auto transport = std::make_unique<mcp::StdioTransport>(executor);
    mcp::Client client(std::move(transport), executor);

    co_await client.connect({"my-client", "1.0.0"}, {});
    auto result = co_await client.call_tool("hello", {{"name", "World"}});
    std::cout << result.content.dump() << std::endl;
}, boost::asio::detached);
```

## Usage Highlights

### Tools, Resources, and Prompts

```cpp
// Register a read-only resource
server.add_resource("mcp://status", "System status", "text/plain", []() {
    return "All systems go.";
});

// Register a prompt template
server.add_prompt("greet", "Greets the user", {{"name", "User name"}}, [](const nlohmann::json& args) {
    return {{"messages", {{{"role", "user"}, {"content", {{"type", "text"}, {"text", "Hello, " + args["name"].get<std::string>()}}}}}}};
});
```

### Server Context (Logging & Progress)

Async handlers have access to a `Context` for real-time interaction:

```cpp
server.add_tool("long_task", "A task with progress", schema,
    [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
        ctx.log_info("Starting work...");
        co_await ctx.report_progress(50, 100);
        co_return {{"status", "done"}};
    });
```

## Documentation

For full guides, API reference, and integration details, visit our **[Documentation Site](https://yurirocha15.github.io/mcp-cpp-sdk)**.

- **[Getting Started](https://yurirocha15.github.io/mcp-cpp-sdk/getting-started.html)**: Detailed installation and build instructions.
- **[Core Concepts](https://yurirocha15.github.io/mcp-cpp-sdk/concepts/index.html)**: Deep dive into Tools, Resources, and Transports.
- **[Client App Integrations](https://yurirocha15.github.io/mcp-cpp-sdk/integrations/client-apps.html)**: How to connect your server to Claude, IDEs, and the MCP Inspector.
- **[Examples](https://yurirocha15.github.io/mcp-cpp-sdk/examples.html)**: Walkthrough of included example applications.

---

## For Developers

This section is for contributors and developers wanting to build, test, and contribute to `mcp-cpp-sdk` itself.

### Building from Source

To build the library, tests, and examples locally:

```bash
# Install dependencies
python scripts/init.py

# Build project (release with examples and tests)
python scripts/build.py --examples --test
```

### Build Commands Reference

| Flag | Description |
|------|-------------|
| `--debug` | Build in debug mode |
| `--test` | Build and run unit tests |
| `--examples` | Build example applications |
| `--sanitize` | Build with ASan/UBSan (Linux/macOS) |
| `--docs` | Generate local Doxygen + Sphinx documentation |
| `--clean` | Clean build artifacts |

### Code Quality & Standards

- **Formatting**: `make format` (requires `clang-format`)
- **Linting**: `make lint` (requires `clang-tidy`)
- **Testing**: We use GoogleTest for all unit and integration tests.

### Contributing

Please see the [CONTRIBUTING guide](docs/contributing.rst) for the full process.

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.
