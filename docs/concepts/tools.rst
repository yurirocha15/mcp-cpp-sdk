Tools
=====

Tools are one of the core primitives in MCP. They allow an LLM to take actions
on the user's behalf, such as performing calculations, interacting with
external APIs, or managing local files.

In `mcp-cpp-sdk`, tools are defined using the `add_tool` method on the `Server`
class. Each tool requires:

- **Name**: A unique identifier for the tool.
- **Description**: A human-readable explanation of what the tool does.
- **Input Schema**: A JSON Schema object defining the input parameters. It must
  declare ``"type": "object"``, since tool arguments are always an object.
- **Handler**: A C++ function or lambda that executes the tool logic.

Basic Tool Registration
-----------------------

The simplest way to register a tool is with a synchronous handler that takes
`nlohmann::json` and returns `nlohmann::json`. A returned object made only of
``CallToolResult`` members (``content``, ``structuredContent``, ``isError``,
``_meta``) that decodes as one is sent as the result, as in the example below.
Any other value is turned into a ``CallToolResult``: a string becomes the text
block, an object becomes ``structuredContent`` with its serialization as the
text block, and anything else is serialized into the text block. An object
whose only member is an empty ``content`` array is treated as data and wrapped.

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
                       std::string text = "Result: " + std::to_string(a + b);
                       return {{"content", nlohmann::json::array(
                           {nlohmann::json{{"type", "text"}, {"text", std::move(text)}}})}};
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

1. **Throwing Exceptions**: A ``std::exception`` thrown by any tool handler
   (synchronous, typed or asynchronous) is converted into a tool error result
   (``isError: true``) whose text is the exception message. An exception from
   middleware is not: it is answered as a JSON-RPC ``-32603`` error (see
   :doc:`middleware`).
2. **Explicit Error Returns**: A handler declared with
   ``Out=mcp::CallToolResult`` can return a complete result whose ``isError``
   field is ``true``. ``add_raw_tool()`` provides the equivalent low-level JSON
   escape hatch with runtime validation.

The following example demonstrates both patterns:

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :lines: 45-73

Use ``isError: true`` when the tool completed normally but needs to report an
application-level failure back to the client. Either way the client receives a
normal ``tools/call`` result and the session stays open. This is a critical
distinction:

- **Protocol Errors**: Issues like malformed JSON, invalid method names, or
  disconnected transports cause JSON-RPC level errors.
- **Application Errors**: Issues within your tool's logic (e.g., "file not found",
  "invalid input value") should usually be returned as ``isError: true`` tool
  results so the client can inspect them as normal tool output.

Tool Output Schema
------------------

In addition to the input schema, you can provide an optional `outputSchema`.
This allows clients to understand the structure of the data returned by the
tool, enabling better integration with typed systems or UI rendering.

When an ``outputSchema`` is provided, the SDK includes it in the tool definition sent to clients.
The schema is advertised as ``outputSchema`` in the tool's metadata, enabling clients to understand
the structure of the tool's output. A tool with an ``outputSchema`` must return
``structuredContent`` (unless the result is an error), and that content must conform to the
schema; otherwise the call fails with a JSON-RPC error (see `Schema Validation`_).

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
   :lines: 94-109

Schema Validation
-----------------

The two most recent protocol revisions (2025-11-25 and 2026-07-28) require a
server to validate tool inputs and to return structured results that conform
to the output schema. The SDK does both by default.

**At registration.** ``add_tool()`` and ``add_raw_tool()`` compile the input and
output schemas and throw ``mcp::SchemaError`` (a ``std::invalid_argument``)
naming the tool and the field when a schema cannot be used.
``SchemaError::category()`` says why:

- ``eInvalidSchema``: the schema is not a JSON object, fails its dialect's
  metaschema, holds NaN or Infinity (which JSON cannot represent), or (for
  ``inputSchema``) does not declare ``"type": "object"``.
- ``eUnsupportedDialect``: ``$schema`` names a dialect the SDK does not
  implement.
- ``eUnresolvedRef``: a ``$ref`` points outside the document.

**Dialects.** A schema without ``$schema`` is JSON Schema 2020-12, the default
the specification sets. ``$schema`` may also name 2019-09, draft-07, draft-06 or
draft-04.

**References.** ``$ref`` resolves within the document (JSON Pointer fragments,
anchors, subschemas identified by ``$id``) and to the metaschemas of the
supported dialects. Anything else, such as an ``http:`` or ``file:`` URI or a
relative file name, is refused; the SDK never fetches a schema.

**Patterns.** ``pattern`` and ``patternProperties`` use a linear-time matcher
for the ECMA-262 subset JSON Schema recommends. Backreferences, lookaround and
Unicode property escapes (``\p{...}``) are not supported, and a schema using
them is refused as ``eInvalidSchema``. So is a pattern that nests groups more
than 64 levels deep, repeats more than 10,000 times in one ``{n,m}``, or compiles
to more than 20,000 matcher instructions; a counted repetition compiles to one
copy of its operand per repetition, so ``^.{1,2000}$`` is well within the limit.

**Arguments.** Each ``tools/call`` checks the arguments against the input schema
just before the handler would run, after any middleware. Arguments that do not
conform are answered with an ``isError: true`` result whose text lists the
violations (``Invalid arguments: /quantity: ...``), and the handler does not
run. ``Server::invoke_tool()`` applies the same check.

**Structured results.** A successful result's ``structuredContent`` is checked
against the ``outputSchema``. A result that does not conform is a fault in the
tool and is answered with ``Internal error (-32603)`` naming the violations.
A NaN or Infinity in the result is checked as the ``null`` it would be sent as.
Error results are not checked.

**Turning checks off.** An application that validates on its own can turn
either check off before ``run()``; ``tool_validation_stats()`` counts the calls
each check refused:

.. code-block:: cpp

   mcp::ToolValidationOptions validation;
   validation.validate_arguments = false;
   validation.validate_structured_content = true;
   server.set_tool_validation(validation);

   auto stats = server.tool_validation_stats();
   // stats.argument_rejections, stats.structured_content_rejections

**Limits.** Validation is bounded so that neither a schema nor a value can
exhaust the stack or take time exponential in its size. A value nested deeper
than 64 levels is invalid (``SchemaOptions::max_instance_depth``). The
evaluation path through the schema is limited to 192 keywords and array
indexes, which a schema recursing through ``anyOf`` and ``$ref`` at every level
reaches at about 48 levels of the value, and the evaluation work per value is
budgeted in proportion to its size and nesting. Pattern matching runs in time
linear in the text, but a pattern that keeps many alternatives open at once,
such as an unanchored ``.{1,1000}x``, costs thousands of steps per character, so
matching has its own budget: 16 million steps plus 64 for every byte of string
and property name in the value (``SchemaOptions`` sets both for a
``CompiledSchema``). Typical patterns take a handful of steps per
byte. A value that exceeds a limit is reported invalid with ``validation
stopped: ...``. A schema that exceeds a limit even for a trivial value, such as
``{"$ref": "#"}``, is refused at registration. Schema documents may nest up to
64 levels; checking a 2020-12 schema against its metaschema reaches the path
limit when ``properties`` nest 27 levels deep.

**Cost.** A 2020-12 schema that uses only ``type``, ``enum``, ``const``, the
numeric bounds, ``minLength``, ``maxLength``, ``pattern``, ``items``,
``minItems``, ``maxItems``, ``properties``, ``required``,
``additionalProperties``, ``minProperties``, ``maxProperties``, boolean
subschemas and annotations is evaluated directly on the arguments; other
schemas are evaluated by the bundled jsoncons library, which costs several
microseconds more per call for typical arguments. Violation messages are
written for people and their wording may change.

**On the client.** ``Client::list_tools()`` compiles the schemas of the listed
tools, leaves out a tool whose schema cannot be used and reports it through
``ClientOptions::on_tool_schema_error``. ``Client::call_tool()`` checks a
successful result's ``structuredContent`` against the tool's listed
``outputSchema`` and throws ``mcp::StructuredContentError`` when it does not
conform. ``ClientOptions::validate_tool_schemas = false`` turns both off.

``mcp::CompiledSchema`` (``mcp/protocol/schema.hpp``) is the validator behind
all of this and can be used directly:

.. code-block:: cpp

   auto schema = mcp::CompiledSchema::compile(
       {{"type", "object"}, {"required", {"id"}}});
   auto result = schema.validate({{"name", "x"}});
   // result.valid == false; result.violations[0].message names "id"

See Also
--------

- :doc:`context` for details on logging and progress.
- :doc:`index` for an overview of core concepts.
