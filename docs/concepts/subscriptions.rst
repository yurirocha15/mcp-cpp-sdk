Notifications and Subscriptions
===============================

Notifications and subscriptions enable real-time communication between MCP servers and clients. While the standard request-response cycle is suitable for most interactions, notifications allow either side to push updates asynchronously, and subscriptions specifically allow clients to stay informed about changes to resources.

Subscriptions Overview
----------------------

In MCP, subscriptions are specifically tied to resources. A client can "subscribe" to a resource URI to indicate interest in receiving updates whenever that resource changes.

Key benefits include:
- Reduced latency for data updates.
- Lower overhead compared to polling.
- Real-time synchronization for telemetry, logs, or shared state.

Resource Subscriptions
----------------------

Server-side Subscription Callbacks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Servers can track when clients subscribe to or unsubscribe from resources. This is useful for managing underlying data sources, such as opening a database connection or starting a hardware sensor only when active subscribers exist.

You can register subscription handlers using ``on_subscribe()`` and ``on_unsubscribe()``:

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :lines: 64-73
   :linenos:
   :caption: Registering subscription callbacks on the server.

Resource Change Notifications
-----------------------------

When a resource's content is updated, the server should notify all active subscribers. The ``notify_resource_updated()`` method sends a ``notifications/resources/updated`` message to all clients that have an active subscription for the given URI.

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :lines: 148-150
   :linenos:
   :caption: Notifying subscribers of a resource update.

List Change Notifications
-------------------------

Beyond individual resource updates, servers can notify clients when their entire collection of tools, resources, or prompts has changed. This is common when new capabilities are dynamically added or removed at runtime.

The following methods are available for list change notifications:

- ``notify_tools_list_changed()``: Sends ``notifications/tools/list_changed``.
- ``notify_resources_list_changed()``: Sends ``notifications/resources/list_changed``.
- ``notify_prompts_list_changed()``: Sends ``notifications/prompts/list_changed``.

Example of triggering list notifications:

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :lines: 156-176
   :linenos:
   :caption: Triggering list change notifications for tools, resources, and prompts.

Client-side Notification Handling
---------------------------------

Clients must explicitly register handlers to process incoming notifications. This is done using the ``on_notification()`` method, which maps a notification method name to a callback function.

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :lines: 111-135
   :linenos:
   :caption: Registering client-side notification handlers.

Example Walkthrough
-------------------

The following example demonstrates a complete flow where a client subscribes to a counter resource, and the server periodically notifies the client of updates.

Architecture of Notifications
-----------------------------

Notifications in MCP are one-way messages that do not expect a response. This makes them ideal for high-frequency updates or background events where the overhead of a full request-response cycle would be prohibitive.

- **Methods**: Notification methods are prefixed with ``notifications/``.
- **Parameters**: Notifications can carry arbitrary JSON parameters.
- **Delivery**: Notifications are delivered asynchronously over the established transport.

Subscription State Management
-----------------------------

While the `mcp-cpp-sdk` handles the underlying message routing, the server application is responsible for managing the logic associated with subscriptions.

- **Persistence**: Subscriptions are typically session-based and do not persist across reconnections unless re-established by the client.
- **Scalability**: When notifying thousands of clients, consider the performance impact of JSON serialization. The SDK optimizes this where possible by reusing serialized buffers.

Example Walkthrough
-------------------

The following example demonstrates a complete flow where a client subscribes to a counter resource, and the server periodically notifies the client of updates.

1. **Server Setup**: The server enables the resource subscription capability.
2. **Registration**: The server registers a resource and subscription handlers.
3. **Subscription**: The client sends a ``resources/subscribe`` request.
4. **Notification**: The server calls ``notify_resource_updated()`` when the counter increments.
5. **Handling**: The client's registered callback processes the update.
6. **Unsubscription**: The client eventually unsubscribes to stop receiving updates.

Advanced Notification Patterns
------------------------------

Custom Notifications
~~~~~~~~~~~~~~~~~~~~

While MCP defines several standard notification methods, you can also send custom notifications using the underlying protocol layer if your client and server agree on the method names. However, for most resource-related needs, the standard ``updated`` and ``list_changed`` methods are preferred.

Throttling and Debouncing
~~~~~~~~~~~~~~~~~~~~~~~~~

For rapidly changing resources, it is recommended to throttle or debounce notifications on the server side to avoid overwhelming the client. For example, if a log file is being updated multiple times per millisecond, sending a single notification every 100ms is often more efficient.

For a complete working example, see ``examples/features/notifications_subscriptions.cpp`` in the source tree.

See Also
--------

- :doc:`resources`: Deep dive into how resources work.
- :doc:`context`: Using the request context for logging and progress.
- :doc:`transports`: How notifications are carried over different transport layers.
