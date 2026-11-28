Client App Integrations
=======================

Any MCP-compatible application can host a C++ server that shares its transport.

Validating Your Server
----------------------

Validate a server with a simple tool before connecting a desktop application.

Using the MCP Inspector
^^^^^^^^^^^^^^^^^^^^^^^

The MCP Inspector is a tool for testing and debugging MCP servers. Its web UI
calls a server's tools, resources and prompts.

With Node.js installed, run it without installing it:

.. code-block:: bash

   npx @modelcontextprotocol/inspector /path/to/your/repo/build/release/example-server-stdio

The inspector starts a local web server (by default at ``http://localhost:6274``)
that shows the server's capabilities and calls its tools.

Using our Interactive Client
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK's ``example-interactive-client`` validates a server from the terminal:

.. code-block:: bash

   ./build/release/example-interactive-client /path/to/your/repo/build/release/example-server-stdio

This client launches your server as a child process, connects to it over its
standard input and output, lists its tools, and calls the first one with empty
arguments. Arguments after the server path are passed to the server.

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
