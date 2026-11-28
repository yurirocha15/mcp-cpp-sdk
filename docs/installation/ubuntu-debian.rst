Install on Ubuntu and Debian
============================

Requirements
------------

* Ubuntu 22.04, 24.04 or 26.04, or Debian 12 or 13
* amd64 or arm64

Install from the APT repository
-------------------------------

The APT repository is hosted by Cloudsmith.

1. Add the repository and its signing key:

   .. code-block:: bash

      sudo apt update
      sudo apt install ca-certificates curl
      sudo curl -fsSL https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/gpg.AA58C5CE6D55B3F6.key -o /usr/share/keyrings/mcp-cpp-sdk.asc
      echo "deb [signed-by=/usr/share/keyrings/mcp-cpp-sdk.asc] https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/deb/$(. /etc/os-release && echo "$ID $VERSION_CODENAME") main" | sudo tee /etc/apt/sources.list.d/mcp-cpp-sdk.list

   The key's fingerprint is
   ``1E1A 4FFD 5C7C A18D 7F20 0C3D AA58 C5CE 6D55 B3F6``.

2. Install the SDK:

   .. code-block:: bash

      sudo apt update
      sudo apt install libmcp-cpp-sdk-dev

   This installs the shared library and its headers.  For the static library,
   also install ``libmcp-cpp-sdk-static-dev``.

3. Check the installed version:

   .. code-block:: bash

      dpkg-query --show libmcp-cpp-sdk-dev

The SDK is installed.  :doc:`../getting-started` builds a first server and
client with it.

Upgrade
-------

Install the newest version, then remove the shared library of the previous one:

.. code-block:: bash

   sudo apt update
   sudo apt install libmcp-cpp-sdk-dev
   sudo apt autoremove

Add ``libmcp-cpp-sdk-static-dev`` to the ``apt install`` line if you use the
static library.

Uninstall
---------

1. Remove the packages, including the shared library ``libmcp-cpp-sdk0.4.0``:

   .. code-block:: bash

      sudo apt remove libmcp-cpp-sdk-dev libmcp-cpp-sdk-static-dev libmcp-cpp-sdk0.4.0

2. Remove the repository and its key:

   .. code-block:: bash

      sudo rm /etc/apt/sources.list.d/mcp-cpp-sdk.list /usr/share/keyrings/mcp-cpp-sdk.asc

Next steps
----------

* :doc:`../getting-started`: a first server and client
* :doc:`cmake`: linking the shared or the static library
