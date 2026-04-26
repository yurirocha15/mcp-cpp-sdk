Completions
===========

The Model Context Protocol (MCP) provides a mechanism for servers to offer completion suggestions for tool arguments, prompt arguments, and resource templates. This enables a richer user experience in client applications, such as IDEs or chat interfaces, by providing real-time suggestions as the user types.

Overview
--------

Completions are triggered by the client using the ``completion/complete`` request. The server responds with a list of possible values based on the current argument name and partial value provided by the client.

Key use cases include:
- Suggesting programming languages or frameworks for a code analysis tool.
- Auto-completing file paths or resource identifiers.
- Providing valid options for a configuration prompt.

Server-side Setup
-----------------

To support completions, a server must register a completion provider using ``set_completion_provider()``. This handler receives a ``CompleteParams`` object and returns a ``CompleteResult``.

.. literalinclude:: ../../examples/features/completions.cpp
   :language: cpp
   :start-after: // ========== COMPLETION PROVIDER ==========
   :end-before: // ========== TRANSPORT & RUN ==========
   :dedent: 8

The handler typically inspects ``params.argument.name`` to determine which argument is being completed and uses ``params.argument.value`` to filter suggestions based on what the user has already typed.

CompleteParams
--------------

The ``CompleteParams`` structure provides the context for the completion request:

- ``ref``: A reference to the prompt (``PromptReference``) or resource template (``ResourceTemplateReference``) being completed.
- ``argument``: The specific argument name and its current partial value.
- ``context``: Optional additional context, such as values of other arguments already filled by the user.

CompleteResult
--------------

The server returns a ``CompleteResult`` containing:

- ``values``: A list of string suggestions.
- ``total``: (Optional) The total number of completions available if the list is truncated.
- ``hasMore``: (Optional) A boolean indicating if more completions can be retrieved.

Client-side Usage
-----------------

Clients can request completions by calling ``client.complete()``. This is typically done as the user interacts with a tool or prompt interface.

.. literalinclude:: ../../examples/features/completions.cpp
   :language: cpp
   :start-after: // Test 1: Complete language argument with "C"
   :end-before: // Test 2: Complete framework argument with "R"
   :dedent: 20

Example Walkthrough
-------------------

The completions example (``examples/features/completions.cpp``) demonstrates a full round-trip:

1. **Server Setup**: The server defines a ``code_analyzer`` tool and sets a completion provider that knows how to suggest "language" and "framework" values.
2. **Client Request**: The client simulates a user typing "C" for the "language" argument.
3. **Server Response**: The server filters its internal list and returns "C++" and "C" as suggestions.
4. **Result Handling**: The client receives and prints the suggestions.

This flow allows clients to provide interactive, type-ahead style interfaces for complex MCP tools and prompts.
