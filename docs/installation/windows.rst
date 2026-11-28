Install on Windows
==================

Requirements
------------

* Windows x64 with Visual Studio 2022 (MSVC v143)
* Boost 1.74 or newer, nlohmann_json 3.10.5 or newer and OpenSSL 3.x, built for
  x64 MSVC with the dynamic CRT, for example from vcpkg or Conan 2

The Windows packages hold the SDK only; they do not include those libraries.
Build your project with the same toolset and CRT as the SDK.

Install with Chocolatey
-----------------------

1. In an elevated PowerShell, install the SDK from the Chocolatey Community
   Repository:

   .. code-block:: powershell

      choco install mcp-cpp-sdk --version=0.4.0

   If ``choco`` cannot find that version yet, `install from the zip`_ instead.
   The package declares Chocolatey's ``vcredist140``, so the Visual C++ runtime
   is installed with it.

2. Open a new shell, which receives ``MCP_CPP_SDK_ROOT``, the folder holding the
   SDK, and check the installed version:

   .. code-block:: powershell

      choco list mcp-cpp-sdk

3. Configure your project with the SDK and the folders of its dependencies in
   ``CMAKE_PREFIX_PATH``:

   .. code-block:: powershell

      cmake -S . -B build -DCMAKE_PREFIX_PATH="$env:MCP_CPP_SDK_ROOT;<dependency prefixes>"

A program linked to the shared library loads ``mcp-cpp-sdk-<version>.dll`` from
``$env:MCP_CPP_SDK_ROOT\bin`` at run time: copy it next to the program or add
that folder to ``PATH``.

The SDK is installed.  :doc:`../getting-started` builds a first server and
client with it.

Install from the zip
--------------------

Each GitHub Release has the SDK as
``mcp-cpp-sdk-<version>-windows-x64-v143-md.zip``.  Extract it and use the
extracted folder where the Chocolatey steps use ``MCP_CPP_SDK_ROOT``.  Check the
zip against the release's ``SHA256SUMS`` first, as
:ref:`Build from Source <verify-release-files>` shows for the source archive.

Upgrade
-------

.. code-block:: powershell

   choco upgrade mcp-cpp-sdk

Uninstall
---------

.. code-block:: powershell

   choco uninstall mcp-cpp-sdk

Next steps
----------

* :doc:`../getting-started`: a first server and client
* :doc:`cmake`: linking the shared or the static library
