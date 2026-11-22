Roots
=====

.. note::

   Protocol revision 2026-07-28 deprecates roots; the SDK keeps them for peers
   that use them. See :ref:`spec-deprecated`.

Roots allow a client to declare specific URI prefixes that it has access to. Servers can then discover these roots to understand the boundaries of the client environment, which is particularly useful for tools that need to perform file system operations or access specific resources.

Roots Overview
--------------

In the Model Context Protocol, roots represent the "home" or "workspace" directories of a client. By declaring roots, the client informs the server about:

- The absolute URIs (typically ``file:///``) that the server is allowed to interact with.
- The names of these workspaces to provide context to the LLM.

Using Roots
-----------

A tool asks for the client's roots with ``ctx.request_roots()`` and awaits
them, so its handler is a coroutine in both modes, and so is the client's
handler, which ``on_roots_list()`` registers:

.. literalinclude:: ../../examples/sync/server.cpp
   :language: cpp
   :start-after: // Roots:
   :end-before: // Middleware:
   :dedent: 4

.. literalinclude:: ../../examples/sync/client.cpp
   :language: cpp
   :start-after: // Roots:
   :end-before: // The session
   :dedent: 4

Client-side Setup
-----------------

A client provides its list of roots using the ``set_roots()`` method. This method stores the roots internally, registers a handler for ``roots/list``, and—when called before ``connect()``—automatically advertises the matching client capability.

.. literalinclude:: ../../examples/features/roots.cpp
   :language: cpp
   :start-after: // Declare roots that the client can access
   :end-before: Implementation client_info;

The ``set_roots`` method also takes an optional ``notify`` parameter. If set to
``true``, the client sends ``notifications/roots/list_changed`` only when its
initialization capabilities advertised ``roots.listChanged=true``. This is a
client capability; servers do not advertise roots support.

In a 2026-07-28 session there is no ``notifications/roots/list_changed``, and
``set_roots()`` sends nothing even with ``notify`` set. The client declares the
``roots`` capability in each request instead, when a ``roots/list`` handler
exists.

Server-side Discovery
---------------------

Servers can discover client roots at any time using the ``request_roots()`` method available in the :doc:`context`. This is a "reverse RPC" call where the server sends a request to the client. In a 2026-07-28 session the server asks through an ``input_required`` result and the client answers on a retry of its request (see :doc:`protocol`); this works in the handler of a ``tools/call``, ``prompts/get`` or ``resources/read``.

.. literalinclude:: ../../examples/features/roots.cpp
   :language: cpp
   :start-after: std::cout << "Tool: Requesting roots from client\n";
   :end-before: std::cout << "Tool: Received "
   :dedent: 16

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
