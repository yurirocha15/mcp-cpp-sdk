Tools
=====

Tools are one of the core primitives in MCP. They allow an LLM to take actions
on the user's behalf, such as performing calculations, interacting with
external APIs, or managing local files.

In `mcp-cpp-sdk`, tools are defined using the `add_tool` method on the `Server`
class. Each tool requires:

- **Name**: A unique identifier for the tool.
- **Description**: A human-readable explanation of what the tool does.
- **Input Schema**: A JSON Schema defining the input parameters.
- **Handler**: A C++ function or lambda that executes the tool logic.

Basic Tool Registration
-----------------------

The simplest way to register a tool is with a synchronous handler that takes
`nlohmann::json` and returns `nlohmann::json`.

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

Typed Handlers
--------------

For better type safety and cleaner code, you can use custom C++ structs for
tool input and output. Any type that implements `to_json` and `from_json`
(using `NLOHMANN_DEFINE_TYPE_INTRUSIVE` or manual implementation) can be used.

Using typed handlers provides several advantages:

1. **Automatic Validation**: The SDK automatically deserializes the incoming
   JSON into your struct, validating the presence and types of required fields.
2. **Type Safety**: Your handler logic works with native C++ types rather than
   dynamic JSON objects.
3. **Self-Documentation**: The input and output structures serve as clear
   documentation of the tool's interface.

The following example demonstrates defining input and output structures for a
calculator tool:

.. literalinclude:: ../../examples/servers/stdio/server_stdio.cpp
   :language: cpp
   :lines: 14-34

You then register the tool by providing these types as template arguments to
`add_tool`:

.. literalinclude:: ../../examples/servers/stdio/server_stdio.cpp
   :language: cpp
   :lines: 70-78

When using typed handlers, the SDK uses the template parameters to automatically
wrap your handler in a layer that performs the conversion between `nlohmann::json`
and your custom types.

Async Handlers with Context
---------------------------

Many tools need to perform asynchronous work, such as network requests or
long-running computations. `mcp-cpp-sdk` supports coroutine-based handlers
returning `mcp::Task<T>`.

Additionally, handlers can accept a `mcp::Context&` parameter to access
server-level features:

- **Logging**: Send real-time logs back to the client.
- **Progress Reporting**: Inform the client about long-running task status.
- **Cancellation**: Check if the client has requested to abort the task.

The following example shows a tool that logs information and performs an
asynchronous operation:

.. literalinclude:: ../../examples/servers/stdio/server_stdio.cpp
   :language: cpp
   :lines: 103-118

Progress and Cancellation
^^^^^^^^^^^^^^^^^^^^^^^^^

For tasks that take a significant amount of time, it is good practice to
report progress and periodically check for cancellation requests.

.. literalinclude:: ../../examples/features/progress_cancellation.cpp
   :language: cpp
   :lines: 50-92

For more details on using the context, see :doc:`context`.

Error Handling
--------------

There are two primary ways to handle errors in tool handlers:

1. **Throwing Exceptions**: If a handler throws a `std::exception`, the SDK
   automatically catches it and returns a tool error response (`isError: true`)
   to the client. The exception message is typically included in the error
   content.
2. **Explicit Error Returns**: You can return a `mcp::CallToolResult` object
   with the `isError` field set to `true`. This gives you full control over
   the error message and any additional metadata you want to return.

The following example demonstrates both patterns:

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :lines: 45-76

Note that errors in tool handlers are treated as application-level errors,
not JSON-RPC protocol errors. This means the connection remains stable even
if a tool fails. This is a critical distinction:

- **Protocol Errors**: Issues like malformed JSON, invalid method names, or
  disconnected transports cause JSON-RPC level errors.
- **Application Errors**: Issues within your tool's logic (e.g., "file not found",
  "invalid input value") should be handled by setting `isError: true` or
  throwing an exception.

Tool Output Schema
------------------

In addition to the input schema, you can provide an optional `outputSchema`.
This allows clients to understand the structure of the data returned by the
tool, enabling better integration with typed systems or UI rendering.

When an `outputSchema` is provided, the SDK can use it to validate that your
tool's output conforms to the expected format before sending it back to the
client.

.. code-block:: cpp

   nlohmann::json output_schema = {
       {"type", "object"},
       {"properties", {
           {"result", {{"type", "number"}}}
       }}
   };

   server.add_tool<AddInput, AddOutput>(
       "add_with_output_schema",
       "Add two numbers with defined output structure",
       input_schema,
       output_schema,
       [](AddInput in) -> AddOutput { return {in.a + in.b}; }
   );

Structured Content
------------------

While simple tools can return raw JSON, more complex tools might want to
return structured content blocks (text, images, or embedded resources).
The `mcp::CallToolResult` structure provides a standard way to return multiple
content blocks.

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :lines: 97-113

See Also
--------

- :doc:`context` for details on logging and progress.
- :doc:`index` for an overview of core concepts.
