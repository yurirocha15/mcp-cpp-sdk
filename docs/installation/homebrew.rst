Install with Homebrew
=====================

Requirements
------------

* Homebrew on macOS (Intel or Apple silicon) or on Linux (x86_64 or arm64)

Install from the tap
--------------------

1. Install the SDK from the project's tap:

   .. code-block:: bash

      brew install yurirocha15/mcp-cpp-sdk/mcp-cpp-sdk

   The formula installs the shared and static libraries, the headers and their
   CMake files.  Where a release has no verified bottle for the host, Homebrew
   builds the SDK from source.

2. Check the installed version:

   .. code-block:: bash

      brew list --versions mcp-cpp-sdk

The SDK is installed.  :doc:`../getting-started` builds a first server and
client with it.

Upgrade
-------

.. code-block:: bash

   brew update
   brew upgrade mcp-cpp-sdk

Uninstall
---------

.. code-block:: bash

   brew uninstall mcp-cpp-sdk
   brew untap yurirocha15/mcp-cpp-sdk

Next steps
----------

* :doc:`../getting-started`: a first server and client
* :doc:`cmake`: linking the shared or the static library
