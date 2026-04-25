Prompts
=======

Prompts are predefined interaction templates provided by the MCP server.
They allow servers to guide the LLM's behavior by suggesting a conversation
structure or specific context for the user.

In `mcp-cpp-sdk`, prompts are registered with a name, description, and an optional
set of arguments.

Registering a Prompt
--------------------

A prompt handler returns a list of messages that the LLM should process.

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   server.add_prompt("echo", "Echos a message",
                     {{"message", "The message to echo"}},
                     [](const nlohmann::json& args) -> nlohmann::json {
                         std::string msg = args.at("message").get<std::string>();
                         return {
                             {"messages", {
                                 {{"role", "user"}, {"content", {{"type", "text"}, {"text", msg}}}}
                             }}
                         };
                     });

Prompt Capabilities
-------------------

To enable prompts, include them in your server capabilities during initialization:

.. code-block:: cpp

   mcp::ServerCapabilities caps;
   caps.prompts = mcp::ServerCapabilities::PromptsCapability{};

   mcp::Server server(info, std::move(caps));
