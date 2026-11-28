# mcp-cpp-sdk

A C++20 SDK for the [Model Context Protocol](https://modelcontextprotocol.io) (MCP). Build servers that give LLM applications tools, resources and prompts, and clients that use them.

[![Release](https://img.shields.io/github/v/release/yurirocha15/mcp-cpp-sdk)](https://github.com/yurirocha15/mcp-cpp-sdk/releases/latest)
[![Docs](https://img.shields.io/badge/docs-online-blue.svg)](https://mcp-cpp-sdk.yurirocha.com/)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Build Status](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/yurirocha15/mcp-cpp-sdk/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/yurirocha15/mcp-cpp-sdk/graph/badge.svg)](https://app.codecov.io/gh/yurirocha15/mcp-cpp-sdk)

## Why mcp-cpp-sdk?

- **C++20 coroutines**: Asynchronous I/O on Boost.Asio, written with `co_await`. A sync mode, with plain-function handlers and blocking calls, is also available for simple programs and rapid prototyping.
- **Typed protocol models**: MCP messages are C++ types that convert to and from `nlohmann/json`.
- **Three transports**: Stdio, WebSocket and Streamable HTTP.
- **Protocol revisions**: A `Server` serves revision `2026-07-28` alongside `2025-11-25` and earlier on the same transport. A `Client` detects which one the server speaks when it connects.
- **Tested against the spec**: The official MCP conformance suites run in CI against protocol `2025-11-25`, and [known gaps](conformance/expected-failures.yml) are listed. A separate job reports the alpha runner's `2026-07-28` scenarios without gating. This is not an official SDK tier rating.
- **Fast**: Throughput and latency on par with the official Rust SDK in our [benchmark](benchmark/RESULTS.md).

## Quick Start

### 1. Installation

| Platform | How |
|---|---|
| Ubuntu 22.04 / 24.04 / 26.04, Debian 12 / 13 | [APT repository](https://mcp-cpp-sdk.yurirocha.com/installation/ubuntu-debian.html) |
| Fedora 43 / 44 | [RPM repository](https://mcp-cpp-sdk.yurirocha.com/installation/fedora.html) |
| AlmaLinux, Rocky Linux, RHEL 9 / 10 | [RPM repository](https://mcp-cpp-sdk.yurirocha.com/installation/enterprise-linux.html) |
| Arch Linux | AUR package [`mcp-cpp-sdk`](https://mcp-cpp-sdk.yurirocha.com/installation/arch.html) |
| macOS / Linux | `brew install yurirocha15/mcp-cpp-sdk/mcp-cpp-sdk` |
| Windows x64 | [`choco install mcp-cpp-sdk`](https://mcp-cpp-sdk.yurirocha.com/installation/windows.html) |
| Any platform | CMake `FetchContent`, below |

The [installation guide](https://mcp-cpp-sdk.yurirocha.com/installation.html)
has the full commands and the CMake and package names.

Before 1.0, releases are not binary compatible with each other, so rebuild your
project when you change versions.

To build the SDK as part of your project with CMake's `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/yurirocha15/mcp-cpp-sdk.git
    GIT_TAG v0.4.0
)
# Skip the SDK's own tests, examples and docs.
set(BUILD_TESTING OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_DOCS OFF)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp::sdk)
```

### 2. A Minimal Server

```cpp
#include <mcp/mcp.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    server.run_stdio();
}
```

Link it with `target_link_libraries(greeter PRIVATE mcp::sdk)`. MCP hosts launch it as a stdio
server.

### 3. A Minimal Client

```cpp
#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
}
```

`./greeter-client ./greeter` prints `Hello, Ada!`.

Both programs use the blocking API. The
[getting-started guide](https://mcp-cpp-sdk.yurirocha.com/getting-started.html) explains
them, and [Async Servers and Clients](https://mcp-cpp-sdk.yurirocha.com/guides/async-mode.html)
covers the coroutine API.

## Usage Highlights

### Async Mode

In async mode the program runs the event loop, an `mcp::Runtime`, and handlers and client calls
are coroutines that return `mcp::Task` (see
[Async Servers and Clients](https://mcp-cpp-sdk.yurirocha.com/guides/async-mode.html)). The
greeter in async mode:

```cpp
#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/stdio.hpp>

#include <memory>
#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                        co_return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    // Server::run() serves stdio on the event loop until standard input closes.
    mcp::Runtime runtime;
    runtime.spawn(
        server.run(std::make_shared<mcp::StdioTransport>(runtime.executor()), runtime.executor()));
    runtime.run();
}
```

Its client awaits each call in a coroutine on the same kind of loop:

```cpp
#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    mcp::Runtime runtime;
    mcp::StdioProcessOptions process;
    process.command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(std::make_shared<mcp::StdioClientTransport>(runtime.executor(), process),
                       runtime.executor());

    const auto greet = [&client]() -> mcp::Task<void> {
        co_await client.connect("greeter-client", "1.0.0");
        const nlohmann::json args{{"name", "Ada"}};
        const auto result = co_await client.call_tool("greet", args);
        std::cout << mcp::result_text(result) << "\n";
        client.close();
    };
    runtime.spawn(greet());
    runtime.run();
}
```

### Tools, Resources, and Prompts

Resources and prompts use the same typed-handler model as tools: provide the
protocol metadata (`mcp::Resource`, `mcp::ResourceTemplate`, or `mcp::Prompt`)
and a handler whose input and output are serializable protocol types. See the
[stdio server example](examples/servers/stdio/server_stdio.cpp) for complete,
compiled registrations.

### Server Context (Progress & Cancellation)

A handler that takes the request's `Context` reports progress and sees when the client cancels
the request:

```cpp
server.add_tool<nlohmann::json, nlohmann::json>("long_task", "A task with progress", schema,
    [](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
        co_await ctx.report_progress(50, 100);
        if (ctx.is_cancelled()) {
            co_return nlohmann::json{{"status", "cancelled"}};
        }
        co_return nlohmann::json{{"status", "done"}};
    });
```

### Streamable HTTP

`run_http()` serves any number of clients over Streamable HTTP until SIGINT or SIGTERM. A
callback it takes configures the endpoint before it listens, such as its allowed origins and
bearer token validation (see [Transports](https://mcp-cpp-sdk.yurirocha.com/concepts/transports.html)):

```cpp
server.run_http("127.0.0.1", 8080);
```

A client connects to the server's URL:

```cpp
mcp::Client client(mcp::Endpoint::http("http://127.0.0.1:8080/mcp"));
client.sync().connect("my-client", "1.0.0");
```

### Elicitation

A handler can ask the user, through the client, to fill in a form. The client answers with the
handler it registers with `on_elicitation()` (see
[Elicitation](https://mcp-cpp-sdk.yurirocha.com/concepts/elicitation.html)):

```cpp
mcp::ElicitRequestFormParams form;
form.message = "How should they be addressed?";
form.requestedSchema = mcp::tool_params({{"title", "string", "Such as Dr. or Ms."}});
const mcp::ElicitRequestParams question = form;
const auto answer = co_await ctx.elicit(question);
if (answer.action == mcp::ElicitAction::eAccept && answer.content) {
    title = answer.content->value("title", std::string());
}
```

### Tasks

Under protocol revision 2026-07-28, a tool call can become a task: the server answers at once
with a handle, runs the tool apart from the request, and keeps its result for the client to
fetch (see [Tasks](https://mcp-cpp-sdk.yurirocha.com/concepts/tasks.html)):

```cpp
// Server
server.enable_tasks();
server.set_tool_task_mode("render", mcp::ToolTaskMode::eAuto);

// Client
auto started = client.sync().start_tool("render", {{"scene", "demo"}});
if (auto* task = std::get_if<mcp::TaskHandle>(&started)) {
    started = task->sync().result();  // waits for the task to finish
}
```

### Middleware

Middleware wraps each request that an application handler answers, such as `tools/call` and
`resources/read`, in the order it is registered (see
[Middleware](https://mcp-cpp-sdk.yurirocha.com/concepts/middleware.html)):

```cpp
server.use([](mcp::Context& ctx, const nlohmann::json& params,
              mcp::TypeErasedHandler next) -> mcp::Task<nlohmann::json> {
    std::cerr << "request\n";
    co_return co_await next(ctx, params);
});
```

## Documentation

Guides, API reference and integration details are on the **[documentation site](https://mcp-cpp-sdk.yurirocha.com/)**.

- **[Getting Started](https://mcp-cpp-sdk.yurirocha.com/getting-started.html)**: A first server and client.
- **[Installation](https://mcp-cpp-sdk.yurirocha.com/installation.html)**: Packages for each platform, and builds from source.
- **[Core Concepts](https://mcp-cpp-sdk.yurirocha.com/concepts/index.html)**: Deep dive into Tools, Resources, and Transports.
- **[Guides](https://mcp-cpp-sdk.yurirocha.com/guides/index.html)**: Async servers and clients, converting sync programs, testing, error handling and deployment.
- **[Client App Integrations](https://mcp-cpp-sdk.yurirocha.com/integrations/client-apps.html)**: How to connect your server to Claude, IDEs, and the MCP Inspector.
- **[Examples](https://mcp-cpp-sdk.yurirocha.com/examples.html)**: Walkthrough of included example applications.

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
[Known Issues](https://mcp-cpp-sdk.yurirocha.com/contributing.html#known-issues)
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

- **Formatting**: `make format` (requires `pre-commit`, which `python scripts/init.py --dev` installs)
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

## Acknowledgements

mcp-cpp-sdk is built on the work of these open-source projects. Thank you to their authors and
maintainers.

- [Boost](https://www.boost.org), in particular [Asio](https://github.com/boostorg/asio) and
  [Beast](https://github.com/boostorg/beast): executors, coroutines, networking, HTTP and WebSocket.
- [nlohmann/json](https://github.com/nlohmann/json): JSON and JSON-RPC serialization.
- [OpenSSL](https://github.com/openssl/openssl): cryptographic randomness and OAuth helper
  primitives.
- [jsoncons](https://github.com/danielaparker/jsoncons): validated tool schemas and the values
  checked against them up to version 0.3.0; its recorded verdicts are what the SDK's own JSON Schema
  evaluation is tested against.
- [Model Context Protocol](https://github.com/modelcontextprotocol/modelcontextprotocol) and its
  [conformance suite](https://github.com/modelcontextprotocol/conformance): the specification this
  SDK implements and the checks it is tested against.
- [JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite): the test
  cases behind the schema validation.
- [TM Dev Lab v2](https://github.com/thiagomendes/benchmark-mcp-servers-v2): the MCP server
  benchmark whose workload and infrastructure our benchmark is based on.
