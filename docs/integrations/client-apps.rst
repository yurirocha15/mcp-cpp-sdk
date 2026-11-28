Client App Integrations
=======================

The Model Context Protocol (MCP) is designed to be client-agnostic. Any MCP-compatible
application can host your C++ server as long as they share a transport layer.
This guide shows you how to connect your server to various clients and how
to validate that it's working correctly.

Validating Your Server
----------------------

Before connecting to a complex desktop application, you should validate your
server using simpler tools.

Using the MCP Inspector
^^^^^^^^^^^^^^^^^^^^^^^

The ``mcp-inspector`` is the official CLI tool for testing and debugging MCP
servers. It provides a web-based UI to interact with your tools, resources,
and prompts.

If you have Node.js installed, you can run it without installation:

.. code-block:: bash

   npx @modelcontextprotocol/inspector /path/to/your/repo/build/release/example-server-stdio

The inspector will launch a local web server (by default at ``http://localhost:6274``)
where you can see your server's capabilities and trigger its tools.

Using our Interactive Client
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK includes a built-in client template that you can use for quick validation
directly from your terminal:

.. code-block:: bash

   ./build/release/example-interactive-client /path/to/your/repo/build/release/example-server-stdio

This client launches your server as a child process, connects to it over its
standard input and output, lists its tools, and calls the first one with empty
arguments, providing an end-to-end "proof of life" for your transport and
schema logic. Arguments after the server path are passed to the server.

Connecting MCP Clients
----------------------

Coding agents, IDEs and desktop apps start a local server as a child process and
talk to it over the **stdio transport**. Each client keeps its servers in its own
configuration file, and the format differs from client to client.

`add-mcp <https://github.com/neon-solutions/add-mcp>`_, which runs on Node.js, adds
the server to clients such as Claude Code, Codex, Cursor, Gemini CLI, OpenCode,
VS Code and Zed. It finds the clients installed on the machine and asks which ones
to set up:

.. code-block:: bash

   npx add-mcp /path/to/your/repo/build/release/example-server-stdio --name my-cpp-server -g

Each argument for the server goes in its own ``--args``.

Clients that read an ``mcpServers`` file, such as Claude Desktop, Cursor and
Gemini CLI, take this entry:

.. code-block:: json

   {
     "mcpServers": {
       "my-cpp-server": {
         "command": "/path/to/your/repo/build/release/example-server-stdio",
         "args": []
       }
     }
   }

Troubleshooting Integrations
----------------------------

- **Absolute Paths**: Most host applications require **absolute paths** to
  your server executable.
- **Environment Variables**: If your server depends on specific environment
  variables (like `PATH` or API keys), ensure the host application's
  configuration includes them.
- **Logs**: If your server fails to start, check the host application's logs
  for `stderr` output from your subprocess.
