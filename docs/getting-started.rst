Getting Started
===============

This page installs the SDK and builds a minimal server and client with the
blocking API, in which handlers are plain functions, client calls block until
they are answered, and the ``Server`` and ``Client`` run their own event loop.
The coroutine API, in which the program runs the event loop and handlers and
calls are C++20 coroutines, is described in :doc:`guides/async-mode`.

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
   conan install . --output-folder=build --build=missing \
     -s compiler.cppstd=20 \
     -c tools.cmake.cmaketoolchain:generator=Ninja

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
     GIT_TAG v0.3.0
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

Since 0.3.0, ``BUILD_TESTING``, ``BUILD_EXAMPLES`` and ``BUILD_DOCS`` default
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

This server offers one tool, ``greet``, over stdio:

.. literalinclude:: ../examples/sync/minimal_server.cpp
   :language: cpp
   :start-at: #include

* The server advertises the capabilities its registrations imply, here tools.
* ``mcp::tool_params()`` makes the tool's input schema from the name, JSON Schema
  type and description of each argument. Arguments are required unless marked
  optional, and a call whose arguments do not match is answered with an error
  result. See :doc:`concepts/tools`.
* The handler's return value becomes the result: a string its text, an object
  its structured content. An exception becomes an error result.
* ``run_stdio()`` serves until standard input closes or the process receives
  SIGINT or SIGTERM.

Build it with CMake against an installed SDK (see :doc:`installation`):

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(greeter CXX)
   find_package(mcp-cpp-sdk CONFIG REQUIRED)
   add_executable(greeter minimal_server.cpp)
   target_compile_features(greeter PRIVATE cxx_std_20)
   target_link_libraries(greeter PRIVATE mcp::sdk)

Your First Client
-----------------

This client launches the server, calls its tool and prints the answer:

.. literalinclude:: ../examples/sync/minimal_client.cpp
   :language: cpp
   :start-at: #include

* ``mcp::Endpoint::stdio()`` launches the server and talks to it over its
  standard input and output. ``Endpoint::http(url)`` connects to a Streamable
  HTTP server instead.
* The calls of ``client.sync()`` block until they return the result or throw.
* ``connect()`` detects the protocol revision the server speaks; see
  :doc:`guides/older-protocols`.
* ``mcp::result_text()`` joins the text blocks of a tool result.

Build it as ``greeter-client`` the same way. ``./greeter-client ./greeter``
prints ``Hello, Ada!``.

.. important::

   **Client requests time out after 30 seconds by default.** A call that takes
   longer throws ``mcp::McpError`` with code ``-32001``, even though the server
   may still be working on it. Set ``ClientOptions::request_timeout`` when
   constructing the client, or give one call a deadline through
   ``RequestOptions``; see :doc:`guides/error-handling`.

Next Steps
----------

* :doc:`guides/async-mode`: servers and clients with coroutines on an event loop
* :doc:`guides/sync-to-async`: converting a server or client to the coroutine API
* :doc:`concepts/index`: each MCP feature, on the server and the client
* :doc:`examples`: the example programs and how to run them
* :doc:`api/index`: the API reference
* :doc:`architecture`: the design and its decisions
* :doc:`contributing`: building, testing and contributing to the SDK
