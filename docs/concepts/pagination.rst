Pagination
==========

Pagination lets a server return a large list of items (tools, resources or prompts) in smaller chunks called pages.

The `mcp-cpp-sdk` implements cursor-based pagination as defined in the Model Context Protocol.

Using Pagination
----------------

The server sets a page size, the same way in both modes:

.. literalinclude:: ../../examples/sync/server.cpp
   :language: cpp
   :start-after: // Pagination:
   :end-before: // Tasks:
   :dedent: 4

A client passes each page's ``nextCursor`` back for the next page:

.. literalinclude:: ../../examples/sync/client.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // Pagination:
   :end-at: } while (cursor);
   :dedent: 4

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // Pagination:
   :end-at: } while (cursor);
   :dedent: 4

Server-side Configuration
-------------------------

Servers can control the maximum number of items returned in a single list response by calling ``set_page_size()``.

.. code-block:: cpp

   // Return at most 100 entries from each tools/list, resources/list,
   // resources/templates/list, or prompts/list request.
   server.set_page_size(100);

When ``set_page_size`` is set to a non-zero value, any list request (e.g., ``tools/list``, ``resources/list``) that exceeds this limit will automatically be paginated. The server will return a ``nextCursor`` in the result, which the client can use to fetch the next page.

.. literalinclude:: ../../examples/features/pagination.cpp
   :language: cpp
   :start-after: // ========== TOOL REGISTRATION ==========
   :end-before: // A tools/list result of a 2026-07-28 request
   :dedent: 8

Cursor-based Pagination
-----------------------

Cursor-based pagination uses an opaque string (the "cursor") to mark the position in the list.

1. **First Request**: The client sends a list request without a cursor.
2. **Paginated Response**: If the server has more items, it returns a page of results along with a ``nextCursor``.
3. **Subsequent Request**: The client sends the same list request, including the ``nextCursor`` received from the previous response.
4. **Termination**: The process continues until the server returns a response without a ``nextCursor``.

Client-side Handling
--------------------

Clients check for ``nextCursor`` in list results and loop until all items are retrieved:

.. literalinclude:: ../../examples/features/pagination.cpp
   :language: cpp
   :start-after: int total_tools = 0;
   :end-before: std::cout << "\n[Client] Pagination complete!
   :dedent: 20

Automatic Pagination
--------------------

The SDK handles the complexity of slicing the data and generating cursors automatically. When a list request arrives:

1. The SDK checks if a page size is set.
2. It identifies the subset of registered items corresponding to the provided cursor (or the first page if no cursor is provided).
3. It returns the items and, if more items remain, calculates the cursor for the next page.

Implementation Details
----------------------

The SDK validates the cursor, calculates the current slice, and returns an
opaque next cursor only when another page remains. Once ``set_page_size()`` is
called, this is transparent to the server developer.

Client-side Example
-------------------

This client-side loop retrieves all tools from a server, regardless of the page size:

.. literalinclude:: ../../examples/features/pagination.cpp
   :language: cpp
   :start-after: // ========== PAGINATION LOOP ==========
   :end-before: // ========== CLEANUP ==========

Pagination Best Practices
-------------------------

When implementing or consuming paginated APIs:

- **Consistent Page Size**: While the server determines the page size, clients should be prepared to handle any number of items up to the requested limit.
- **Opaque Cursors**: Clients must treat cursors as opaque strings. Never attempt to parse or construct cursors manually, as their format is internal to the server implementation.
- **Graceful Termination**: Always check for the absence of ``nextCursor`` as the definitive signal that the list is exhausted.

See Also
--------

- :doc:`tools` for details on tool registration.
- :doc:`resources` for listing resources.
- :doc:`prompts` for listing prompts.
