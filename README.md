# mcp-cpp-sdk

A modern C++20 implementation of the Model Context Protocol (MCP), enabling seamless integration between LLM applications and external tools, resources, and prompts.

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Build Status](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml)
[![Conformance baseline](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/conformance.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/conformance.yml)

## Why mcp-cpp-sdk?

- **Modern C++20**: Coroutine-based asynchronous I/O with Boost.Asio.
- **Shared or Static**: The same public API is available through explicit CMake targets for either linkage model.
- **Typed Protocol Models**: Strongly typed models for the supported MCP surface, with `nlohmann/json` interoperability.
- **Flexible Transports**: Native support for Stdio, WebSocket, and Streamable HTTP.
- **Measured Interoperability**: Official client and server conformance suites run in CI against a pinned `2025-11-25` regression baseline, with unsupported scenarios kept visible. A green baseline means no drift, not that an SDK tier has been achieved.

### How this implementation differs

The SDK combines a compiled client/server/transport runtime with header-based
protocol models and typed handler templates. Ordinary tool return values are
normalized into protocol-valid structured results, while complete raw
`CallToolResult` values use an explicit API. Interoperability and performance
claims are kept reproducible through the pinned conformance baseline and the
shared-workload benchmark adapters in [`benchmark/`](benchmark/).

## Quick Start

### 1. Installation

Package-manager routes are documented in [the installation guide](docs/installation.rst). A route is supported only after the matching GitHub Release record marks it `LIVE`; an upload or open registry PR alone is not availability proof.

The easiest way to use the SDK is via CMake's `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/yurirocha15/mcp-cpp-sdk.git
    # Replace with a verified release tag or full 40-character commit.
    GIT_TAG <verified-tag-or-commit>
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp::sdk)
```

### 2. Create a Minimal Server

```cpp
#include <mcp/mcp.hpp>

int main() {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    mcp::Server server({"hello-server", "1.0.0"}, capabilities);

    server.add_tool("hello", "Greets the user",
        {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}},
        [](const nlohmann::json& args) -> nlohmann::json {
            return nlohmann::json{
                {"message", "Hello, " + args["name"].get<std::string>() + "!"}};
        });

    server.run_http("127.0.0.1", 3000); // Serves http://127.0.0.1:3000/mcp
}
```

### 3. Create a Minimal Client

```cpp
#include <mcp/client/client.hpp>
#include <mcp/transport/http_client.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <iostream>
#include <memory>

int main() {
    boost::asio::io_context io;
    auto transport = std::make_shared<mcp::HttpClientTransport>(
        io.get_executor(), "http://127.0.0.1:3000/mcp");
    mcp::Client client(transport, io.get_executor());

    boost::asio::co_spawn(io, [&]() -> mcp::Task<void> {
        co_await client.connect("my-client", "1.0.0");
        auto result =
            co_await client.call_tool("hello", nlohmann::json{{"name", "World"}});
        std::cerr << nlohmann::json(result).dump(2) << '\n';
        client.close();
    }, boost::asio::detached);

    io.run();
}
```

## Usage Highlights

### Tools, Resources, and Prompts

Resources and prompts use the same typed-handler model as tools: provide the
protocol metadata (`mcp::Resource`, `mcp::ResourceTemplate`, or `mcp::Prompt`)
and a handler whose input and output are serializable protocol types. See the
[stdio server example](examples/servers/stdio/server_stdio.cpp) for complete,
compiled registrations.

### Server Context (Logging & Progress)

Async handlers have access to a `Context` for real-time interaction:

```cpp
server.add_tool<nlohmann::json, nlohmann::json>("long_task", "A task with progress", schema,
    [](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
        co_await ctx.log_info("Starting work...");
        co_await ctx.report_progress(50, 100);
        co_return nlohmann::json{{"status", "done"}};
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
| `--conformance` | Build fixtures for the official MCP conformance runner |
| `--linkage {both,shared,static}` | Select which SDK linkage variants to build |
| `--cppstd {20,23}` | Select the C++ consumer standard (default: C++20) |
| `--sanitize` | Build with ASan/UBSan (Linux/macOS) |
| `--docs` | Generate local Doxygen + Sphinx documentation |
| `--clean` | Clean build artifacts |

### Code Quality & Standards

- **Formatting**: `make format` (requires `clang-format`)
- **Linting**: `make lint` (requires `clang-tidy`)
- **Testing**: We use GoogleTest for all unit and integration tests.

### Contributing

Please see the [CONTRIBUTING guide](docs/contributing.rst) for the full process.

Project maintenance commitments and release gates are documented in the
[maintenance policy](MAINTENANCE.md), [dependency policy](DEPENDENCY_POLICY.md),
[versioning policy](VERSIONING.md), [Tier roadmap](ROADMAP.md), and
[security policy](SECURITY.md).

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.

## Package Hosting

[![OSS hosting by Cloudsmith](https://img.shields.io/badge/OSS%20hosting%20by-cloudsmith-blue?logo=cloudsmith&style=flat-square)](https://cloudsmith.com)

Package repository hosting is graciously provided by [Cloudsmith](https://cloudsmith.com).
Cloudsmith is the only fully hosted, cloud-native, universal package management solution that
enables your organization to create, store and share packages in any format, to any place, with total
confidence.
