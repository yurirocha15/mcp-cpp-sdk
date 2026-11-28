Install on AlmaLinux, Rocky Linux and RHEL
==========================================

Requirements
------------

* AlmaLinux, Rocky Linux or Red Hat Enterprise Linux 9 or 10
* x86_64 or aarch64
* The CRB (CodeReady Builder) and EPEL repositories, which provide the
  ``json-devel`` package the SDK needs

Enable CRB and EPEL
-------------------

Follow the part for your distribution only.

AlmaLinux and Rocky Linux
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: bash

   sudo dnf install dnf-plugins-core
   sudo dnf config-manager --set-enabled crb
   sudo dnf install epel-release

Red Hat Enterprise Linux
^^^^^^^^^^^^^^^^^^^^^^^^

Enable CodeReady Builder through your subscription and install EPEL as the
`EPEL guide <https://docs.fedoraproject.org/en-US/epel/getting-started/>`__
describes for your major version.  The AlmaLinux and Rocky Linux commands do not
apply to RHEL.

Install from the RPM repository
-------------------------------

The RPM repository is hosted by Cloudsmith.

1. Add the repository:

   .. code-block:: bash

      sudo tee /etc/yum.repos.d/mcp-cpp-sdk.repo <<'REPO'
      [mcp-cpp-sdk]
      name=mcp-cpp-sdk
      baseurl=https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/rpm/el/$releasever/$basearch
      gpgkey=https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/gpg.AA58C5CE6D55B3F6.key
      gpgcheck=1
      repo_gpgcheck=1
      REPO

2. Install the SDK:

   .. code-block:: bash

      sudo dnf install mcp-cpp-sdk-devel

   ``dnf`` asks to import the repository's signing key; its fingerprint is
   ``1E1A 4FFD 5C7C A18D 7F20 0C3D AA58 C5CE 6D55 B3F6``.  This installs the
   shared library and its headers.  For the static library, also install
   ``mcp-cpp-sdk-static``.

3. Check the installed version:

   .. code-block:: bash

      rpm -q mcp-cpp-sdk-devel

The SDK is installed.  :doc:`../getting-started` builds a first server and
client with it.

Upgrade
-------

Install the newest version, then remove the shared library of the previous one:

.. code-block:: bash

   sudo dnf upgrade mcp-cpp-sdk-devel
   sudo dnf autoremove

Add ``mcp-cpp-sdk-static`` to the ``dnf upgrade`` line if you use the static
library.

Uninstall
---------

1. Remove the packages, including the shared library:

   .. code-block:: bash

      sudo dnf remove mcp-cpp-sdk-static mcp-cpp-sdk-devel mcp-cpp-sdk0.4.0-libs

2. Remove the repository:

   .. code-block:: bash

      sudo rm /etc/yum.repos.d/mcp-cpp-sdk.repo

Next steps
----------

* :doc:`../getting-started`: a first server and client
* :doc:`cmake`: linking the shared or the static library
