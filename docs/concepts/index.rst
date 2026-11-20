Core Concepts
=============

The Model Context Protocol (MCP) is built on a few core primitives that allow
servers to provide context and functionality to LLMs. This section covers
how these primitives are implemented in the `mcp-cpp-sdk`. Start with
:doc:`protocol` for the protocol revision 2026-07-28 model that the other pages
build on.

Most pages have a section "Adding This to a Sync-Mode Program" that adds the
feature to a program of :doc:`/getting-started`. The rest of each page uses async
mode and the whole API: servers constructed from an ``Implementation`` and
``ServerCapabilities``, typed handlers such as ``add_tool<In, Out>()``, coroutine
handlers and Boost.Asio executors. Both forms work on the same ``Server`` and
``Client``, so a sync-mode program takes what it needs from the rest of a page.

.. toctree::
   :maxdepth: 2

   protocol
   tools
   resources
   prompts
   transports
   middleware
   oauth
   extensions
   apps
   skills
   context
   completions
   subscriptions
   tasks
   elicitation
   pagination
   roots
