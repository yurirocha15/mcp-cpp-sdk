Notifications and Subscriptions
===============================

Notifications and subscriptions enable real-time communication between MCP servers and clients. While the standard request-response cycle is suitable for most interactions, notifications allow either side to push updates asynchronously, and subscriptions specifically allow clients to stay informed about changes to resources.

Subscriptions Overview
----------------------

A client can ask a server to push two kinds of notification: that a list of
tools, prompts or resources changed, and that a resource it is interested in
changed.

Key benefits include:
- Reduced latency for data updates.
- Lower overhead compared to polling.
- Real-time synchronization for telemetry, logs, or shared state.

How a client asks depends on the protocol revision of the session:

- In a 2026-07-28 session the client opens a ``subscriptions/listen`` stream
  with ``Client::listen()``. See `Listening for Notifications`_.
- In a session of 2025-11-25 or earlier the client sends
  ``resources/subscribe`` for each resource URI. See `Resource Subscriptions`_.

The server side is the same in both: the server calls ``notify_resource_updated()``
and the ``notify_*_list_changed()`` methods, and the SDK sends each
notification to the clients that asked for it.

Listening for Notifications
---------------------------

Protocol revision 2026-07-28 has no ``resources/subscribe``. A client sends one
``subscriptions/listen`` request that names every notification it opts in to,
and the server keeps that request open and sends the notifications on it.

``Client::listen()`` takes a ``SubscriptionFilter`` and returns a
``Subscription`` once the server has acknowledged the request:

.. code-block:: cpp

   mcp::SubscriptionFilter filter;
   filter.toolsListChanged = true;
   filter.resourceSubscriptions.push_back("mcp://status/counter");

   mcp::Subscription subscription = co_await client.listen(filter);

   for (;;) {
       auto event = co_await subscription.next();
       if (!event) {
           break;  // the stream ended; see subscription.end_reason()
       }
       std::cout << event->method << "\n";
   }

The filter has ``toolsListChanged``, ``promptsListChanged`` and
``resourcesListChanged`` flags and a list of ``resourceSubscriptions`` URIs. The
server first answers with ``notifications/subscriptions/acknowledged``, which
lists the part of the filter it honors. It honors a list-changed type only if
the matching capability sets ``listChanged``, and resource URIs only if the
``resources`` capability sets ``subscribe``. ``Subscription::acknowledged()``
returns what the server honors, and ``Subscription::unhonored()`` what it left
out.

``Subscription::next()`` returns the notifications of this stream in the order
they arrived, as a ``SubscriptionEvent`` with the ``method`` and ``params``. The
``params._meta`` of each carries the id of the listen request under
``io.modelcontextprotocol/subscriptionId`` (``g_META_SUBSCRIPTION_ID``), which is
how streams on one connection are told apart. Any number of streams may be open
on one client. The client holds at most ``ClientOptions::subscription_queue_limit``
(1024 by default) notifications that ``next()`` has not taken yet, and drops the
oldest beyond that; ``Subscription::dropped_events()`` counts them.

``Subscription::close()`` ends the stream. A handle does not close its stream
when it is destroyed. ``Subscription::end_reason()`` says how a stream ended
(``SubscriptionEnd``): the server closed it, ``close()`` did, the connection
dropped, or the server answered with an error. After a dropped connection,
connect a new client and call ``listen()`` again; the server keeps no
subscription across connections.

``Client::listen()`` throws ``McpError`` with ``g_METHOD_NOT_FOUND`` in a
session of an older revision.

On the server, nothing changes in the handlers. ``notify_tools_list_changed()``,
``notify_prompts_list_changed()``, ``notify_resources_list_changed()`` and
``notify_resource_updated()`` also send to every open stream that asked for the
notification. A stream stays open until the client cancels it or closes the
connection. ``Server::close_subscriptions()`` ends every open stream, and
``Server::subscription_count()`` counts them. The ``on_subscribe()`` and
``on_unsubscribe()`` callbacks below belong to ``resources/subscribe`` and do
not run for ``subscriptions/listen``.

Which transports carry the stream:

- stdio, WebSocket and in-memory transports served by ``Server::run()``.
- ``StreamableHttpSessionManager``, as a server-sent event stream. The client
  must accept ``text/event-stream``. The stream runs on the manager's shared
  modern server, see :doc:`/guides/deployment`.

``HttpServerTransport`` answers ``subscriptions/listen`` with
``g_METHOD_NOT_FOUND``.

Resource Subscriptions
----------------------

In a session of protocol revision 2025-11-25 or earlier, a client subscribes to
a resource URI with the ``resources/subscribe`` request to indicate interest in
updates whenever that resource changes. A 2026-07-28 session has no such
request; it lists the URIs in a ``subscriptions/listen`` filter instead.

Server-side Subscription Callbacks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Servers can track when clients subscribe to or unsubscribe from resources. This is useful for managing underlying data sources, such as opening a database connection or starting a hardware sensor only when active subscribers exist.

You can register subscription handlers using ``on_subscribe()`` and ``on_unsubscribe()``:

.. literalinclude:: ../../examples/features/legacy_protocols.cpp
   :language: cpp
   :lines: 164-165
   :linenos:
   :caption: Registering subscription callbacks on the server.

Resource Change Notifications
-----------------------------

When a resource's content is updated, the server should notify all active subscribers. The ``notify_resource_updated()`` method sends a ``notifications/resources/updated`` message to all clients that have an active subscription for the given URI.

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :lines: 165-166
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
   :lines: 167-173
   :linenos:
   :caption: Triggering list change notifications for tools, resources, and prompts.

Client-side Notification Handling
---------------------------------

In a 2026-07-28 session the notifications of a ``subscriptions/listen`` stream
arrive through ``Subscription::next()``, as shown in `Listening for
Notifications`_. Other notifications, and every notification in a session of an
older revision, go to handlers registered with ``on_notification()``, which maps
a notification method name to a callback function.

.. literalinclude:: ../../examples/features/legacy_protocols.cpp
   :language: cpp
   :lines: 173-180
   :linenos:
   :caption: Registering client-side notification handlers in a 2025-11-25 session.

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

``examples/features/notifications_subscriptions.cpp`` demonstrates a complete
flow in a 2026-07-28 session:

1. **Server Setup**: The server declares the tools and resources capabilities
   with ``listChanged``, and ``subscribe`` for resources. It declares no prompts
   capability.
2. **Registration**: The server registers a counter resource.
3. **Listening**: The client opens a stream with ``listen()`` for the tools,
   prompts and resources list changes and the counter's URI. The
   acknowledgement leaves the prompt notifications out.
4. **Notification**: The server calls ``notify_resource_updated()``,
   ``notify_tools_list_changed()`` and ``notify_resources_list_changed()``.
5. **Handling**: The client reads each notification with ``next()``.
6. **Ending**: The client ends the stream with ``close()``, then opens a second
   one that the server ends with ``close_subscriptions()``.

The ``resources/subscribe`` flow of the older revisions is in
``examples/features/legacy_protocols.cpp``.

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
