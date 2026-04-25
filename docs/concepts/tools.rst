Tools
=====

Tools are one of the core primitives in MCP. They allow an LLM to take actions
on the user's behalf, such as performing calculations, interacting with
external APIs, or managing local files.

In `mcp-cpp-sdk`, tools are defined using the `add_tool` method on the `Server`
class. Each tool requires:

- **Name**: A unique identifier for the tool.
- **Description**: A human-readable explanation of what the tool does.
- **Schema**: A JSON Schema defining the input parameters.
- **Handler**: A C++ function or lambda that executes the tool logic.

Basic Tool
----------

Here's how to register a simple tool that adds two numbers:

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   nlohmann::json schema = {
       {"type", "object"},
       {"properties", {
           {"a", {{"type", "number"}}},
           {"b", {{"type", "number"}}}
       }},
       {"required", nlohmann::json::array({"a", "b"})}
   };

   server.add_tool("add", "Add two numbers", std::move(schema),
                   [](const nlohmann::json& args) -> nlohmann::json {
                       double a = args.at("a").get<double>();
                       double b = args.at("b").get<double>();
                       return {{"result", a + b}};
                   });

Async Tool with Context
-----------------------

For tools that perform asynchronous work or need access to server features like
logging and progress reporting, use the `Context&` parameter:

.. code-block:: cpp

   server.add_tool("long_task", "A task that takes time", schema,
                   [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
                       co_await ctx.report_progress(10, 100);
                       // ... perform async work ...
                       co_await ctx.report_progress(100, 100);
                       co_return nlohmann::json{{"status", "complete"}};
                   });

Typed Tool Handlers
-------------------

You can also use custom structs for tool input/output, provided they have
`to_json` and `from_json` defined:

.. code-block:: cpp

   struct AddArgs {
       double a;
       double b;
       NLOHMANN_DEFINE_TYPE_INTRUSIVE(AddArgs, a, b)
   };

   struct AddResult {
       double sum;
       NLOHMANN_DEFINE_TYPE_INTRUSIVE(AddResult, sum)
   };

   server.add_tool<AddArgs, AddResult>(
       "add_typed", "Add two numbers (typed)", schema,
       [](AddArgs args) -> AddResult {
           return {args.a + args.b};
       });
