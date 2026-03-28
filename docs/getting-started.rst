Getting Started
===============

This guide will help you get started with the mcp-cpp-sdk, from installation
to building your first MCP server and client.

Prerequisites
-------------

Before installing mcp-cpp-sdk, ensure you have the following:

* **C++20 compiler**: GCC 10+, Clang 12+, or MSVC 2019+
* **CMake**: Version 3.15 or higher
* **Boost**: Version 1.75 or higher (Asio, Beast, JSON)
* **nlohmann_json**: Version 3.9.0 or higher
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

   target_link_libraries(your_target PRIVATE mcp::mcp)

Using Git Submodule
^^^^^^^^^^^^^^^^^^^

Clone the repository as a submodule:

.. code-block:: bash

   git submodule add https://github.com/yurirocha15/mcp-cpp-sdk.git third_party/mcp-cpp-sdk
   git submodule update --init --recursive

Then in your CMakeLists.txt:

.. code-block:: cmake

   add_subdirectory(third_party/mcp-cpp-sdk)
   target_link_libraries(your_target PRIVATE mcp::mcp)

Building from Source
--------------------

To build the SDK and examples from source:

.. code-block:: bash

   # Clone the repository
   git clone https://github.com/yurirocha15/mcp-cpp-sdk.git
   cd mcp-cpp-sdk

   # Initialize development environment
   make init

   # Build the project
   make build

   # Run tests
   make test

Quick Start: Minimal Server
----------------------------

Here's a minimal MCP server that exposes a single "add" tool over stdio:

.. code-block:: cpp

   #include <mcp/server.hpp>
   #include <mcp/transport/stdio.hpp>
   #include <boost/asio/io_context.hpp>

   int main() {
       using namespace mcp;

       // Create server with capabilities
       ServerCapabilities caps;
       caps.tools = ServerCapabilities::ToolsCapability{};

       Implementation info;
       info.name = "math-server";
       info.version = "1.0.0";

       Server server(std::move(info), std::move(caps));

       // Register an "add" tool
       server.add_tool(
           "add",
           "Add two numbers",
           nlohmann::json{{"type", "object"},
                         {"properties", {{"a", {{"type", "number"}}},
                                       {"b", {{"type", "number"}}}}}},
           [](const nlohmann::json& params) -> Task<nlohmann::json> {
               double result = params["a"].get<double>() + params["b"].get<double>();
               co_return nlohmann::json{{"result", result}};
           }
       );

       // Run server over stdio transport
       boost::asio::io_context io;
       auto transport = std::make_unique<StdioTransport>(io.get_executor());
       boost::asio::co_spawn(
           io,
           server.run(std::move(transport)),
           boost::asio::detached
       );
       io.run();
   }

Quick Start: Minimal Client
----------------------------

Here's a minimal MCP client that connects to a server and calls the "add" tool:

.. code-block:: cpp

   #include <mcp/client.hpp>
   #include <mcp/transport/stdio.hpp>
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
               ClientInfo info;
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
