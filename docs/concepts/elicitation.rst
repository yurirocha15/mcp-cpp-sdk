Elicitation
===========

Elicitation is a mechanism that allows a server to request additional information from the user during the execution of a tool, resource, or prompt. While traditional MCP interactions are client-initiated, elicitation enables a "reverse RPC" pattern where the server pauses execution to ask for missing data, confirmation, or credentials.

Key features include:

- **Server-Initiated**: The server triggers the request via the ``Context`` object.
- **Asynchronous**: Execution pauses until the user responds or the request times out.
- **Structured**: Supports both form-based input (with JSON Schema validation) and URL-based redirects.

Server-side Elicitation
-----------------------

A server-side handler can initiate elicitation using the :doc:`context` object's ``elicit()`` method. This method is available in any async handler that accepts a ``Context&``.

.. literalinclude:: ../../examples/features/elicitation.cpp
   :language: cpp
   :start-after: server.add_tool<nlohmann::json, nlohmann::json>(
   :end-before: // ========== TRANSPORT & RUN ==========
   :dedent: 12

The ``elicit()`` method takes an ``ElicitRequestParams`` object and returns a ``Task<ElicitResult>``.

Form-based Elicitation
----------------------

Form-based elicitation (``ElicitRequestFormParams``) allows the server to request specific data fields defined by a JSON Schema. The client is responsible for rendering an appropriate UI (e.g., a dialog with input fields) and validating the user's input against the schema.

.. code-block:: cpp

   ElicitRequestFormParams form_params;
   form_params.mode = "form";
   form_params.message = "Please provide your contact information";
   form_params.requestedSchema = nlohmann::json{
       {"type", "object"},
       {"properties", {
           {"name", {{"type", "string"}, {"title", "Full Name"}}},
           {"email", {{"type", "string"}, {"title", "Email Address"}}}
       }},
       {"required", nlohmann::json::array({"name", "email"})}
   };

   auto result = co_await ctx.elicit(form_params);

URL-based Elicitation
---------------------

URL-based elicitation (``ElicitRequestURLParams``) is used when the user needs to perform an action on an external website, such as completing an OAuth flow or verifying their identity.

.. code-block:: cpp

   ElicitRequestURLParams url_params;
   url_params.mode = "url";
   url_params.message = "Please sign in to continue";
   url_params.url = "https://auth.example.com/login?id=123";
   url_params.elicitationId = "login-flow-456";

   auto result = co_await ctx.elicit(url_params);

Client-side Handling
--------------------

Clients must register a handler to respond to elicitation requests. The ``mcp::Client`` class provides a convenience method ``on_elicitation()`` for this purpose.

.. literalinclude:: ../../examples/features/elicitation.cpp
   :language: cpp
   :start-after: mcp::Client client(client_transport, io_ctx.get_executor());
   :end-before: std::cout << "Client: Calling request_user_info tool\n";
   :dedent: 20

ElicitResult Actions
--------------------

The ``ElicitResult`` returned by the client contains an ``action`` field of type ``ElicitAction``:

- ``ElicitAction::eAccept``: The user provided the requested information or confirmed the action. For form-based elicitation, the ``content`` field will contain the submitted data.
- ``ElicitAction::eDecline``: The user explicitly refused to provide the information.
- ``ElicitAction::eCancel``: The user dismissed the request without making a choice (e.g., closed a dialog).

The server should handle each of these cases to ensure a graceful user experience.

.. code-block:: cpp

   if (result.action == ElicitAction::eAccept) {
       // Process result.content
   } else if (result.action == ElicitAction::eDecline) {
       // Handle refusal
   } else if (result.action == ElicitAction::eCancel) {
       // Handle cancellation
   }

Example Walkthrough
-------------------

The complete elicitation flow involves:

1. **Server** calls ``ctx.elicit()`` within a tool handler.
2. **SDK** sends an ``elicitation/create`` request to the client.
3. **Client** receives the request via its ``on_elicitation()`` handler.
4. **Client** presents a UI to the user and awaits their response.
5. **Client** returns an ``ElicitResult`` to the server.
6. **Server** resumes execution with the provided data or handles the refusal.

For a full working example, see ``examples/features/elicitation.cpp``.

See Also
--------

- :doc:`context` for more on reverse RPC capabilities.
- :doc:`tools` for tool registration and handlers.
