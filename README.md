# mcp-cpp-sdk

A C++20 SDK for the [Model Context Protocol](https://modelcontextprotocol.io) (MCP). Build servers that give LLM applications tools, resources and prompts, and clients that use them.

[![Release](https://img.shields.io/github/v/release/yurirocha15/mcp-cpp-sdk)](https://github.com/yurirocha15/mcp-cpp-sdk/releases/latest)
[![Docs](https://img.shields.io/badge/docs-online-blue.svg)](https://www.yurirocha.com/mcp-cpp-sdk/)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Build Status](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml)
[![Conformance baseline](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/conformance.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/conformance.yml)

## Why mcp-cpp-sdk?

- **C++20 coroutines**: Asynchronous I/O on Boost.Asio, written with `co_await`.
- **Typed protocol models**: MCP messages are C++ types that convert to and from `nlohmann/json`.
- **Three transports**: Stdio, WebSocket and Streamable HTTP.
- **Protocol revisions**: A `Server` serves revision `2026-07-28` alongside `2025-11-25` and earlier on the same transport. A `Client` detects which one the server speaks when it connects.
- **Tested against the spec**: The official MCP conformance suites run in CI against protocol `2025-11-25`, and [known gaps](conformance/expected-failures.yml) are listed. This is not an official SDK tier rating.
- **Fast**: Throughput and latency on par with the official Rust SDK in our [benchmark](benchmark/RESULTS.md).

## Quick Start

### 1. Installation

| Platform | How |
|---|---|
| Ubuntu 22.04 / 24.04 / 26.04, Debian 12 / 13 | [APT repository](https://www.yurirocha.com/mcp-cpp-sdk/installation.html#apt-deb) |
| Fedora 43 / 44, Enterprise Linux 9 / 10 | [RPM repository](https://www.yurirocha.com/mcp-cpp-sdk/installation.html#rpm) |
| Arch Linux | AUR package [`mcp-cpp-sdk`](https://aur.archlinux.org/packages/mcp-cpp-sdk) |
| macOS / Linux | `brew install yurirocha15/mcp-cpp-sdk/mcp-cpp-sdk` |
| Windows x64 | SDK zip from the [latest release](https://github.com/yurirocha15/mcp-cpp-sdk/releases/latest) |
| Any platform | CMake `FetchContent`, below |

The [installation guide](https://www.yurirocha.com/mcp-cpp-sdk/installation.html)
has the full commands and the CMake and package names.

Before 1.0, releases are not binary compatible with each other, so rebuild your
project when you change versions.

To build the SDK as part of your project with CMake's `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/yurirocha15/mcp-cpp-sdk.git
    GIT_TAG v0.2.0
)
# Skip the SDK's own tests, examples and docs.
set(BUILD_TESTING OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_DOCS OFF)
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
        mcp::ConnectResult connected = co_await client.connect("my-client", "1.0.0");
        std::cerr << "Connected using protocol " << connected.protocolVersion << '\n';
        nlohmann::json arguments{{"name", "World"}};
        auto result = co_await client.call_tool("hello", arguments);
        std::cerr << nlohmann::json(result).dump(2) << '\n';
        client.close();
    }, boost::asio::detached);

    io.run();
}
```

`Client` defaults to `mcp::ClientProtocol::Auto()`: it sends `server/discover`
first and falls back to `initialize` when the server does not answer as a
2026-07-28 server. `connect()` returns a `mcp::ConnectResult` holding the
session's `era`, the `protocolVersion` in use, the server's `capabilities`, and,
when the server sent them, `serverInfo`, `instructions` and `supportedVersions`.
To skip detection, set `ClientOptions::protocol` to
`mcp::ClientProtocol::Legacy()` or `mcp::ClientProtocol::Version("2026-07-28")`.

Note the named `arguments` variable. See [Compiler Notes](#compiler-notes) for
why it is not built inline.

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

Guides, API reference and integration details are on the **[documentation site](https://www.yurirocha.com/mcp-cpp-sdk)**.

- **[Getting Started](https://www.yurirocha.com/mcp-cpp-sdk/getting-started.html)**: Detailed installation and build instructions.
- **[Core Concepts](https://www.yurirocha.com/mcp-cpp-sdk/concepts/index.html)**: Deep dive into Tools, Resources, and Transports.
- **[Client App Integrations](https://www.yurirocha.com/mcp-cpp-sdk/integrations/client-apps.html)**: How to connect your server to Claude, IDEs, and the MCP Inspector.
- **[Examples](https://www.yurirocha.com/mcp-cpp-sdk/examples.html)**: Walkthrough of included example applications.

## Compiler Notes

### GCC 12 and 13: initializer lists inside a `co_await` expression

GCC 12 and GCC 13 crash with an internal compiler error when a `co_await`
expression contains an initializer list with non-trivial elements. GCC 13 is the
default compiler on Ubuntu 24.04, so this is easy to hit. Build the value into a
named variable first:

```cpp
// Crashes GCC 12 and GCC 13
auto result = co_await client.call_tool("hello", nlohmann::json{{"name", "World"}});
auto prompt = co_await client.get_prompt("greet", std::map<std::string, std::string>{{"who", "you"}});

// Compiles
nlohmann::json arguments{{"name", "World"}};
auto result = co_await client.call_tool("hello", arguments);
```

This is a compiler bug, not an SDK one.

### GCC 11: temporaries inside a `co_await`

GCC releases before 13 can destroy a temporary created inside a `co_await`
expression twice
([GCC bug 101367](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101367)). With
GCC 11 a braced aggregate argument such as `mcp::Implementation{...}` crashes
the program. Build such a value as a named variable before the `co_await`:

```cpp
// Crashes with GCC 11
co_await client.connect(mcp::Implementation{"my-client", "1.0.0"}, capabilities);

// Works
mcp::Implementation client_info{"my-client", "1.0.0"};
co_await client.connect(client_info, capabilities);
```

The contributing guide's
[Known Issues](https://www.yurirocha.com/mcp-cpp-sdk/contributing.html#known-issues)
section has the full list of affected code for both compiler bugs.

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

Project policies: [maintenance](MAINTENANCE.md), [dependencies](DEPENDENCY_POLICY.md),
[versioning](VERSIONING.md), [security](SECURITY.md) and the [roadmap](ROADMAP.md).

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.

## Package Hosting

[![OSS hosting by Cloudsmith](https://img.shields.io/badge/OSS%20hosting%20by-cloudsmith-blue?logo=cloudsmith&style=flat-square)](https://cloudsmith.com)

Package repository hosting is graciously provided by [Cloudsmith](https://cloudsmith.com).
Cloudsmith is the only fully hosted, cloud-native, universal package management solution that
enables your organization to create, store and share packages in any format, to any place, with total
confidence.
