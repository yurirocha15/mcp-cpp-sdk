Elicitation
===========

Elicitation is a mechanism that allows a server to request additional information from the user during the execution of a tool, resource, or prompt. While traditional MCP interactions are client-initiated, elicitation enables a "reverse RPC" pattern where the server pauses execution to ask for missing data, confirmation, or credentials.

Key features include:

- **Server-Initiated**: The server triggers the request via the ``Context`` object.
- **Asynchronous**: Execution pauses until the user responds or the request times out.
- **Structured**: Supports both form-based input (with JSON Schema validation) and URL-based redirects.

Adding This to a Simple-Mode Program
------------------------------------

A tool of the server of :doc:`/getting-started` asks the user a question with
``ctx.elicit()``, and the client answers with the handler it registers with
``on_elicitation()``.

.. literalinclude:: /_generated/progression/server_add_elicitation.diff
   :language: diff

.. literalinclude:: /_generated/progression/client_add_elicitation.diff
   :language: diff

Server-side Elicitation
-----------------------

A server-side handler can initiate elicitation using the :doc:`context` object's ``elicit()`` method. This method is available in any async handler that accepts a ``Context&``.

.. literalinclude:: ../../examples/features/elicitation.cpp
   :language: cpp
   :start-after: server.add_tool<nlohmann::json, nlohmann::json>(
   :end-before: // ========== TRANSPORT & RUN ==========
   :dedent: 12

The ``elicit()`` method takes an ``ElicitRequestParams`` object and returns a ``Task<ElicitResult>``.

How the question reaches the client depends on the protocol revision of the
session. In a session of 2025-11-25 or earlier the server sends an
``elicitation/create`` request. In a 2026-07-28 session the server cannot send
requests, so for a ``tools/call``, ``prompts/get`` or ``resources/read`` it
answers with an ``input_required`` result that holds the question, and the
client retries the request with the answer. Your handler and your client code
are the same in both cases. See :doc:`protocol`. If the client did not declare
the capability the question needs in the request, the server answers with
``g_MISSING_REQUIRED_CLIENT_CAPABILITY``.

Form-based Elicitation
----------------------

Form-based elicitation (``ElicitRequestFormParams``) allows the server to request specific data fields defined by a JSON Schema. The client is responsible for rendering an appropriate UI (e.g., a dialog with input fields) and validating the user's input against the schema.

``requestedSchema`` is JSON, so a schema written by hand works:

.. code-block:: cpp

   ElicitRequestFormParams form_params;
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

Typed Schemas
~~~~~~~~~~~~~

``ElicitationSchema`` builds the same object from typed fields, one struct for
each primitive type the specification defines:

.. list-table::
   :header-rows: 1

   * - Type
     - Schema
   * - ``StringSchema``
     - ``type: "string"`` with ``minLength``, ``maxLength``, ``format`` and ``default``
   * - ``NumberSchema``
     - ``type: "number"``, or ``"integer"`` with ``integer = true``; ``minimum``, ``maximum``, ``default``
   * - ``BooleanSchema``
     - ``type: "boolean"`` with ``default``
   * - ``EnumSchema``
     - single select, ``enum`` (``enumNames`` is the legacy way to label the values)
   * - ``TitledEnumSchema``
     - single select, ``oneOf`` of ``const`` and ``title``
   * - ``MultiSelectEnumSchema``
     - ``type: "array"`` of ``enum`` values, with ``minItems`` and ``maxItems``
   * - ``TitledMultiSelectEnumSchema``
     - ``type: "array"`` of ``anyOf`` ``const`` and ``title``

``PrimitiveSchemaDefinition`` is the ``std::variant`` of these types. A field
is added with its name and, as the last argument, whether the user must fill it
in:

.. code-block:: cpp

   ElicitationSchema form;
   form.add("name", StringSchema{.title = "Full Name", .minLength = 1}, true)
       .add("color", TitledEnumSchema{.options = {{"#f00", "Red"}, {"#0f0", "Green"}}})
       .add("topics", MultiSelectEnumSchema{.enumValues = {"bugs", "docs"}});

   ElicitRequestFormParams form_params;
   form_params.message = "Tell us about you";
   form_params.requestedSchema = form;

``to_titled(EnumSchema)`` turns an ``EnumSchema`` that has ``enumNames`` into a
``TitledEnumSchema``. ``ElicitationSchema::from_json_schema()`` reads a
requested schema back into fields, and throws ``std::invalid_argument`` for
one that is not a flat object of the primitive types above, or whose
``minLength``, ``maxLength``, ``minItems`` or ``maxItems`` is not a
non-negative integer. The properties of a JSON object have no order on the
wire, so the order in which fields were added is not preserved in the JSON
form or when a schema is read back; ``fields()`` lists them in the order they
were added only for a schema built in code. An ``integer`` field's bounds are
written as integers: a ``minimum`` rounds up, a ``maximum`` rounds down, a
bound beyond the 64-bit range on its open side is clamped, and a bound or
``default`` that no 64-bit integer can satisfy makes the conversion to JSON
throw ``std::invalid_argument``.

``ElicitationSchema::decode()`` checks an accepted ``content`` against the
schema (every required field present, no undeclared field, each value within
its type, bounds and options) and returns an ``ElicitedContent`` that reads
each field in its type: ``get<std::string>("name")``,
``get<std::int64_t>("age")``, ``get<double>("score")``, ``get<bool>("news")``
and ``get<std::vector<std::string>>("topics")``. It throws
``std::invalid_argument`` listing the violations when the content does not
conform. ``format`` is an annotation and is not checked.

Validating the answer
~~~~~~~~~~~~~~~~~~~~~

The specification asks clients to validate the answer against the schema they
were given, and servers to validate the data they receive.

- **Client.** When an ``on_elicitation()`` handler returns ``accept`` for a
  form request, the client checks ``content`` against the request's
  ``requestedSchema`` before sending it, in both protocol eras. Content that
  does not conform is not sent: the call of the handler fails with
  ``ElicitationContentError`` (code ``g_INVALID_PARAMS``, the violations in
  ``data()``). A legacy ``elicitation/create`` request is then answered with a
  JSON-RPC error, so the server's ``ctx.elicit()`` call throws. In a
  2026-07-28 session the question came back in the result of the application's
  own call, which then throws ``ElicitationContentError``. Set
  ``ClientOptions::validate_elicitation_content`` to ``false`` for an
  application that validates the content itself. The check covers handlers
  registered with ``on_elicitation()``, not one registered for
  ``"elicitation/create"`` with ``on_request()``. A schema the SDK cannot
  compile (an unsupported dialect, say) leaves the content unchecked.
- **Server.** ``Context::elicit()`` checks the accepted form content against
  the request's ``requestedSchema`` before it returns, and throws
  ``std::runtime_error`` listing the violations when it does not conform. Set
  ``InputOptions::validate_content`` to ``false`` to receive the answer as sent. An answer to
  a URL request carries no content and is not checked.

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

``elicitationId`` is optional. For a session of an earlier revision,
``ctx.elicit()`` generates one when you leave it unset. A 2026-07-28 question
carries none: the server removes it from the question it sends.

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
2. **SDK** sends an ``elicitation/create`` request to the client (in a 2026-07-28 session, it answers the client's request with an ``input_required`` result instead).
3. **Client** receives the request via its ``on_elicitation()`` handler.
4. **Client** presents a UI to the user and awaits their response.
5. **Client** returns an ``ElicitResult`` to the server.
6. **Server** resumes execution with the provided data or handles the refusal.

For a full working example, see ``examples/features/elicitation.cpp``.

See Also
--------

- :doc:`context` for more on reverse RPC capabilities.
- :doc:`tools` for tool registration and handlers.
