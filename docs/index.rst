mcp-cpp-sdk
===========

A modern C++20 Model Context Protocol SDK.

The mcp-cpp-sdk provides a compiled, coroutine-based implementation of the
Model Context Protocol (MCP), enabling seamless integration between AI models
and context providers. Built on Boost.Asio for async I/O and using modern C++20
features, this SDK offers a clean, type-safe API for building MCP servers and clients.

Features
--------

* **Shared or static linkage**: Install and link the variant that fits your application
* **Modern C++20**: Leverages coroutines, concepts, and ranges
* **Flexible transports**: stdio, WebSocket, Streamable HTTP, and custom transport support
* **Type-safe**: Strong typing with JSON serialization via nlohmann_json
* **Async-first**: Built on Boost.Asio for high-performance I/O
* **Broad MCP surface**: Tools, resources, prompts, sampling, roots, progress, and notifications
* **Measured conformance**: Pinned official suites with explicit expected-failure evidence
* **Authentication building blocks**: Experimental OAuth helpers and HTTP bearer validation

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
