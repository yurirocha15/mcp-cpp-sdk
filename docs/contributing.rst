Contributing
============

Thank you for your interest in contributing to mcp-cpp-sdk! This document provides
guidelines for contributing code, documentation, and bug reports.

Development Setup
-----------------

To set up your development environment:

.. code-block:: bash

   # Clone the repository
   git clone https://github.com/yurirocha15/mcp-cpp-sdk.git
   cd mcp-cpp-sdk

   # Initialize development environment
   python scripts/init.py --dev

   # Build with tests
   python scripts/build.py

   # Run tests
   python scripts/build.py --test

The ``python scripts/init.py --dev`` target installs all development dependencies including:

* Conan packages (Boost, nlohmann_json, GTest)
* Git hooks for pre-commit checks
* Development tools (clang-format, clang-tidy)

Code Style
----------

C++20 Standards
^^^^^^^^^^^^^^^

The SDK uses modern C++20 features:

* **Coroutines**: Use ``co_await`` and ``co_return`` for async operations
* **Concepts**: Define type requirements with concepts, not SFINAE
* **Ranges**: Use range-based algorithms where appropriate
* **Modules**: Not yet - header-only for now

Follow these guidelines:

* Prefer value semantics and move semantics over raw pointers
* Use RAII for all resource management
* Avoid manual memory management (``new``/``delete``)
* Use ``std::unique_ptr`` for ownership, ``std::shared_ptr`` only when necessary
* Prefer ``constexpr`` for compile-time constants

Naming Conventions
^^^^^^^^^^^^^^^^^^

Follow these naming conventions:

* **Classes/Structs**: ``PascalCase`` (e.g., ``Server``, ``ITransport``, ``CallToolRequest``)
* **Functions/Methods**: ``snake_case`` (e.g., ``add_tool()``, ``call_tool()``, ``log_info()``)
* **Variables**: ``snake_case`` (e.g., ``request_id``, ``tool_name``, ``json_result``)
* **Private members**: ``snake_case_`` with trailing underscore (e.g., ``transport_``, ``strand_``)
* **Constants**: ``kPascalCase`` (e.g., ``kDefaultTimeout``)
* **Template parameters**: ``PascalCase`` (e.g., ``template <typename T>``)
* **Namespaces**: ``snake_case`` (e.g., ``namespace mcp { namespace detail { ... } }``)

Code Formatting
^^^^^^^^^^^^^^^

Use ``make format`` to automatically format your code with clang-format.

The CI pipeline will reject PRs with formatting violations.

Doxygen Comments
^^^^^^^^^^^^^^^^

All public APIs must have Doxygen documentation:

.. code-block:: cpp

   /**
    * @brief Register a tool with the server.
    *
    * @details Tools are callable functions exposed to MCP clients. The handler
    * will be invoked when a client sends a CallToolRequest with matching name.
    *
    * @param name Tool name (must be unique)
    * @param description Human-readable description
    * @param input_schema JSON schema for tool parameters
    * @param handler Callable that processes the tool request
    *
    * @throws std::invalid_argument if tool name is empty or already registered
    */
   void add_tool(std::string name, std::string description,
                 nlohmann::json input_schema, TypeErasedHandler handler);

Required Doxygen tags:

* ``@brief`` - One-line summary
* ``@param`` - For each parameter
* ``@return`` - For non-void returns (or ``@returns``)
* ``@throws`` - For exceptions that may be thrown
* ``@details`` - Extended description (optional but recommended)

Testing
-------

All contributions must include tests. The SDK uses Google Test (GTest).

Writing Tests
^^^^^^^^^^^^^

Place tests in ``test/`` directory, mirroring the library surface area:

.. code-block:: bash

   include/mcp/server.hpp  →  test/server_core_test.cpp
   include/mcp/client.hpp  →  test/client_core_test.cpp

Example test structure:

.. code-block:: cpp

   #include <mcp/server.hpp>
   #include <gtest/gtest.h>

   TEST(ServerTest, AddToolRegistersCorrectly) {
       mcp::Server server(/* ... */);

       server.add_tool("test", "A test tool", {},
           [](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
               co_return nlohmann::json{{"result", "ok"}};
           });

       // Assertions...
   }

Running Tests
^^^^^^^^^^^^^

.. code-block:: bash

   # Run all tests
   python scripts/build.py --test

   # Run the full suite with ctest output on failures
   ctest --preset conan-release --output-on-failure

   # Run the GoogleTest binary directly with a filter
   ./build/mcp-sdk-tests --gtest_filter=ServerCoreTest.*

Code Coverage
^^^^^^^^^^^^^

Aim for >80% code coverage for new features:

.. code-block:: bash

   # Generate coverage report
   python scripts/build.py --coverage --test

   # View report
   open build/coverage/index.html

Pull Request Process
--------------------

1. Fork and Branch
^^^^^^^^^^^^^^^^^^

Fork the repository and create a feature branch:

.. code-block:: bash

   git checkout -b feature/my-awesome-feature

Use descriptive branch names:

* ``feature/websocket-compression`` - New features
* ``fix/client-timeout-bug`` - Bug fixes
* ``docs/api-reference`` - Documentation
* ``refactor/handler-dispatch`` - Refactoring

2. Make Changes
^^^^^^^^^^^^^^^

* Write code following the style guide
* Add tests for new functionality
* Update documentation (Doxygen comments, .rst files)
* Run tests locally: ``python scripts/build.py --test``
* Format code: ``make format``

3. Commit
^^^^^^^^^

Write clear, atomic commit messages:

.. code-block:: bash

   # Good commit messages
   git commit -m "feat: Add WebSocket compression support"
   git commit -m "fix: Fix client timeout handling for large responses"
   git commit -m "docs: Update architecture.rst with transport details"

   # Bad commit messages
   git commit -m "fix bug"
   git commit -m "WIP"
   git commit -m "asdf"

Follow these guidelines:

* First line: Use conventional commit format, imperative mood, <80 chars (e.g., "feat: Add X", "fix: Fix Y")
* Body: Explain **why**, not **what** (code shows what)
* Reference issues: "Fixes #123" or "Relates to #456"

4. Push and Create PR
^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   git push origin feature/my-awesome-feature

Then create a pull request on GitHub with:

* **Title**: Clear, descriptive summary
* **Description**: What, why, and how
* **Tests**: Describe test coverage
* **Breaking changes**: Call out any API changes

PR Template
^^^^^^^^^^^

.. code-block:: markdown

   ## Summary
   Brief description of the change.

   ## Motivation
   Why is this change needed? What problem does it solve?

   ## Changes
   - Bullet list of changes
   - Include any API modifications
   - Note breaking changes

   ## Testing
   - Unit tests added: [Yes/No]
   - Integration tests added: [Yes/No]
   - Manual testing performed: [Yes/No]

   ## Documentation
   - Doxygen comments updated: [Yes/No]
   - User documentation (RST) updated: [Yes/No]

   Fixes #123

5. Code Review
^^^^^^^^^^^^^^

Maintainers will review your PR. Be prepared to:

* Answer questions about design decisions
* Make requested changes
* Add additional tests
* Update documentation

All PRs require:

* Passing CI checks (build, tests, format)
* At least one approving review
* No unresolved comments

6. Merge
^^^^^^^^

Once approved, a maintainer will merge your PR. The project uses squash merging
to keep the main branch history clean.

Reporting Bugs
--------------

Use GitHub Issues to report bugs. Include:

* **Environment**: OS, compiler version, Boost version
* **Steps to reproduce**: Minimal example code
* **Expected behavior**: What should happen
* **Actual behavior**: What actually happens
* **Stack trace**: If applicable (crashes, exceptions)

Example bug report:

.. code-block:: markdown

   **Environment**
   - OS: Ubuntu 22.04
   - Compiler: GCC 11.3
   - Boost: 1.80
   - SDK Version: 0.1.0

   **Steps to Reproduce**
   ```cpp
   mcp::Client client(transport, executor);
   co_await client.call_tool("nonexistent", {});  // Hangs forever
   ```

   **Expected**: Exception thrown or timeout
   **Actual**: Hangs indefinitely

   **Stack Trace**
   (gdb output if available)

Known Issues
------------

GCC 11 SSO Coroutine Bug
------------------------

**When writing coroutines (``Task<T>`` functions), never store ``std::string`` locals
in the coroutine frame across a ``co_await`` suspension on GCC 11.**

There is a confirmed GCC 11 compiler bug:

* `GCC Bug #107288 <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=107288>`_
* `GCC Bug #100611 <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100611>`_
* `Boost.Asio Issue #1027 <https://github.com/chriskohlhoff/asio/issues/1027>`_

GCC 11 corrupts the internal self-pointer of ``std::string`` (SSO — Small String
Optimization) when the string lives in a heap-allocated coroutine frame and survives
across a suspension point. The corruption happens at destructor time, causing
``munmap_chunk(): invalid pointer`` crashes.

**Affected types** (unsafe across ``co_await`` in GCC 11):

* ``std::string`` (including moved-from empty strings)
* ``std::optional<std::string>`` with short values
* Any struct containing ``std::string`` (e.g. ``JSONRPCRequest``, ``RequestId`` with string variant)

**Safe types** across ``co_await``:

* ``nlohmann::json`` (heap-allocated internally)
* ``std::string_view`` (trivially copyable pointer+size)
* ``int64_t``, raw pointers (trivially copyable)

**Fix patterns used in this codebase:**

**Scope-before-await**: Build SSO-prone objects inside a ``{}`` scope,
serialize to ``std::string wire`` (long output, > 15 chars, heap-allocated), close scope
(destroying all SSO strings), then ``co_await transport->write_message(wire)``.

.. code-block:: cpp

   std::string wire;
   {
       JSONRPCNotification notification;
       notification.method = std::move(method);  // SSO risk
       notification.params = std::move(params);
       wire = nlohmann::json(std::move(notification)).dump();
   }  // SSO strings destroyed here
   co_await transport_->write_message(wire);  // safe: wire is always > 15 chars

**int64_t for IDs**: Store the request ID as ``int64_t`` across suspensions
(trivially copyable), reconstruct the string after all ``co_await`` calls.

.. code-block:: cpp

   int64_t id = next_id_.fetch_add(1);  // safe across suspension
   // ... co_await ...
   auto id_str = std::to_string(id);  // reconstruct after suspension

**No named temporaries**: Pass ``params.get<In>()`` directly as a function
argument instead of storing it in a named local, so no SSO string ever enters the frame.

.. code-block:: cpp

   // DO NOT: auto input = params.get<In>();  // named local stays in frame
   Out output = co_await handler(ctx, params.get<In>());  // direct: never in frame

**Rule for wire builders**: ``make_result_wire()`` and ``make_error_wire()`` are
**synchronous** static helpers. Do NOT convert them to ``Task<void>`` coroutines —
that would force callers to store ``RequestId`` (SSO risk) in their frames across the
callee's suspensions.

**Detection**: Run tests on Ubuntu 22.04 / GCC 11.
The crash manifests as ``munmap_chunk(): invalid pointer`` at process exit, not at the
point of access, making it hard to debug without valgrind.

Feature Requests
----------------

Feature requests are welcome! Use GitHub Issues with the "enhancement" label.

Include:

* **Use case**: Why do you need this feature?
* **Proposed API**: What should the API look like?
* **Alternatives considered**: Other approaches you evaluated
* **Breaking changes**: Would this break existing code?

Community Guidelines
--------------------

* Be respectful and inclusive
* Provide constructive feedback
* Help newcomers get started
* Celebrate contributions from others

Thank you for contributing to mcp-cpp-sdk!
