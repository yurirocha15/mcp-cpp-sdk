Getting Started
===============

This guide will help you get started with the mcp-cpp-sdk, from installation
to building your first MCP server and client.

Prerequisites
-------------

You need:

* **C++20 compiler**: GCC 11+, Clang 14+, AppleClang 15+, or MSVC 2022
* **CMake**: 3.20 or newer
* **Boost** 1.74+, **nlohmann_json** 3.10.5+ and **OpenSSL** 3.0+, which the
  packages below install for you

Building the SDK itself with its scripts also needs **Conan** 2 and, on Windows,
`Chocolatey <https://chocolatey.org/install>`__.

Installation
------------

See :doc:`installation` for package-manager commands, supported platforms,
shared/static target selection, verification, and uninstall instructions.

From a Source Checkout with Conan
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

In a clone of this repository, Conan can provide the dependencies:

.. code-block:: bash

   # Install dependencies
   conan install . --build=missing -s compiler.cppstd=20

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
     GIT_TAG v0.2.0
   )
   # Skip the SDK's own tests, examples and docs.
   set(BUILD_TESTING OFF)
   set(BUILD_EXAMPLES OFF)
   set(BUILD_DOCS OFF)
   FetchContent_MakeAvailable(mcp-cpp-sdk)

   target_link_libraries(your_target PRIVATE mcp::sdk)

Using Git Submodule
^^^^^^^^^^^^^^^^^^^

Clone the repository as a submodule:

.. code-block:: bash

   git submodule add https://github.com/yurirocha15/mcp-cpp-sdk.git third_party/mcp-cpp-sdk
   git submodule update --init --recursive

Then in your CMakeLists.txt:

.. code-block:: cmake

   add_subdirectory(third_party/mcp-cpp-sdk)
   target_link_libraries(your_target PRIVATE mcp::sdk)

After 0.2.0, ``BUILD_TESTING``, ``BUILD_EXAMPLES`` and ``BUILD_DOCS`` default
to ``ON`` only when the SDK is the top-level project, so a project that adds it
with FetchContent or ``add_subdirectory`` builds only the libraries and does
not need GTest. With 0.2.0, set the three options to ``OFF`` before adding the
SDK, as in the FetchContent example.

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

Your First Server
-----------------

The SDK has a simple mode: a server or client in a few lines, with no event
loop, executor or Boost in the program. This server offers one tool, ``greet``,
over stdio:

.. literalinclude:: ../examples/simple/server_base.cpp
   :language: cpp
   :start-at: #include

* ``mcp::Server server("greeter", "1.0.0")`` names the server. It advertises the
  capabilities its registrations imply: tools once it has a tool, resources once
  it has a resource, and so on.
* ``mcp::tool_params()`` writes the tool's input schema: one ``ParamSpec`` per
  argument, with its name, JSON Schema type, description and whether it is
  required (it is unless the fourth member says otherwise). Arguments that do not
  match the schema are answered with an error result without calling the
  handler. Any JSON Schema can be passed instead.
* The handler takes the arguments as JSON and returns a JSON value: a string
  becomes the text of the result, and an object its structured content. An
  exception it throws becomes an error result (``isError: true``).
* ``server.run_stdio()`` serves until the client closes standard input or the
  process gets SIGINT or SIGTERM.

Build it with CMake against an installed SDK (see :doc:`installation`):

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(greeter CXX)
   find_package(mcp-cpp-sdk CONFIG REQUIRED)
   add_executable(greeter server_base.cpp)
   target_compile_features(greeter PRIVATE cxx_std_20)
   target_link_libraries(greeter PRIVATE mcp::sdk)

An MCP host such as an editor or a desktop assistant can now launch
``./greeter`` as a stdio server.

Your First Client
-----------------

This client launches the server, calls its tool and prints the answer:

.. literalinclude:: ../examples/simple/client_base.cpp
   :language: cpp
   :start-at: #include

* ``mcp::Endpoint::stdio({command})`` names the server to launch, by its program
  and arguments. ``Endpoint::http(url)`` names a Streamable HTTP server instead.
* A client constructed from an ``Endpoint`` runs its own event loop on a thread of
  its own. ``client.sync()`` returns a view whose members block the calling
  thread until the call is done and return its result or throw its exception.
* ``connect()`` detects the protocol revision the server speaks: it sends
  ``server/discover`` and falls back to ``initialize`` for a server of
  2025-11-25 or earlier. See :doc:`guides/older-protocols` to choose one
  instead.
* ``mcp::result_text()`` joins the text blocks of a tool result.
* Destroying the client closes the connection, which ends the server process.

.. important::

   **Client requests time out after 30 seconds by default.** A call that takes
   longer throws ``mcp::McpError`` with code ``-32001``, even though the server
   may still be working on it. Pass ``ClientOptions`` with a longer
   ``request_timeout`` when constructing the client if a tool runs longer:
   ``mcp::ClientOptions options; options.request_timeout = std::chrono::minutes(5);``
   and then ``mcp::Client client(mcp::Endpoint::stdio({command}), options);``.
   See :doc:`guides/error-handling` for per-request deadlines and what a timeout
   does and does not guarantee about the server.

Growing the Program
-------------------

Each feature is added to these programs without rewriting them: a resource or
a prompt is one more call on the same ``Server``, HTTP is a different run line,
and a handler that logs or reports progress becomes a coroutine while the other
handlers stay as they are. :doc:`guides/growing-a-server` and
:doc:`guides/growing-a-client` show each step as a diff against the programs
above, and every step is a compiled example in ``examples/simple``.

Next Steps
----------

* Grow the server and the client one feature at a time:
  :doc:`guides/growing-a-server` and :doc:`guides/growing-a-client`
* Explore the :doc:`examples` to see more advanced usage patterns
* Read the :doc:`api/index` for detailed API documentation
* Learn about the :doc:`architecture` and design decisions
* Check out :doc:`contributing` to contribute to the project
