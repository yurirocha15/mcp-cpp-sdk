Middleware
==========

Middleware provides a powerful way to intercept and augment the execution of tool handlers, resource requests, and prompt retrievals. By using middleware, you can implement cross-cutting concerns like logging, authentication, rate limiting, and request validation without cluttering your core business logic.

Overview
--------

Middleware functions sit between the server's request dispatcher and the final handler. They can:

- Inspect and modify incoming request parameters.
- Perform operations before and after the handler executes.
- Short-circuit the request by returning a result or error before the handler is even called.
- Catch and handle exceptions from inner layers.
- Inject additional context or metadata.

Registering Middleware
----------------------

Middleware is registered using the ``server.use()`` method. A middleware function must match the ``mcp::Middleware`` signature:

.. code-block:: cpp

   using Middleware = std::function<Task<nlohmann::json>(
       Context& ctx,
       const nlohmann::json& params,
       TypeErasedHandler next
   )>;

Where:

- ``ctx``: The request context (provides logging, progress, etc.).
- ``params``: The raw JSON parameters of the request.
- ``next``: A functional object representing the next step in the chain (either another middleware or the final handler).

The following example shows how to register multiple middleware layers:

.. literalinclude:: ../../examples/features/middleware.cpp
   :language: cpp
   :start-after: // ========== MIDDLEWARE REGISTRATION ==========
   :end-before: // ========== TOOL REGISTRATION ==========

Execution Order
---------------

Middleware follows an "onion" model. The first middleware registered with ``server.use()`` becomes the **outermost** layer. It is the first to execute on the way "in" and the last to execute on the way "out".

For example, if you register ``Middleware A`` then ``Middleware B``:

1. ``A`` starts.
2. ``A`` calls ``next()``, which triggers ``B``.
3. ``B`` starts.
4. ``B`` calls ``next()``, which triggers the actual **Handler**.
5. **Handler** executes and returns a result to ``B``.
6. ``B`` finishes and returns a result to ``A``.
7. ``A`` finishes and returns the final result to the client.

Short-circuiting
----------------

A middleware can choose not to call ``next()``. This is known as short-circuiting. It is useful for security checks, caching, or validation where you want to return an error or a cached response without involving the final handler.

When a middleware short-circuits, the execution chain is halted. Control immediately passes back to the previous middleware in the chain (if any) or back to the server's dispatcher.

In the example below, ``Middleware C`` checks for a "block" flag and returns a custom error if it's present:

.. literalinclude:: ../../examples/features/middleware.cpp
   :language: cpp
   :start-after: // Middleware C: Inner middleware (registered third)
   :end-before: // ========== TOOL REGISTRATION ==========

Composition Patterns
--------------------

Middleware can be composed to build complex request processing pipelines. By combining several small, single-purpose middleware functions, you can create highly specialized server behaviors while keeping the code maintainable.

Common patterns include:

- **Logging & Metrics**: An outer middleware that logs the start and end of every request, measuring execution time. This provides visibility into server performance and usage patterns.
- **Authentication**: A middleware that validates API keys, session tokens, or OAuth tokens before allowing access to sensitive tools. This ensures that only authorized clients can trigger certain actions.
- **Rate Limiting**: A middleware that tracks request frequency from clients and returns a "Too Many Requests" error if they exceed their quota.
- **Validation**: A middleware that ensures request parameters conform to additional business rules beyond what JSON Schema can express (e.g., checking if a file exists or if a value is within a dynamic range).
- **Error Mapping**: A middleware that catches low-level exceptions or library-specific errors and transforms them into user-friendly MCP errors.
- **Tracing**: A middleware that injects tracing headers or spans into the context to enable distributed tracing across multiple services.

Common Examples
---------------

Authentication
^^^^^^^^^^^^^^

You can create a middleware that checks for a specific header or parameter. For a full OAuth implementation, see the :doc:`oauth` concept page.

.. code-block:: cpp

   server.use([](Context& ctx, const nlohmann::json& params, TypeErasedHandler next) -> Task<nlohmann::json> {
       if (!is_authorized(params)) {
           mcp::CallToolResult err;
           err.isError = true;
           err.content.push_back(mcp::TextContent{"Unauthorized access"});
           co_return nlohmann::json(err);
       }
       co_return co_await next(ctx, params);
   });

Logging
^^^^^^^

Middleware can use the ``Context`` to send logs back to the client during request execution.

.. code-block:: cpp

   server.use([](Context& ctx, const nlohmann::json& params, TypeErasedHandler next) -> Task<nlohmann::json> {
       co_await ctx.log_info("Request started");
       auto result = co_await next(ctx, params);
       co_await ctx.log_info("Request finished");
       co_return result;
   });

Best Practices
--------------

- **Keep it focused**: Each middleware should handle exactly one concern. This makes your code easier to test, debug, and reuse across different servers.
- **Always co_await next**: Unless you are explicitly short-circuiting, ensure you ``co_await next(ctx, params)`` to maintain the execution chain. Forgetting to call or await ``next`` will cause the request to hang or terminate prematurely.
- **Handle exceptions**: If a middleware performs risky operations (like database queries or network calls), wrap ``next()`` in a try-catch block to ensure consistent error reporting and prevent server crashes.
- **Mind the order**: Register global concerns (like logging or tracing) first, and more specific concerns (like tool-specific validation or authentication) later. Remember that the first registered middleware is the outermost.
- **Use the Context**: Leverage the ``ctx`` object for logging information relevant to the middleware's task. This helps in auditing and troubleshooting request flows in production environments.
- **Avoid Heavy lifting**: Middleware should be relatively lightweight. If a middleware needs to perform a very expensive operation, consider if it should be part of the tool handler itself or if it can be optimized with caching.
- **Thread Safety**: Ensure your middleware logic is thread-safe, as it may be called from multiple concurrent request handlers within the Boost.Asio io_context. Avoid sharing non-thread-safe state unless protected by mutexes or strands.

See Also
--------

- :doc:`tools` for details on tool handlers.
- :doc:`context` for information on using the request context.
- :doc:`oauth` for an advanced use case involving authentication middleware.
