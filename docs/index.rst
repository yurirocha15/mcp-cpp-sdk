mcp-cpp-sdk
===========

A modern C++20 Model Context Protocol SDK.

The mcp-cpp-sdk provides a header-only, coroutine-based implementation of the
Model Context Protocol (MCP), enabling seamless integration between AI models
and context providers. Built on Boost.Asio for async I/O and using modern C++20
features, this SDK offers a clean, type-safe API for building MCP servers and clients.

Features
--------

* **Header-only**: Easy integration, no separate compilation required
* **Modern C++20**: Leverages coroutines, concepts, and ranges
* **Flexible transports**: stdio, WebSocket, Streamable HTTP, and custom transport support
* **Type-safe**: Strong typing with JSON serialization via nlohmann_json
* **Async-first**: Built on Boost.Asio for high-performance I/O
* **Full MCP support**: Tools, resources, prompts, sampling, roots, progress, and notifications
* **Authentication-ready**: OAuth 2.1 helpers and authenticated client transport support

Quick Links
-----------

* :doc:`getting-started` - Installation and quick start guide
* :doc:`api/index` - Complete API reference
* :doc:`examples` - Example programs and code snippets
* :doc:`architecture` - Design overview and architectural decisions

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   Overview <self>
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
