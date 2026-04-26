Pagination
==========

Pagination allows servers to return large lists of items (tools, resources, or prompts) in smaller, manageable chunks called pages. This is critical for performance and stability when dealing with hundreds or thousands of items.

The `mcp-cpp-sdk` implements cursor-based pagination as defined in the Model Context Protocol.

Server-side Configuration
-------------------------

Servers can control the maximum number of items returned in a single list response by calling ``set_page_size()``.

.. literalinclude:: ../../include/mcp/server.hpp
   :language: cpp
   :lines: 263-268

When ``set_page_size`` is set to a non-zero value, any list request (e.g., ``tools/list``, ``resources/list``) that exceeds this limit will automatically be paginated. The server will return a ``nextCursor`` in the result, which the client can use to fetch the next page.

The following example demonstrates setting a page size of 3:

.. literalinclude:: ../../examples/features/pagination.cpp
   :language: cpp
   :lines: 45-47

Cursor-based Pagination
-----------------------

Unlike offset-based pagination, cursor-based pagination uses an opaque string (the "cursor") to mark the position in the list. This pattern is more robust against changes to the underlying data during traversal.

1. **First Request**: The client sends a list request without a cursor.
2. **Paginated Response**: If the server has more items, it returns a page of results along with a ``nextCursor``.
3. **Subsequent Request**: The client sends the same list request, including the ``nextCursor`` received from the previous response.
4. **Termination**: The process continues until the server returns a response without a ``nextCursor``.

Client-side Handling
--------------------

Clients should check for the presence of ``nextCursor`` in list results and loop until all items are retrieved.

The following example shows how a client can paginate through a list of tools:

.. literalinclude:: ../../examples/features/pagination.cpp
   :language: cpp
   :lines: 108-147

Key components of the client-side loop:

- **Cursor Storage**: Keep track of the current cursor (initially empty).
- **Request with Cursor**: Pass the ``cursor`` to the ``list_tools()``, ``list_resources()``, or ``list_prompts()`` method.
- **Result Processing**: Process the items returned in the current page.
- **Cursor Update**: If ``list_result.nextCursor`` is present, update the cursor and continue the loop. If not, the traversal is complete.

Automatic Pagination
--------------------

The SDK handles the complexity of slicing the data and generating cursors automatically. When a list request arrives:

1. The SDK checks if a page size is set.
2. It identifies the subset of registered items corresponding to the provided cursor (or the first page if no cursor is provided).
3. It returns the items and, if more items remain, calculates the cursor for the next page.

Implementation Details
----------------------

Internal pagination logic uses the ``paginate`` helper and ``PaginationSlice`` structure:

.. literalinclude:: ../../include/mcp/server.hpp
   :language: cpp
   :lines: 449-455

The process of pagination is fully transparent to the server developer once ``set_page_size()`` is called. The SDK internal dispatch logic detects when a list request arrives, calculates the appropriate slice of items based on the provided cursor (or lack thereof), and constructs the response with the ``nextCursor`` if more items remain. This ensures that even as the number of tools or resources grows, the memory footprint and network payload per request remain stable.

Client-side Example
-------------------

The following example demonstrates a complete client-side loop that retrieves all tools from a server, regardless of the page size:

.. literalinclude:: ../../examples/features/pagination.cpp
   :language: cpp
   :start-after: // ========== PAGINATION LOOP ==========
   :end-before: // ========== CLEANUP ==========

Pagination Best Practices
-------------------------

When implementing or consuming paginated APIs, consider the following best practices:

- **Consistent Page Size**: While the server determines the page size, clients should be prepared to handle any number of items up to the requested limit.
- **Opaque Cursors**: Clients must treat cursors as opaque strings. Never attempt to parse or construct cursors manually, as their format is internal to the server implementation.
- **Graceful Termination**: Always check for the absence of ``nextCursor`` as the definitive signal that the list is exhausted.
- **Resource Efficiency**: Use pagination to avoid blocking the server's event loop with massive JSON serialization tasks for very large lists.

See Also
--------

- :doc:`tools` for details on tool registration.
- :doc:`resources` for listing resources.
- :doc:`prompts` for listing prompts.
