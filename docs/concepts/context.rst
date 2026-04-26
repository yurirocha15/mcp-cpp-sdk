Server Context
==============

The ``mcp::Context`` object is a fundamental construct in the MCP C++ SDK. It is passed as a reference to every asynchronous handler for tools, resources, and prompts. Think of the ``Context`` as the per-request interface between your server's logic and the connected client.

Core Responsibilities
---------------------

The ``Context`` object handles three main categories of interaction:

1.  **Downstream Communication**: Sending real-time updates to the client (logging and progress).
2.  **Request Lifecycle**: Managing state specific to the current request (cancellation).
3.  **Reverse RPC**: Initiating requests from the server back to the client (sampling, elicitation, and roots).

Lifecycle and Safety
--------------------

A ``Context`` is valid only for the duration of the request handler. It should never be stored beyond the ``co_return`` of the handler. The SDK ensures that the underlying transport and state required by the ``Context`` remain valid while the handler's coroutine is active.

.. note::
   The ``Context`` is designed to be coroutine-safe. You can safely pass it by reference into nested coroutines or helper functions, provided they are awaited within the main handler.

Logging
-------


You can send log messages to the client using various log levels. The SDK respects the server's current log level, automatically filtering messages that are less severe than the configured threshold.

Available log levels (from most to least severe):

*   **Emergency** (``LoggingLevel::eEmergency``): System is unusable.
*   **Alert** (``LoggingLevel::eAlert``): Action must be taken immediately.
*   **Critical** (``LoggingLevel::eCritical``): Critical conditions.
*   **Error** (``LoggingLevel::eError``): Error conditions.
*   **Warning** (``LoggingLevel::eWarning``): Warning conditions.
*   **Notice** (``LoggingLevel::eNotice``): Normal but significant conditions.
*   **Info** (``LoggingLevel::eInfo``): Informational messages.
*   **Debug** (``LoggingLevel::eDebug``): Debug-level messages.

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   server.add_tool("debug_tool", "Tool with logging", schema,
                   [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
                       // Direct level helpers
                       ctx.log_info("Executing debug tool...");

                       // Generic log method
                       ctx.log(mcp::LoggingLevel::eDebug, "Parameters: " + args.dump());

                       if (args.contains("error")) {
                           ctx.log(mcp::LoggingLevel::eError, "An intentional error occurred");
                       }

                       co_return nlohmann::json{{"status", "ok"}};
                   });

Logging is an asynchronous operation but generally returns quickly as it just pushes to the transport buffer. It's a non-blocking way to provide visibility into server-side operations without affecting the final tool output.

Progress Reporting
------------------

For long-running tasks, you can report progress to the client. This is essential for providing feedback during operations like heavy computation, large file transfers, or multi-step API interactions.

Prerequisites
~~~~~~~~~~~~~

Progress reporting requires:
1.  The client must support the ``notifications/progress`` capability.
2.  The client must provide a ``progressToken`` in the request metadata (``_meta``).

If no token is provided in the request, ``ctx.report_progress()`` calls are silently ignored.

Usage Details
~~~~~~~~~~~~~

The ``report_progress`` method accepts:
*   ``progress``: The current progress value (double).
*   ``total``: An optional total progress value (double).
*   ``message``: An optional human-readable progress message string.

.. literalinclude:: ../../examples/features/progress_cancellation.cpp
   :language: cpp
   :start-after: // ========== TOOL REGISTRATION ==========
   :end-before: // ========== TRANSPORT & RUN ==========
   :dedent: 8

The example above demonstrates a typical pattern where a tool loops through several steps, reporting the percentage completion and a descriptive message at each stage.

Cancellation
------------

MCP supports cooperative cancellation, allowing clients to stop execution of requests that are no longer needed. This is particularly important for expensive LLM operations or long-running background tasks.

How it works
~~~~~~~~~~~~

1.  A client sends a ``notifications/cancelled`` message for a specific request ID.
2.  The server marks the corresponding ``Context`` as cancelled.
3.  The tool handler checks ``ctx.is_cancelled()`` and exits gracefully.

Implementation Pattern
~~~~~~~~~~~~~~~~~~~~~~

Handlers should periodically check ``ctx.is_cancelled()`` during long-running operations. Since cancellation is cooperative, the server doesn't forcibly terminate the coroutine; it's the handler's responsibility to stop work.

.. code-block:: cpp

   for (int i = 0; i < steps; ++i) {
       // Check for cancellation at the start of each iteration
       if (ctx.is_cancelled()) {
           ctx.log_info("Task cancelled by client. Cleaning up...");
           // Perform any necessary cleanup (closing files, etc.)
           co_return nlohmann::json{
               {"content", {{{"type", "text"}, {"text", "Task cancelled"}}}},
               {"isError", true}
           };
       }

       // ... perform expensive work ...
       co_await ctx.report_progress(i + 1, steps, "Step " + std::to_string(i+1));
   }

Sampling (Reverse RPC)
----------------------

Sampling allows the server to request a completion from the LLM via the client. This "Reverse RPC" capability enables servers to build complex, multi-step workflows where the server uses the client's LLM to process data or make decisions.

Usage Example
~~~~~~~~~~~~~

The server sends a ``sampling/createMessage`` request back to the client. The client (e.g., Claude Desktop) then handles the request, often by prompting the user or using its own internal model.

.. code-block:: cpp

   server.add_tool("generate_summary", "Requests text from LLM", schema,
                   [](const nlohmann::json& args, mcp::Context& ctx) -> mcp::Task<nlohmann::json> {
                       mcp::CreateMessageRequestParams params;
                       params.messages.push_back({
                           .role = "user",
                           .content = mcp::TextContent{.text = "Summarize this: " + args["text"].get<std::string>()}
                       });
                       params.maxTokens = 100;

                       try {
                           auto result = co_await ctx.sample_llm(params);
                           co_return nlohmann::json{{"summary", result.content.text}};
                       } catch (const std::exception& e) {
                           ctx.log(mcp::LoggingLevel::eError, "Sampling failed: " + std::string(e.what()));
                           co_return nlohmann::json{{"error", "Failed to sample LLM"}};
                       }
                   });

Elicitation
-----------

Elicitation enables the server to request additional information or confirmation from the user. It's often used when a tool needs parameters that weren't initially provided or when a sensitive action requires explicit user approval.

Modes of Elicitation
~~~~~~~~~~~~~~~~~~~~

1.  **Form Mode**: Requests the user to fill out a structured form defined by a JSON schema.
2.  **URL Mode**: Directs the user to an external URL (e.g., for OAuth flows).

.. literalinclude:: ../../examples/features/elicitation.cpp
   :language: cpp
   :start-after: // ========== TOOL REGISTRATION ==========
   :end-before: // ========== TRANSPORT & RUN ==========
   :dedent: 8

The elicitation result contains an ``action`` (accept, decline, or cancel) and the elicited ``content`` (form data or response).

Roots
-----

Roots discovery allows a server to discover the files and directories the client has made available. This is crucial for servers that need to operate on local files while maintaining a security sandbox defined by the client.

The ``request_roots()`` method returns a list of roots that the client has explicitly shared. This might include:
*   Local project directories.
*   Specific documentation folders.
*   In-memory virtual file systems.

.. literalinclude:: ../../examples/features/roots.cpp
   :language: cpp
   :start-after: // ========== TOOL REGISTRATION ==========
   :end-before: // ========== TRANSPORT & RUN ==========
   :dedent: 8

The server calls ``ctx.request_roots()`` to get a list of ``Root`` objects, each containing a URI and an optional name. This is particularly useful for tools like "file_search" or "code_indexer" that need to know where they are allowed to look.

Advanced: Context Internal Architecture
---------------------------------------

While most developers will only use the public API of ``Context``, understanding its internal structure can help when debugging or extending the SDK.

The ``Context`` holds:
*   A reference to the ``ITransport`` for sending notifications.
*   A ``RequestSender`` (a functional wrapper) for performing reverse RPC.
*   A shared atomic flag for cancellation.
*   A ``ProgressToken`` extracted from the request's metadata.

This architecture ensures that ``Context`` is lightweight and efficient to pass around, while providing a rich set of capabilities.

Best Practices
--------------

*   **Check Cancellation Often**: For any operation taking more than a few hundred milliseconds, check ``is_cancelled()`` regularly.
*   **Meaningful Progress Messages**: Don't just send percentages; send messages like "Scanning files...", "Parsing AST...", or "Uploading results...".
*   **Log Level Discipline**: Keep ``Info`` and above for significant events. Use ``Debug`` for the "firehose" of internal state.
*   **Handle Reverse RPC Failures**: Sampling and Elicitation can fail due to network issues, client rejection, or user cancellation. Always use try-catch blocks or check result statuses.
*   **Feature Detection**: Remember that some clients might not support all features (like sampling or roots). The SDK handles the protocol level, but your tool logic should be resilient to missing capabilities.

Cross-References
----------------

To further explore the MCP C++ SDK and how it integrates with other features, consider these resources:

*   :doc:`tools`: Comprehensive guide to tool registration and schemas.
*   :doc:`resources`: Learn how to expose read-only data via MCP URIs.
*   :doc:`prompts`: Guide to creating dynamic prompt templates for LLMs.
*   :doc:`transports`: Understand the underlying communication layers (Stdio, WebSocket, HTTP).
*   :doc:`elicitation` (if available): Advanced patterns for user interaction.
*   :doc:`roots` (if available): Deep dive into the roots discovery protocol.
*   **Examples**: The ``examples/features/`` directory in the source tree contains self-contained code for all features discussed here.

Summary
-------

The ``mcp::Context`` is the bridge between your tool logic and the external world. By effectively using logging, progress reporting, and reverse RPC, you can create highly interactive and intelligent MCP servers that provide a superior user experience.
