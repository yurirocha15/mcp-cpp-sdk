mcp-cpp-sdk
===========

A modern C++20 Model Context Protocol SDK.

The mcp-cpp-sdk provides a compiled, coroutine-based implementation of the
Model Context Protocol (MCP), enabling seamless integration between AI models
and context providers. Built on Boost.Asio for async I/O and using modern C++20
features, this SDK offers a clean, type-safe API for building MCP servers and clients.

Features
--------

* **Modern C++20**: Leverages coroutines and concepts for readable asynchronous code and clear compile-time errors
* **Flexible transports**: stdio, WebSocket, Streamable HTTP, and custom transport support
* **Type-safe**: Strong typing with JSON serialization via nlohmann_json
* **Async-first**: Built on Boost.Asio for high-performance I/O. A sync mode, with plain-function handlers and blocking calls, is also available for simple programs and rapid prototyping
* **Protocol revision 2026-07-28**: Sessionless requests, ``server/discover``, cache hints, multi round-trip requests and ``subscriptions/listen``, with the older revisions served on the same transports
* **Broad MCP surface**: Tools, resources, prompts, sampling, roots, progress, and notifications
* **Conformance tested**: The official MCP suites run in CI, with known gaps listed
* **Authentication building blocks**: Experimental OAuth helpers and HTTP bearer validation
* **Optional extensions**: MCP Apps, Skills, OAuth client credentials and enterprise-managed authorization, each off until the application opts in (:doc:`concepts/extensions`)

Quick Links
-----------

* :doc:`getting-started` - Installation and quick start guide
* :doc:`guides/async-mode` - Servers and clients with coroutines on an event loop
* :doc:`api/index` - Complete API reference
* :doc:`examples` - Example programs and code snippets
* :doc:`concepts/protocol` - How protocol revision 2026-07-28 works in the SDK
* :doc:`architecture` - Design overview and architectural decisions

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   Overview <self>
   installation
   getting-started
   examples

.. toctree::
   :maxdepth: 2
   :caption: Core Documentation

   concepts/index
   guides/index
   integrations/client-apps
   api/index

.. toctree::
   :maxdepth: 2
   :caption: Project Info

   architecture
   contributing

Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
