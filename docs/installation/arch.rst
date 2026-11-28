Install on Arch Linux
=====================

Requirements
------------

* Arch Linux on x86_64
* ``base-devel`` and ``git``, which building an AUR package needs

Install from the AUR
--------------------

The AUR package ``mcp-cpp-sdk`` builds the SDK from the release's signed source
archive.

1. Import the release signing key, and check that its fingerprint matches
   ``validpgpkeys`` in the ``PKGBUILD``:

   .. code-block:: bash

      curl --fail --location --remote-name \
        https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/v0.4.0/release-signing-key.asc
      gpg --show-keys --with-fingerprint release-signing-key.asc
      gpg --import release-signing-key.asc

2. Get the package and read its ``PKGBUILD``:

   .. code-block:: bash

      git clone https://aur.archlinux.org/mcp-cpp-sdk.git
      cd mcp-cpp-sdk

3. Verify the source signature, then build and install the package:

   .. code-block:: bash

      makepkg --verifysource
      makepkg -si

   The build produces ``mcp-cpp-sdk``, with the shared library and headers, and
   ``mcp-cpp-sdk-static``, with the static library.

4. Check the installed version:

   .. code-block:: bash

      pacman -Q mcp-cpp-sdk

The SDK is installed.  :doc:`../getting-started` builds a first server and
client with it.

Upgrade
-------

In the ``mcp-cpp-sdk`` clone, fetch the new ``PKGBUILD`` and build again:

.. code-block:: bash

   git pull
   makepkg -si

Uninstall
---------

.. code-block:: bash

   sudo pacman -Rns mcp-cpp-sdk-static mcp-cpp-sdk

Leave out ``mcp-cpp-sdk-static`` if it is not installed.

Next steps
----------

* :doc:`../getting-started`: a first server and client
* :doc:`cmake`: linking the shared or the static library
