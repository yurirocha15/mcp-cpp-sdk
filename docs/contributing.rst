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
   python scripts/build.py build

   # Run tests
   python scripts/build.py test

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
   python scripts/build.py test

   # Run the full suite with ctest output on failures
   ctest --preset conan-release --output-on-failure

   # Run the GoogleTest binary directly with a filter
   ./build/mcp-sdk-tests --gtest_filter=ServerCoreTest.*

Code Coverage
^^^^^^^^^^^^^

Aim for >80% code coverage for new features:

.. code-block:: bash

   # Generate coverage report
   python scripts/build.py coverage

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
* Run tests locally: ``python scripts/build.py test``
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
