API Reference
=============

This section provides a complete API reference for the mcp-cpp-sdk, automatically
generated from the source code documentation using Doxygen and Breathe.

Overview
--------

The mcp-cpp-sdk is organized into several key namespaces and components:

* **mcp::Server** - MCP server implementation for hosting tools, resources, and prompts
* **mcp::Client** - MCP client implementation for connecting to servers
* **mcp::ITransport** - Transport abstraction for communication (stdio, WebSocket, Streamable HTTP)
* **mcp::HttpClientTransport** / **mcp::HttpServerTransport** - Streamable HTTP transport implementations
* **mcp::StreamableHttpSessionManager** - Multi-session HTTP endpoint manager
* **mcp::Context** - Execution context providing logging and reverse RPC
* **mcp::Task<T>** - Coroutine return type for async operations
* **Protocol Types** - Request/response types for JSON-RPC messages
* **mcp::auth** - OAuth 2.1 helpers, discovery client, token store, and authenticated transport wrapper

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

HTTP Transports
^^^^^^^^^^^^^^^

.. doxygenclass:: mcp::HttpClientTransport
   :members:
   :undoc-members:

.. doxygenclass:: mcp::HttpServerTransport
   :members:
   :undoc-members:

.. doxygenclass:: mcp::StreamableHttpSessionManager
   :members:
   :undoc-members:

Authentication
^^^^^^^^^^^^^^

.. doxygenclass:: mcp::auth::OAuthHttpClient
   :members:
   :undoc-members:

.. doxygenclass:: mcp::auth::OAuthDiscoveryClient
   :members:
   :undoc-members:

.. doxygenclass:: mcp::auth::OAuthClientTransport
   :members:
   :undoc-members:

.. doxygenclass:: mcp::auth::InMemoryTokenStore
   :members:
   :undoc-members:

Core Types
----------

Task<T>
^^^^^^^

.. doxygentypedef:: mcp::Task

Protocol Types
--------------

The SDK includes comprehensive protocol and supporting types for all MCP
messages. They are available through the complete generated index below,
which avoids duplicating the class-level API sections above.

Complete Index
--------------

For a complete alphabetical index of all classes, functions, and types, use
the generated Sphinx index and search UI in the built documentation.
