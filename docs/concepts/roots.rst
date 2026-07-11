Roots
=====

Roots allow a client to declare specific URI prefixes that it has access to. Servers can then discover these roots to understand the boundaries of the client environment, which is particularly useful for tools that need to perform file system operations or access specific resources.

Roots Overview
--------------

In the Model Context Protocol, roots represent the "home" or "workspace" directories of a client. By declaring roots, the client informs the server about:

- The absolute URIs (typically ``file:///``) that the server is allowed to interact with.
- The names of these workspaces to provide context to the LLM.

Client-side Setup
-----------------

A client provides its list of roots using the ``set_roots()`` method. This method stores the roots internally and automatically registers a handler for the ``roots/list`` request.

.. literalinclude:: ../../examples/features/roots.cpp
   :language: cpp
   :start-after: // Declare roots that the client can access
   :end-before: Implementation client_info;

The ``set_roots`` method also takes an optional ``notify`` parameter. If set to ``true``, the client will send a ``notifications/roots/list_changed`` notification to the server whenever the roots are updated, provided the server supports this capability.

Server-side Discovery
---------------------

Servers can discover client roots at any time using the ``request_roots()`` method available in the :doc:`context`. This is a "reverse RPC" call where the server sends a request to the client.

.. literalinclude:: ../../examples/features/roots.cpp
   :language: cpp
   :start-after: try {
   :end-before: std::cout << "Tool: Received " << roots_result.roots.size()
   :lines: 1-2

Handling Roots Requests
-----------------------

While ``set_roots()`` provides a convenient way to manage a static or semi-static list of roots, you can also register a dynamic handler using ``on_roots_list()``. This is useful if the roots need to be calculated on-the-fly when requested.

.. code-block:: cpp

   client.on_roots_list([](const nlohmann::json& params) -> Task<ListRootsResult> {
       ListRootsResult result;
       result.roots = {
           Root{.uri = "file:///dynamic/path", .name = "Dynamic"}
       };
       co_return result;
   });

Roots Change Notifications
--------------------------

When the set of roots changes (e.g., the user opens a new folder in their IDE), the client should inform the server.

1. **Client**: Call ``set_roots(new_roots, true)`` to update and notify.
2. **Server**: If the server wants to react to these changes, it should monitor the ``roots/list`` state or re-request roots when needed.

Example Walkthrough
-------------------

The complete implementation of roots discovery can be found in the feature examples. It demonstrates a server tool that, when called, asks the client for its roots and returns them as a text summary.

.. literalinclude:: ../../examples/features/roots.cpp
   :language: cpp
   :start-after: // ========== TOOL REGISTRATION ==========
   :end-before: // ========== TRANSPORT & RUN ==========
