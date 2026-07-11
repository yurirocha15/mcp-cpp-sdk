Installation
============

Availability rule
-----------------

The release pipeline prepares GitHub Release, Conan 2, APT/DEB, RPM, AUR,
Homebrew, and Chocolatey outputs.  A command in this page is supported only
when the corresponding immutable GitHub Release record says that the route is
``LIVE``.  ``LIVE`` means the public package was queried afresh, installed in a
clean supported environment, and used to compile and run a C++ consumer.

Before the first stable release, or while a registry submission is pending,
use a reviewed source revision.  Do not infer availability from this page,
from an uploaded file, or from an open package-repository pull request.

Supported baseline
------------------

The SDK requires CMake 3.20 or newer, C++20, Boost 1.74 or newer,
nlohmann_json 3.10.5 or newer, and OpenSSL 3.0 or newer.  Supported compiler
minima are GCC 11, Clang 14, AppleClang 15, and MSVC 2022 with the v143
toolset.  C++20 and C++23 consumers are tested independently.

The initial native-package baselines are:

* Ubuntu 22.04, 24.04, and 26.04 on amd64 and arm64;
* Debian 12 and 13 on amd64 and arm64;
* Fedora 43 and 44 on x86_64 and aarch64;
* Enterprise Linux 9 and 10 on x86_64 and aarch64; AlmaLinux, Rocky Linux,
  and other derivatives require their own recorded clean consumer check;
* current Arch Linux on x86_64 and aarch64;
* current supported Homebrew macOS and Linux bottle runners; and
* Windows x64 with MSVC 2022 and the dynamic CRT.

A derivative is not automatically a tested baseline.  It is supported only
after a clean install and consumer test for that derivative is recorded by a
release.

CMake linkage selection
-----------------------

The installed package exports three targets:

``mcp::sdk_shared``
   Explicit shared-library linkage.

``mcp::sdk_static``
   Explicit static-library linkage.  Static consumers must relink when they
   update the SDK.

``mcp::sdk``
   The documented default selected by the installed package configuration.

For an explicit variant:

.. code-block:: cmake

   find_package(mcp-cpp-sdk 0.2 CONFIG REQUIRED)
   target_link_libraries(my_application PRIVATE mcp::sdk_shared)

Replace the last target with ``mcp::sdk_static`` for static linkage.  Do not
select a variant by guessing a physical library filename.

Conan 2
-------

Conan packages are delivered through a recipe contribution to ConanCenter;
there is no private Conan server.  After the release record marks ConanCenter
``LIVE``:

.. code-block:: bash

   conan install --requires=mcp-cpp-sdk/0.2.0 \
     --options=mcp-cpp-sdk/*:shared=True --build=missing

Use ``shared=False`` for the static package ID.  Pin the version and retain the
generated lockfile when reproducibility matters.  ConanCenter review may finish
after the GitHub Release; an open recipe pull request is not an installable
route.

APT / DEB
---------

The public APT repository is hosted by Cloudsmith.  Once the release record
marks the exact distribution route ``LIVE``, obtain the repository setup
instructions from the public repository and inspect the downloaded script
before running it.  The canonical setup endpoint is:

.. code-block:: text

   https://dl.cloudsmith.io/public/yurirocha15/mcp-cpp-sdk/cfg/setup/bash.deb.sh

Then install shared development files, with the optional static archive:

.. code-block:: bash

   sudo apt update
   sudo apt install libmcp-cpp-sdk-dev
   sudo apt install libmcp-cpp-sdk-static-dev  # optional static target

The package manager installs the matching ``libmcp-cpp-sdk0.2`` shared runtime
when needed.  Remove the SDK with:

.. code-block:: bash

   sudo apt remove libmcp-cpp-sdk-dev libmcp-cpp-sdk-static-dev \
     libmcp-cpp-sdk0.2

RPM
---

The public RPM repository is also hosted by Cloudsmith.  Use its setup
instructions only after the release record marks the matching Fedora or
Enterprise Linux route ``LIVE``.  The canonical setup endpoint is:

.. code-block:: text

   https://dl.cloudsmith.io/public/yurirocha15/mcp-cpp-sdk/cfg/setup/bash.rpm.sh

Install the development package and, when required, the static add-on:

.. code-block:: bash

   sudo dnf install mcp-cpp-sdk-devel
   sudo dnf install mcp-cpp-sdk-static  # optional static target

Uninstall with:

.. code-block:: bash

   sudo dnf remove mcp-cpp-sdk-static mcp-cpp-sdk-devel \
     mcp-cpp-sdk0.2-libs

Arch Linux and derivatives
--------------------------

The AUR package base is ``mcp-cpp-sdk`` and produces shared and static split
packages.  After the AUR route is ``LIVE``, inspect the ``PKGBUILD`` and verify
the source signature before building:

.. code-block:: bash

   git clone https://aur.archlinux.org/mcp-cpp-sdk.git
   cd mcp-cpp-sdk
   makepkg --verifysource
   makepkg -si

Remove installed split packages with ``pacman -Rns mcp-cpp-sdk`` and, if
installed, ``mcp-cpp-sdk-static``.  Manjaro and other derivatives become
supported only when their release-specific clean consumer test is recorded.

Homebrew
--------

After the Homebrew route is ``LIVE``:

.. code-block:: bash

   brew tap yurirocha15/mcp-cpp-sdk
   brew install mcp-cpp-sdk

The formula installs both library variants and their CMake metadata.  Use
``brew uninstall mcp-cpp-sdk`` to remove it.  Homebrew falls back to a source
build where a release has no verified bottle for the host.

Chocolatey
----------

After Chocolatey moderation completes and the release record says ``LIVE``,
install the x64 developer SDK from an elevated shell:

.. code-block:: powershell

   choco install mcp-cpp-sdk --version=0.2.0

The package contains shared and static SDK variants for MSVC 2022.  Remove it
with ``choco uninstall mcp-cpp-sdk``.  A new shell receives
``MCP_CPP_SDK_ROOT``; pass it to CMake when configuring a consumer:

.. code-block:: powershell

   cmake -S . -B build -DCMAKE_PREFIX_PATH="$env:MCP_CPP_SDK_ROOT"

GitHub Release and source builds
The Chocolatey package does not redistribute third-party development SDKs.
Provide x64 MSVC-compatible Boost 1.74 or newer, nlohmann-json 3.10.5 or newer,
and OpenSSL 3.x, then include their prefixes alongside
``MCP_CPP_SDK_ROOT`` in ``CMAKE_PREFIX_PATH``.  Conan 2 or vcpkg may be
used to supply those dependencies.  Do not mix compiler toolsets or CRT
linkages with the packaged MSVC 2022 dynamic-CRT binaries.

--------------------------------

Each stable GitHub Release provides deterministic source archives, detached
OpenPGP signatures, ``SHA256SUMS`` with its signature, SBOMs, provenance, and a
signed release manifest.  Verify the checksum signature and source-archive
signature before building.  Do not use GitHub's automatically generated source
archives as release inputs.

For a source checkout with Conan 2 dependencies:

.. code-block:: bash

   conan install . --output-folder=build --build=missing \
     -s compiler.cppstd=20 \
     -c tools.cmake.cmaketoolchain:generator=Ninja
   cmake --preset conan-release -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
   cmake --build --preset conan-release
   cmake --install build/Release --prefix /your/install/prefix

Use the actual preset/build directory emitted by Conan on your platform.

AppImage and Snap
-----------------

AppImage and Snap are not produced.  The repository currently delivers a C++
library rather than an end-user application, so neither format has a valid
application payload.
