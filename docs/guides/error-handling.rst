Error Handling
==============

This guide covers error handling patterns in the MCP C++ SDK, focusing on both server-side and client-side strategies. Proper error handling ensures that your MCP applications are resilient, informative, and provide a smooth user experience even when things go wrong.

Overview
--------

The MCP C++ SDK leverages a combination of JSON-RPC error codes, C++ exceptions, and structured result flags to communicate errors between servers and clients. The primary goals of the error handling system are:

1. **Clarity**: Provide meaningful error messages and codes to identify the source of the problem.
2. **Isolation**: Prevent a single failure from crashing the entire server or client session.
3. **Graceful Degradation**: Allow clients to handle errors and continue functioning where possible.

JSON-RPC Error Codes
--------------------

The SDK follows the JSON-RPC 2.0 specification for error reporting. Standard error codes include:

- ``-32700``: Parse error (Invalid JSON received)
- ``-32600``: Invalid Request
- ``-32601``: Method not found
- ``-32602``: Invalid params
- ``-32603``: Internal error

Additionally, the SDK defines MCP-specific codes:

- ``-32000``: Request cancelled
- ``-32001``: Request timed out

When implementing custom tools, you should prefer using standard error codes where they apply, or use codes in the range ``-32000`` to ``-32099`` for application-specific protocol errors.

Server-side Error Handling
--------------------------

Server handlers (tools, resources, prompts) can report errors in two primary ways:

1. **Throwing Exceptions**: Any ``std::exception`` thrown from a handler is automatically caught by the SDK and converted into a JSON-RPC ``Internal error (-32603)`` response. This is the safest way to handle unexpected conditions, as the SDK ensures the error is correctly serialized and the connection remains stable.
2. **Returning Error Results**: For application-level errors (e.g., "File not found" or "Invalid input"), handlers can return a ``CallToolResult`` with the ``isError`` flag set to ``true``. This allows the client to distinguish between a technical failure (like a crash or timeout) and a logical error within the tool's execution.

Choosing Between Exceptions and Error Results
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Use **Exceptions** for:
  - Unexpected system failures (e.g., memory allocation failure, disk I/O errors).
  - Programming errors (e.g., out-of-bounds access, logic errors).
  - Conditions that prevent the handler from fulfilling its contract at all.
- Use **Error Results** for:
  - Expected failure modes (e.g., user not found, validation errors).
  - Situations where the tool successfully ran but couldn't produce the desired output.
  - Providing detailed, structured error information back to the LLM.

Example: Throwing an Exception
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :start-after: // Tool 1: Throws std::runtime_error
   :end-before: // Tool 2: Returns error JSON explicitly

Example: Returning an Error Result
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :start-after: // Tool 2: Returns error JSON explicitly
   :end-before: // Tool 3: Accesses invalid argument (nlohmann::json throws)

Implicit Exceptions
~~~~~~~~~~~~~~~~~~~

It's important to be aware that some library calls might throw exceptions that you don't explicitly catch. For instance, ``nlohmann::json`` throws an exception if you try to access a non-existent key using ``at()``. The SDK will catch these and report them as internal errors, ensuring the server doesn't crash.

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :start-after: // Tool 3: Accesses invalid argument (nlohmann::json throws)
   :end-before: // Tool 4: Successful tool (for comparison)

Client-side Error Handling
--------------------------

Clients should always wrap server calls in ``try-catch`` blocks to handle potential network issues, protocol violations, or server-side errors.

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :start-after: // ========== TEST 1: Catch exception from thrown error ==========
   :end-before: // Wait between tests

Catching Server Errors
~~~~~~~~~~~~~~~~~~~~~~

When a server returns a JSON-RPC error response, the ``Client`` methods (like ``call_tool`` or ``read_resource``) throw a ``std::runtime_error`` containing the error code and message. This simplifies the client logic, as you can use standard C++ exception handling for both local and remote failures.

Handling Application Errors
~~~~~~~~~~~~~~~~~~~~~~~~~~~

If the server returns a successful JSON-RPC response but the tool itself failed (indicated by ``isError: true``), the client receives the result normally and must check the flag. This is common when the tool executes correctly but encounters a business logic error that the LLM should be aware of.

.. literalinclude:: ../../examples/features/error_handling.cpp
   :language: cpp
   :start-after: // ========== TEST 2: Handle error JSON response ==========
   :end-before: // Wait between tests

Graceful Degradation Patterns
-----------------------------

To ensure your application remains usable during partial failures, consider the following patterns:

- **Feature Toggling**: If a specific tool call fails consistently, your client application can temporarily disable or hide the functionality associated with that tool.
- **Fallback Content**: When reading a resource fails, provide sensible default content or previously cached values. This is particularly useful for configuration or preference resources.
- **UI Feedback**: Instead of showing a generic "Server Error," use the detailed error message from the server to inform the user about what went wrong (e.g., "The search service is currently down, please try again later").
- **Partial Results**: In operations that return lists or multiple pieces of data, consider returning the successfully retrieved parts even if some encountered errors.

Retry Strategies
----------------

For transient errors, such as network timeouts or temporary server busy states (indicated by specific error codes), implementing a retry strategy is highly beneficial.

- **Exponential Backoff**: Increase the wait time between retries (e.g., 100ms, 200ms, 400ms) to avoid overwhelming a struggling server.
- **Max Retries**: Always define a maximum number of attempts to avoid infinite loops.
- **Idempotency**: Only retry operations that are idempotent (safe to repeat), such as reading resources or pings. Avoid retrying tools that perform side effects (like making a purchase or deleting a file) unless you are certain it's safe.

Logging and Monitoring
----------------------

The SDK provides a built-in logging system that is essential for debugging and monitoring production systems. Use the ``Context`` object in your handlers to log errors and warnings.

.. code-block:: cpp

   server.add_tool("example", "...", schema,
       [](const nlohmann::json& args, mcp::Context& ctx) -> Task<nlohmann::json> {
           try {
               // ... work ...
           } catch (const std::exception& e) {
               ctx.log_error("Tool failed: " + std::string(e.what()));
               throw;
           }
       });

By logging at the server level, you can aggregate errors across multiple clients and identify systemic issues. For more details on logging, see the :doc:`../concepts/context` guide.


Example Walkthrough
-------------------

The ``examples/features/error_handling.cpp`` file provides a complete, runnable example of these patterns. It demonstrates:

1. A server with multiple tools that fail in different ways.
2. A client that catches these failures and continues to function.
3. Successful recovery where a subsequent tool call succeeds after previous failures.

Run the example to see the output:

.. code-block:: bash

   ./build/release/example-feature-error-handling
