Build from Source
=================

This page covers systems without a package and projects that build the SDK
themselves.  Either way, CMake must find Boost 1.74 or newer, nlohmann_json
3.10.5 or newer and OpenSSL 3.0 or newer.

Build the SDK with your project
-------------------------------

CMake's ``FetchContent`` downloads the SDK and builds it with your project:

.. code-block:: cmake

   include(FetchContent)
   FetchContent_Declare(
     mcp-cpp-sdk
     GIT_REPOSITORY https://github.com/yurirocha15/mcp-cpp-sdk.git
     GIT_TAG v0.4.0
   )
   # Skip the SDK's own tests, examples and docs.
   set(BUILD_TESTING OFF)
   set(BUILD_EXAMPLES OFF)
   set(BUILD_DOCS OFF)
   FetchContent_MakeAvailable(mcp-cpp-sdk)

   target_link_libraries(your_target PRIVATE mcp::sdk)

A Git submodule works the same way with ``add_subdirectory``:

.. code-block:: bash

   git submodule add https://github.com/yurirocha15/mcp-cpp-sdk.git third_party/mcp-cpp-sdk

.. code-block:: cmake

   add_subdirectory(third_party/mcp-cpp-sdk)
   target_link_libraries(your_target PRIVATE mcp::sdk)

A project that adds the SDK this way links ``mcp::sdk`` without
``find_package``.

Install from a release's source archive
---------------------------------------

.. _verify-release-files:

1. Download ``release-signing-key.asc``, ``SHA256SUMS``, ``SHA256SUMS.asc`` and
   the source archive ``mcp-cpp-sdk-<version>.tar.gz`` from the
   `GitHub Release <https://github.com/yurirocha15/mcp-cpp-sdk/releases>`__, and
   check them in the folder that holds them:

   .. code-block:: bash

      gpg --show-keys --with-fingerprint release-signing-key.asc
      gpg --import release-signing-key.asc
      gpg --verify SHA256SUMS.asc SHA256SUMS
      sha256sum --check --ignore-missing SHA256SUMS

   The key's fingerprint is
   ``32F4 760A 898C AA62 344F 81B0 78DD AF6A 8010 5366``.  ``gpg --verify``
   reports a good signature by "MCP C++ SDK Release", and ``sha256sum`` reports
   each downloaded file as ``OK``.  Do not build from the source archives that
   GitHub generates for each tag.

2. Extract the archive, then build and install the SDK, here with Conan 2
   providing the dependencies:

   .. code-block:: bash

      conan install . --output-folder=build --build=missing \
        -s compiler.cppstd=20 \
        -c tools.cmake.cmaketoolchain:generator=Ninja
      cmake --preset conan-release -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
      cmake --build --preset conan-release
      cmake --install build/build/Release --prefix /your/install/prefix

   This builds the static library, which ``mcp::sdk`` then names; add
   ``-o "mcp-cpp-sdk/*:shared=True"`` to ``conan install`` for the shared
   library. Conan names the preset and build folder for your platform; use the
   ones it prints.  Pass the install prefix in ``CMAKE_PREFIX_PATH`` when configuring
   your project.

To work on the SDK itself, with its tests and examples, see
:doc:`../contributing`.

Next steps
----------

* :doc:`../getting-started`: a first server and client
* :doc:`cmake`: linking the shared or the static library
