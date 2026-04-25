Server Context
==============

The `mcp::Context` object is a crucial parameter in async handlers for tools
and resources. It provides a way to interact with the server environment and
communicate with the client in real-time.

Logging
-------

You can send log messages to the client using various log levels:

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   server.add_tool("debug_tool", "Tool with logging", schema,
                   [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
                       ctx.log_info("Executing debug tool...");
                       ctx.log_debug("Parameters: " + args.dump());
                       co_return nlohmann::json{{"status", "ok"}};
                   });

Progress Reporting
------------------

For long-running tasks, you can report progress to the client:

.. code-block:: cpp

   server.add_tool("long_task", "Long task", schema,
                   [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
                       for (int i = 0; i <= 100; i += 10) {
                           co_await ctx.report_progress(i, 100);
                           // ... perform work ...
                       }
                       co_return nlohmann::json{{"status", "done"}};
                   });

Sampling (Reverse RPC)
----------------------

Sampling allows the server to request a completion from the LLM via the client:

.. code-block:: cpp

   server.add_tool("generate_text", "Requests text from LLM", schema,
                   [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
                       mcp::SamplingParams params;
                       params.prompt = "Generate a short summary of MCP.";
                       params.max_tokens = 100;

                       auto result = co_await ctx.sample_llm(std::move(params));
                       co_return result.to_json();
                   });
