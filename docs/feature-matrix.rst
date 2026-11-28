Feature Matrix
==============

The table lists the 48 non-experimental features of the MCP SDK feature list.
For each feature it names the API that implements it, the section of the
documentation that explains it, and a compiled example that uses it. The table is
generated from ``docs/feature_matrix.json`` by ``scripts/gen_feature_matrix.py``, and
``scripts/check_feature_matrix.py`` checks that every symbol, section and example it
names exists.

48 features are implemented.

.. list-table::
   :header-rows: 1
   :widths: 4 18 16 10 20 16 16

   * - #
     - Feature
     - Method
     - Status
     - API
     - Guide
     - Example
   * - 1
     - Tools - listing
     - ``tools/list``
     - Implemented
     - ``list_tools``
     - :doc:`concepts/tools`, Using Tools
     - ``examples/clients/stdio/client_stdio.cpp``
   * - 2
     - Tools - calling
     - ``tools/call``
     - Implemented
     - ``call_tool``
     - :doc:`concepts/tools`, Basic Tool Registration
     - ``examples/clients/stdio/client_stdio.cpp``
   * - 3
     - Tools - text results
     -
     - Implemented
     - ``TextContent``, ``make_tool_text_result``
     - :doc:`concepts/tools`, Result Content
     - ``examples/features/rich_content.cpp``
   * - 4
     - Tools - image results
     -
     - Implemented
     - ``ImageContent``
     - :doc:`concepts/tools`, Result Content
     - ``examples/features/rich_content.cpp``
   * - 5
     - Tools - audio results
     -
     - Implemented
     - ``AudioContent``
     - :doc:`concepts/tools`, Result Content
     - ``examples/features/rich_content.cpp``
   * - 6
     - Tools - embedded resources
     -
     - Implemented
     - ``EmbeddedResource``
     - :doc:`concepts/tools`, Result Content
     - ``examples/features/rich_content.cpp``
   * - 7
     - Tools - error handling
     -
     - Implemented
     - ``make_tool_error_result``
     - :doc:`concepts/tools`, Error Handling
     - ``examples/features/error_handling.cpp``
   * - 8
     - Tools - change notifications
     - ``notifications/tools/list_changed``
     - Implemented
     - ``notify_tools_list_changed``
     - :doc:`concepts/subscriptions`, List Change Notifications
     - ``examples/features/notifications_subscriptions.cpp``
   * - 9
     - Resources - listing
     - ``resources/list``
     - Implemented
     - ``list_resources``
     - :doc:`concepts/pagination`, Listing Resources and Prompts
     - ``examples/features/pagination.cpp``
   * - 10
     - Resources - reading text
     - ``resources/read``
     - Implemented
     - ``read_resource``, ``TextResourceContents``
     - :doc:`concepts/resources`, Static Resources
     - ``examples/clients/stdio/client_stdio.cpp``
   * - 11
     - Resources - reading binary
     - ``resources/read``
     - Implemented
     - ``BlobResourceContents``
     - :doc:`concepts/resources`, Binary Resources
     - ``examples/features/rich_content.cpp``
   * - 12
     - Resources - templates
     - ``resources/templates/list``
     - Implemented
     - ``list_resource_templates``
     - :doc:`concepts/resources`, Resource Templates
     - ``examples/clients/stdio/client_stdio.cpp``
   * - 13
     - Resources - template reading
     -
     - Implemented
     - ``add_resource_template``
     - :doc:`concepts/resources`, Resource Templates
     - ``examples/servers/stdio/server_stdio.cpp``
   * - 14
     - Resources - subscribing
     - ``resources/subscribe``
     - Implemented
     - ``subscribe_resource``
     - :doc:`guides/older-protocols`, Methods that exist only in legacy sessions
     - ``examples/features/legacy_protocols.cpp``
   * - 15
     - Resources - unsubscribing
     - ``resources/unsubscribe``
     - Implemented
     - ``unsubscribe_resource``
     - :doc:`guides/older-protocols`, Methods that exist only in legacy sessions
     - ``examples/features/legacy_protocols.cpp``
   * - 16
     - Resources - change notifications
     - ``notifications/resources/list_changed``
     - Implemented
     - ``notify_resources_list_changed``
     - :doc:`concepts/resources`, Resource Change Notifications
     - ``examples/features/notifications_subscriptions.cpp``
   * - 17
     - Prompts - listing
     - ``prompts/list``
     - Implemented
     - ``list_prompts``
     - :doc:`concepts/pagination`, Listing Resources and Prompts
     - ``examples/features/pagination.cpp``
   * - 18
     - Prompts - getting simple
     - ``prompts/get``
     - Implemented
     - ``get_prompt``, ``add_text_prompt``
     - :doc:`concepts/prompts`, Using Prompts
     - ``examples/sync/client.cpp``
   * - 19
     - Prompts - getting with arguments
     - ``prompts/get``
     - Implemented
     - ``get_prompt``, ``PromptArgument``
     - :doc:`concepts/prompts`, Parameterized Prompts and Arguments
     - ``examples/clients/stdio/client_stdio.cpp``
   * - 20
     - Prompts - embedded resources
     -
     - Implemented
     - ``EmbeddedResource``, ``PromptMessage``
     - :doc:`concepts/prompts`, Content Blocks
     - ``examples/features/rich_content.cpp``
   * - 21
     - Prompts - image content
     -
     - Implemented
     - ``ImageContent``, ``PromptMessage``
     - :doc:`concepts/prompts`, Content Blocks
     - ``examples/features/rich_content.cpp``
   * - 22
     - Prompts - change notifications
     - ``notifications/prompts/list_changed``
     - Implemented
     - ``notify_prompts_list_changed``
     - :doc:`concepts/prompts`, Prompt List Change Notifications
     - ``examples/features/notifications_subscriptions.cpp``
   * - 23
     - Sampling - creating messages
     - ``sampling/createMessage``
     - Implemented
     - ``sample_llm``, ``on_sampling``
     - :doc:`concepts/context`, Sampling (Reverse RPC)
     - ``examples/sync/server.cpp``
   * - 24
     - Elicitation - form mode
     - ``elicitation/create``
     - Implemented
     - ``ElicitRequestFormParams``
     - :doc:`concepts/elicitation`, Form-based Elicitation
     - ``examples/features/elicitation.cpp``
   * - 25
     - Elicitation - URL mode
     - ``elicitation/create (mode: "url")``
     - Implemented
     - ``ElicitRequestURLParams``, ``UrlElicitationRequired``
     - :doc:`concepts/elicitation`, URL-based Elicitation
     - ``examples/features/url_elicitation.cpp``
   * - 26
     - Elicitation - schema validation
     -
     - Implemented
     - ``ElicitationSchema``, ``ElicitationContentError``
     - :doc:`concepts/elicitation`, Validating the answer
     - ``examples/features/elicitation.cpp``
   * - 27
     - Elicitation - default values
     -
     - Implemented
     - ``BooleanSchema``, ``defaultValue``
     - :doc:`concepts/elicitation`, Typed Schemas
     - ``examples/features/elicitation.cpp``
   * - 28
     - Elicitation - enum values
     -
     - Implemented
     - ``TitledEnumSchema``, ``MultiSelectEnumSchema``
     - :doc:`concepts/elicitation`, Typed Schemas
     - ``examples/features/elicitation.cpp``
   * - 29
     - Elicitation - complete notification
     - ``notifications/elicitation/complete``
     - Implemented
     - ``notify_elicitation_complete``, ``on_elicitation_complete``
     - :doc:`concepts/elicitation`, Completing a URL elicitation
     - ``examples/features/url_elicitation.cpp``
   * - 30
     - Roots - listing
     - ``roots/list``
     - Implemented
     - ``request_roots``, ``on_roots_list``
     - :doc:`concepts/roots`, Using Roots
     - ``examples/features/roots.cpp``
   * - 31
     - Roots - change notifications
     - ``notifications/roots/list_changed``
     - Implemented
     - ``set_roots``, ``on_roots_list_changed``
     - :doc:`concepts/roots`, Roots Change Notifications
     - ``examples/features/roots.cpp``
   * - 32
     - Logging - sending log messages
     - ``notifications/message``
     - Implemented
     - ``log_info``
     - :doc:`concepts/context`, Logging
     - ``examples/sync/server.cpp``
   * - 33
     - Logging - setting level
     - ``logging/setLevel``
     - Implemented
     - ``set_log_level``
     - :doc:`guides/older-protocols`, Methods that exist only in legacy sessions
     - ``examples/features/legacy_protocols.cpp``
   * - 34
     - Completions - resource argument
     - ``completion/complete``
     - Implemented
     - ``ResourceTemplateReference``
     - :doc:`concepts/completions`, Resource Template Completions
     - ``examples/features/completions.cpp``
   * - 35
     - Completions - prompt argument
     - ``completion/complete``
     - Implemented
     - ``PromptReference``, ``set_completion_provider``
     - :doc:`concepts/completions`, Using Completions
     - ``examples/features/completions.cpp``
   * - 36
     - Ping
     - ``ping``
     - Implemented
     - ``ping``
     - :doc:`guides/older-protocols`, Methods that exist only in legacy sessions
     - ``examples/features/legacy_protocols.cpp``
   * - 37
     - Streamable HTTP transport (client)
     -
     - Implemented
     - ``HttpClientTransport``
     - :doc:`concepts/transports`, HTTP Transport (Network)
     - ``examples/features/http_server_convenience.cpp``
   * - 38
     - Streamable HTTP transport (server)
     -
     - Implemented
     - ``run_http``, ``StreamableHttpSessionManager``
     - :doc:`concepts/transports`, HTTP Transport (Network)
     - ``examples/features/http_server_convenience.cpp``
   * - 39
     - SSE transport - legacy (client)
     -
     - Implemented
     - ``LegacySseClientTransport``, ``legacy_sse``
     - :doc:`concepts/transports`, HTTP+SSE Transport (Deprecated)
     - ``examples/features/legacy_sse.cpp``
   * - 40
     - SSE transport - legacy (server)
     -
     - Implemented
     - ``set_legacy_sse``
     - :doc:`concepts/transports`, HTTP+SSE Transport (Deprecated)
     - ``examples/features/legacy_sse.cpp``
   * - 41
     - stdio transport (client)
     -
     - Implemented
     - ``StdioClientTransport``
     - :doc:`concepts/transports`, Stdio Transport (Local)
     - ``examples/clients/stdio/client_stdio.cpp``
   * - 42
     - stdio transport (server)
     -
     - Implemented
     - ``StdioTransport``
     - :doc:`concepts/transports`, Stdio Transport (Local)
     - ``examples/servers/stdio/server_stdio.cpp``
   * - 43
     - Progress notifications
     - ``notifications/progress``
     - Implemented
     - ``report_progress``
     - :doc:`concepts/context`, Progress Reporting
     - ``examples/features/progress_cancellation.cpp``
   * - 44
     - Cancellation
     - ``notifications/cancelled``
     - Implemented
     - ``is_cancelled``, ``CancellationToken``
     - :doc:`concepts/context`, Cancellation
     - ``examples/features/progress_cancellation.cpp``
   * - 45
     - Pagination
     -
     - Implemented
     - ``set_page_size``
     - :doc:`concepts/pagination`, Using Pagination
     - ``examples/features/pagination.cpp``
   * - 46
     - Capability negotiation
     -
     - Implemented
     - ``ServerCapabilities``, ``ClientCapabilities``
     - :doc:`concepts/protocol`, Discovery
     - ``examples/features/notifications_subscriptions.cpp``
   * - 47
     - Protocol version negotiation
     -
     - Implemented
     - ``ClientProtocol``, ``ServerProtocolPolicy``
     - :doc:`guides/older-protocols`, Choosing a Protocol on the Client
     - ``examples/features/legacy_protocols.cpp``
   * - 48
     - JSON Schema 2020-12 support
     -
     - Implemented
     - ``CompiledSchema``
     - :doc:`concepts/tools`, Schema Validation
     - ``examples/features/schema_validation.cpp``
