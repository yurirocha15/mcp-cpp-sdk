Deprecations
============

Protocol revisions deprecate features before the specification removes them.
The SDK keeps a deprecated feature for peers that still use it, and tells the
application that uses it in two ways: a compiler warning on the declaration,
and a notice at run time. The features and their migration paths are listed in
:ref:`spec-deprecated`.

Compiler Warnings
-----------------

Each declaration of a deprecated feature carries ``[[deprecated]]`` through the
``MCP_DEPRECATED`` macro. Its message names the proposal that deprecates the
feature, the revision that deprecates it, its earliest removal and what to use
instead. GCC reports a call to ``Server::get_log_level()`` like this:

.. code-block:: text

   warning: 'mcp::LoggingLevel mcp::Server::get_log_level() const' is deprecated:
   SEP-2577: Logging is deprecated since protocol revision 2026-07-28; earliest
   removal: first revision released on or after 2027-07-28; log to stderr on
   stdio and use OpenTelemetry for observability [-Wdeprecated-declarations]

A program that must keep using such a feature, and builds with
``-Werror=deprecated-declarations`` or ``/we4996``, defines
``MCP_DISABLE_DEPRECATION_ATTRIBUTES`` before it includes any SDK header,
usually for the whole target:

.. code-block:: cmake

   target_compile_definitions(my_server PRIVATE MCP_DISABLE_DEPRECATION_ATTRIBUTES)

The macro turns off the attributes only. The run-time notices below stay.

A few parts of a deprecated feature carry no attribute, because the warning
would reach code that does not use the feature: the protocol types the SDK
still sends and reads, such as ``LoggingLevel`` and ``SamplingMessage``, the
data members of option and capability structures, such as
``RequestOptions::log_level``, and ``ClientIdentitySource::eDynamicRegistration``.
The run-time notice covers ``RequestOptions::log_level`` and dynamic
registration.

Run-Time Notices
----------------

The first time a ``Server``, a ``Client`` or an ``OAuthAuthorizationManager``
uses a deprecated feature, it reports a ``DeprecationNotice`` for it, once per
feature and object:

* A ``Server`` reports Logging, Sampling, Roots and the ``includeContext`` values
  ``thisServer`` and ``allServers`` when a handler calls ``Context::log()``,
  ``Context::log_info()``, ``Context::sample_llm()`` or
  ``Context::request_roots()``, and Logging when the application calls
  ``Server::get_log_level()``. The servers ``run_http()`` builds for its
  sessions share one report.
* A ``Client`` reports Sampling and Roots when the application calls
  ``on_sampling()``, ``set_roots()`` or ``on_roots_list()``, and Logging for the
  first 2026-07-28 request that sets ``RequestOptions::log_level``.
* An ``OAuthAuthorizationManager`` reports dynamic client registration when it
  registers with an authorization server.

Without a handler the notice is one line on standard error, never on standard
output, so a stdio server's protocol stream stays clean:

.. code-block:: text

   mcp-cpp-sdk: SEP-2577: Logging is deprecated since protocol revision 2026-07-28;
   earliest removal: first revision released on or after 2027-07-28. Log to
   standard error on stdio; use OpenTelemetry for observability.

Setting the environment variable ``MCP_SILENCE_DEPRECATION_NOTICES`` to a
non-empty value stops those lines. A handler receives the notices instead of
standard error, and the variable does not affect it:

.. code-block:: cpp

   server.set_deprecation_handler([&audit](const mcp::DeprecationNotice& notice) {
       audit.record(notice.name, notice.proposal, notice.earliest_removal);
   });

   mcp::ClientOptions options;
   options.deprecation_handler = [&audit](const mcp::DeprecationNotice& notice) {
       audit.record(notice.name, notice.proposal, notice.earliest_removal);
   };

``OAuthAuthorizationConfig::deprecation_handler`` does the same for an
``OAuthAuthorizationManager``. A handler runs on the thread that used the
feature, so it should return quickly. ``mcp::deprecation_notice()`` returns the
notice of any ``DeprecatedFeature`` without using it.

The SDK's versioning policy, including how long it keeps a deprecated
feature, is in ``VERSIONING.md`` at the root of the repository.
