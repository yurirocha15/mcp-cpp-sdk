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

   npx @modelcontextprotocol/inspector /path/to/your/build/examples/servers/stdio/example-server-stdio

The inspector will launch a local web server (usually at ``http://localhost:3000``)
where you can see your server's capabilities and trigger its tools.

Using our Interactive Client
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The SDK includes a built-in client template that you can use for quick validation
directly from your terminal:

.. code-block:: bash

   ./build/example-interactive-client /path/to/your/build/examples/servers/stdio/example-server-stdio

This client will connect to your server, list its tools, and attempt to call
the first available tool, providing an end-to-end "proof of life" for your
transport and schema logic.

Desktop Client Integrations
---------------------------

Most desktop applications (like IDEs and AI interfaces) use the **stdio transport**
to launch and communicate with MCP servers.

Claude Desktop
^^^^^^^^^^^^^^

To use your C++ MCP server with Claude Desktop, add it to your
``claude_desktop_config.json`` file.

* **macOS**: ``~/Library/Application Support/Claude/claude_desktop_config.json``
* **Windows**: ``%APPDATA%/Claude/claude_desktop_config.json``

.. code-block:: json

   {
     "mcpServers": {
       "my-cpp-server": {
         "command": "/path/to/your/repo/build/examples/servers/stdio/example-server-stdio",
         "args": []
       }
     }
   }

IDE Plugins (e.g., Cursor, VSCode)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For IDEs that support MCP (like Cursor or VSCode via extensions), you can
typically add a new "MCP Server" in the settings, specifying:

- **Type**: `command` or `stdio`
- **Command**: Absolute path to your compiled binary.
- **Args**: Any necessary CLI flags (e.g., `--llama-port=8080`).

Troubleshooting Integrations
----------------------------

- **Absolute Paths**: Most host applications require **absolute paths** to
  your server executable.
- **Environment Variables**: If your server depends on specific environment
  variables (like `PATH` or API keys), ensure the host application's
  configuration includes them.
- **Logs**: If your server fails to start, check the host application's logs
  for `stderr` output from your subprocess.
