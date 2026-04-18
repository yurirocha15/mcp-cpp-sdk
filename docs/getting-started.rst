Getting Started
===============

This guide will help you get started with the mcp-cpp-sdk, from installation
to building your first MCP server and client.

Prerequisites
-------------

Before installing mcp-cpp-sdk, ensure you have the following:

* **C++20 compiler**: GCC 10+, Clang 12+, or MSVC 2019+
* **CMake**: Version 3.20 or higher
* **Boost**: Recent version with Asio and Beast support (managed via Conan for source builds)
* **nlohmann_json**: Installed automatically for source builds via Conan
* **Conan**: Version 2.0+ (for dependency management)

Installation
------------

Using Conan (Recommended)
^^^^^^^^^^^^^^^^^^^^^^^^^^

The easiest way to get started is with Conan package manager:

.. code-block:: bash

   # Install dependencies
   conan install . --build=missing

   # Build the project
   cmake --preset conan-release
   cmake --build --preset conan-release

Using CMake FetchContent
^^^^^^^^^^^^^^^^^^^^^^^^^

Add mcp-cpp-sdk to your CMakeLists.txt:

.. code-block:: cmake

   include(FetchContent)
   FetchContent_Declare(
     mcp-cpp-sdk
     GIT_REPOSITORY https://github.com/yurirocha15/mcp-cpp-sdk.git
     GIT_TAG main
   )
   FetchContent_MakeAvailable(mcp-cpp-sdk)

   target_link_libraries(your_target PRIVATE mcp-cpp-sdk)

Using Git Submodule
^^^^^^^^^^^^^^^^^^^

Clone the repository as a submodule:

.. code-block:: bash

   git submodule add https://github.com/yurirocha15/mcp-cpp-sdk.git third_party/mcp-cpp-sdk
   git submodule update --init --recursive

Then in your CMakeLists.txt:

.. code-block:: cmake

   add_subdirectory(third_party/mcp-cpp-sdk)
   target_link_libraries(your_target PRIVATE mcp-cpp-sdk)

Building from Source
--------------------

To build the SDK and examples from source:

.. code-block:: bash

   # Clone the repository
   git clone https://github.com/yurirocha15/mcp-cpp-sdk.git
   cd mcp-cpp-sdk

   # Initialize development environment
   python scripts/init.py

   # Build the project
   python scripts/build.py

   # Run tests
   python scripts/build.py --test

Quick Start: Minimal Server
----------------------------

Here's a minimal MCP server that exposes a single tool over stdio.

.. code-block:: cpp

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

``server.run_stdio()`` blocks until the client closes the connection or the
process receives SIGINT/SIGTERM. The runtime, transport, and event loop are
all managed internally.

Advanced: Manual Transport Setup
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If you need direct control over the ``io_context`` — for example, to share it
with other async work — you can wire up the transport manually:

.. code-block:: cpp

   #include <mcp/server.hpp>
   #include <mcp/transport/stdio.hpp>
   #include <boost/asio/co_spawn.hpp>
   #include <boost/asio/detached.hpp>
   #include <boost/asio/io_context.hpp>

   int main() {
       mcp::ServerCapabilities caps;
       caps.tools = mcp::ServerCapabilities::ToolsCapability{};

       mcp::Implementation info{"math-server", "1.0.0"};
       mcp::Server server(std::move(info), std::move(caps));

       nlohmann::json schema = {
           {"type", "object"},
           {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}};

       server.add_tool<nlohmann::json, nlohmann::json>(
           "add", "Add two numbers", std::move(schema),
           [](const nlohmann::json& params) -> mcp::Task<nlohmann::json> {
               double result = params["a"].get<double>() + params["b"].get<double>();
               co_return nlohmann::json{{"result", result}};
           });

       boost::asio::io_context io;
       auto transport = std::make_unique<mcp::StdioTransport>(io.get_executor());
       boost::asio::co_spawn(
           io,
           [&]() -> mcp::Task<void> {
               co_await server.run(std::move(transport), io.get_executor());
           },
           boost::asio::detached);
       io.run();
   }

Quick Start: Minimal Client
----------------------------

.. note::

   A convenience ``client.run_stdio()`` API is planned. For now, clients
   require manual ``io_context`` setup as shown below.

Here's a minimal MCP client that connects to a server and calls a tool:

.. code-block:: cpp

   #include <mcp/client.hpp>
   #include <mcp/transport/stdio.hpp>
   #include <boost/asio/co_spawn.hpp>
   #include <boost/asio/detached.hpp>
   #include <boost/asio/io_context.hpp>
   #include <iostream>

   int main() {
       using namespace mcp;

       boost::asio::io_context io;

       auto transport = std::make_unique<StdioTransport>(io.get_executor());
       Client client(std::move(transport), io.get_executor());

       boost::asio::co_spawn(
           io,
           [&]() -> Task<void> {
               // Connect and initialize
               Implementation info;
               info.name = "math-client";
               info.version = "1.0.0";
               co_await client.connect(std::move(info), {});

               // Call the "add" tool
               nlohmann::json args = {{"a", 5}, {"b", 7}};
               auto result = co_await client.call_tool("add", args);
               std::cout << "Result: " << result.dump(2) << std::endl;

               co_await client.close();
           }(),
           boost::asio::detached
       );

       io.run();
   }

Next Steps
----------

* Explore the :doc:`examples` to see more advanced usage patterns
* Read the :doc:`api/index` for detailed API documentation
* Learn about the :doc:`architecture` and design decisions
* Check out :doc:`contributing` to contribute to the project
