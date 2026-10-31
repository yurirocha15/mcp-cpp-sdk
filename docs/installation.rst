Installation
============

Requirements
------------

The SDK requires CMake 3.20 or newer, C++20, Boost 1.74 or newer,
nlohmann_json 3.10.5 or newer, and OpenSSL 3.0 or newer.  The minimum compilers
are GCC 11, Clang 14, AppleClang 15, and MSVC 2022 with the v143 toolset.  Both
C++20 and C++23 projects are tested.

Versions ``0.x.y`` are stable releases unless they carry an ``-rc.N`` suffix, but
they are not binary compatible with each other.  Rebuild your project when you
change versions, and do not swap one 0.x shared library for another under an
existing executable.

Packages are built for:

* Ubuntu 22.04, 24.04, and 26.04 on amd64 and arm64;
* Debian 12 and 13 on amd64 and arm64;
* Fedora 43 and 44 on x86_64 and aarch64;
* Enterprise Linux 9 and 10 on x86_64 and aarch64, which AlmaLinux, Rocky Linux
  and other compatible distributions can use;
* current Arch Linux on x86_64;
* Homebrew on macOS (Intel and Apple silicon) and Linux (x86_64 and arm64); and
* Windows x64 with MSVC 2022 and the dynamic CRT.

Distributions based on these can usually use their parent's packages, and Arch
derivatives can build the AUR package, but only the systems listed here are built
and tested.

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

   find_package(mcp-cpp-sdk 0.2.0 EXACT CONFIG REQUIRED)
   target_link_libraries(my_application PRIVATE mcp::sdk_shared)

Replace the last target with ``mcp::sdk_static`` for static linkage.  Link these
targets rather than a library filename.

The packages also install pkg-config files: ``mcp-cpp-sdk`` (the default),
``mcp-cpp-sdk-shared`` and ``mcp-cpp-sdk-static``.  ``pkg-config --libs
mcp-cpp-sdk-static`` links the static SDK against the system's shared OpenSSL.
``--static`` adds OpenSSL's own static dependencies, which on Fedora, Enterprise
Linux, Debian 13 and Ubuntu 26.04 include libraries (zlib, zstd) that the
OpenSSL development package does not install.

Conan 2
-------

The SDK is not in ConanCenter yet.  Until it is, build it from source as
described in `GitHub Release and source builds`_; that route can use Conan 2
for the dependencies.

APT / DEB
---------

The APT repository is hosted by Cloudsmith and serves Ubuntu 22.04, 24.04 and
26.04 and Debian 12 and 13.  Add its signing key and the repository for your
distribution:

.. code-block:: bash

   sudo curl -fsSL https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/gpg.AA58C5CE6D55B3F6.key -o /usr/share/keyrings/mcp-cpp-sdk.asc
   echo "deb [signed-by=/usr/share/keyrings/mcp-cpp-sdk.asc] https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/deb/$(. /etc/os-release && echo "$ID $VERSION_CODENAME") main" | sudo tee /etc/apt/sources.list.d/mcp-cpp-sdk.list

The repository signing key's fingerprint is
``1E1A 4FFD 5C7C A18D 7F20 0C3D AA58 C5CE 6D55 B3F6``.  Cloudsmith's setup
script does the same:

.. code-block:: bash

   curl -fsSL https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/cfg/setup/bash.deb.sh | sudo bash

Then install the shared development files, with the optional static archive:

.. code-block:: bash

   sudo apt update
   sudo apt install libmcp-cpp-sdk-dev
   sudo apt install libmcp-cpp-sdk-static-dev  # optional static target

The package manager installs the matching ``libmcp-cpp-sdk0.2.0`` shared runtime
when needed.  Remove the SDK with:

.. code-block:: bash

   sudo apt remove libmcp-cpp-sdk-dev libmcp-cpp-sdk-static-dev \
     libmcp-cpp-sdk0.2.0

RPM
---

The RPM repository is also hosted by Cloudsmith and serves Fedora 43 and 44
and Enterprise Linux 9 and 10.  Add the repository; on Enterprise Linux,
replace ``rpm/fedora/`` with ``rpm/el/``:

.. code-block:: bash

   sudo tee /etc/yum.repos.d/mcp-cpp-sdk.repo <<'REPO'
   [mcp-cpp-sdk]
   name=mcp-cpp-sdk
   baseurl=https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/rpm/fedora/$releasever/$basearch
   gpgkey=https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/gpg.AA58C5CE6D55B3F6.key
   gpgcheck=1
   repo_gpgcheck=1
   REPO

Cloudsmith's setup script does the same:

.. code-block:: bash

   curl -fsSL https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/cfg/setup/bash.rpm.sh | sudo bash

On Fedora 44, the repository file the script writes points at a CA bundle that
Fedora 44 no longer ships, so ``dnf`` cannot load the repository.  Remove that
line before installing:

.. code-block:: bash

   sudo sed -i '/^sslcacert=/d' /etc/yum.repos.d/mcp-cpp-sdk-mcp-cpp-sdk.repo

On AlmaLinux and Rocky Linux 9 or 10, enable CRB and EPEL before installing
the development package; they provide the ``json-devel`` dependency used by
the exported CMake target:

.. code-block:: bash

   sudo dnf install dnf-plugins-core
   sudo dnf config-manager --set-enabled crb
   sudo dnf install epel-release

RHEL users must enable the corresponding CodeReady Builder repository through
their subscription and then follow the official EPEL instructions for that
major version.  Do not use the AlmaLinux/Rocky repository command blindly on
RHEL.  Fedora does not need this Enterprise Linux prerequisite.

Install the development package and, when required, the static add-on:

.. code-block:: bash

   sudo dnf install mcp-cpp-sdk-devel
   sudo dnf install mcp-cpp-sdk-static  # optional static target

Uninstall with:

.. code-block:: bash

   sudo dnf remove mcp-cpp-sdk-static mcp-cpp-sdk-devel \
     mcp-cpp-sdk0.2.0-libs

Arch Linux and derivatives
--------------------------

The AUR package ``mcp-cpp-sdk`` is built and tested on x86_64 and produces shared
and static split
packages.  Inspect the ``PKGBUILD`` and verify the source signature before
building.  First import the exact public release key that is attached to the
immutable GitHub Release, inspect its fingerprint, and compare it with
``validpgpkeys`` in the ``PKGBUILD``:

.. code-block:: bash

   curl --fail --location --remote-name \
     https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/v0.2.0/release-signing-key.asc
   gpg --show-keys --with-fingerprint release-signing-key.asc
   gpg --import release-signing-key.asc

.. code-block:: bash

   git clone https://aur.archlinux.org/mcp-cpp-sdk.git
   cd mcp-cpp-sdk
   makepkg --verifysource
   makepkg -si

Remove installed split packages with ``pacman -Rns mcp-cpp-sdk`` and, if
installed, ``mcp-cpp-sdk-static``.  Manjaro and other Arch derivatives can
build the AUR package when they remain compatible with the targeted Arch
dependencies, but they are not tested separately.

Homebrew
--------

Homebrew on macOS and Linux installs the SDK from the project's tap:

.. code-block:: bash

   brew install yurirocha15/mcp-cpp-sdk/mcp-cpp-sdk

The formula installs both library variants and their CMake metadata.  Use
``brew uninstall mcp-cpp-sdk`` to remove it.  Homebrew falls back to a source
build where a release has no verified bottle for the host.

Chocolatey
----------

The Chocolatey package is not in the Chocolatey Community Repository yet;
until it is, use the Windows SDK zip from the GitHub Release.  Once it is
published, install the x64 developer SDK from an elevated shell:

.. code-block:: powershell

   choco install mcp-cpp-sdk --version=0.2.0

The package contains shared and static SDK variants for MSVC 2022.  Remove it
with ``choco uninstall mcp-cpp-sdk``.  A new shell receives
``MCP_CPP_SDK_ROOT``; pass it to CMake when configuring a consumer:

.. code-block:: powershell

   cmake -S . -B build -DCMAKE_PREFIX_PATH="$env:MCP_CPP_SDK_ROOT"

The Chocolatey package does not redistribute third-party development SDKs.
It declares Chocolatey's ``vcredist140`` package so the current supported
Microsoft Visual C++ runtime is installed before the shared SDK is used.
Provide x64 MSVC-compatible Boost 1.74 or newer, nlohmann-json 3.10.5 or newer,
and OpenSSL 3.x, then include their prefixes alongside
``MCP_CPP_SDK_ROOT`` in ``CMAKE_PREFIX_PATH``.  Conan 2 or vcpkg may be
used to supply those dependencies.  Do not mix compiler toolsets or CRT
linkages with the packaged MSVC 2022 dynamic-CRT binaries.

GitHub Release and source builds
--------------------------------

Each stable GitHub Release provides deterministic source archives, detached
OpenPGP signatures, ``SHA256SUMS`` with its signature, SBOMs, provenance, and a
signed release manifest.  Verify the checksum signature and source-archive
signature before building.  Do not use GitHub's
automatically generated source archives as release inputs.

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
