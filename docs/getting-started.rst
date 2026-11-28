Getting Started
===============

This page builds a minimal server and client with the blocking API, in which
handlers are plain functions, client calls block until they are answered, and
the ``Server`` and ``Client`` run their own event loop.  The coroutine API, in
which the program runs the event loop and handlers and calls are C++20
coroutines, is described in :doc:`guides/async-mode`.

Install the SDK
---------------

Follow the page for your platform in :doc:`installation`.  It installs the SDK
for CMake's ``find_package``.

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

Build it with CMake:

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(greeter CXX)
   find_package(mcp-cpp-sdk CONFIG REQUIRED)
   add_executable(greeter minimal_server.cpp)
   target_compile_features(greeter PRIVATE cxx_std_20)
   target_link_libraries(greeter PRIVATE mcp::sdk)

A project that builds the SDK with ``FetchContent``, as
:doc:`installation/source` shows, leaves out ``find_package``.

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
