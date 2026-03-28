API Reference
=============

This section provides a complete API reference for the mcp-cpp-sdk, automatically
generated from the source code documentation using Doxygen and Breathe.

Overview
--------

The mcp-cpp-sdk is organized into several key namespaces and components:

* **mcp::Server** - MCP server implementation for hosting tools, resources, and prompts
* **mcp::Client** - MCP client implementation for connecting to servers
* **mcp::ITransport** - Transport abstraction for communication (stdio, WebSocket)
* **mcp::Context** - Execution context providing logging and reverse RPC
* **mcp::Task<T>** - Coroutine return type for async operations
* **Protocol Types** - Request/response types for JSON-RPC messages

Core Classes
------------

Server
^^^^^^

.. doxygenclass:: mcp::Server
   :members:
   :undoc-members:

Client
^^^^^^

.. doxygenclass:: mcp::Client
   :members:
   :undoc-members:

Context
^^^^^^^

.. doxygenclass:: mcp::Context
   :members:
   :undoc-members:

Transport Interface
^^^^^^^^^^^^^^^^^^^

.. doxygenclass:: mcp::ITransport
   :members:
   :undoc-members:

Core Types
----------

Task<T>
^^^^^^^

.. doxygentypedef:: mcp::Task

Protocol Types
--------------

The SDK includes comprehensive protocol types for all MCP messages:

.. doxygennamespace:: mcp
   :members:
   :undoc-members:

Complete Index
--------------

For a complete alphabetical index of all classes, functions, and types:

.. doxygenindex::
