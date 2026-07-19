Installation
============

Availability rule
-----------------

The release pipeline prepares GitHub Release, Conan 2, APT/DEB, RPM, AUR,
Homebrew, and Chocolatey outputs.  The GitHub, Cloudsmith (APT/RPM), and AUR
publishers query the exact public object and record ``PUBLISHED`` only after
its identity matches the release.  ConanCenter, Homebrew, and Chocolatey are
provider-controlled review or moderation routes and remain pending until the
destination accepts the release.

AppImage and Snap are not release routes for this project because the SDK is a
C++ library rather than a standalone application.  Use a native development
package, Conan 2, Homebrew, Chocolatey, or a verified source archive instead.

A package-manager route is installable only after the requested version
appears at that route's public provider.  Before the first stable release, or
while a submission is pending, use a reviewed source revision.  Do not infer
availability from this page, an uploaded file, or an open package-repository
pull request.

Supported baseline
------------------

The SDK requires CMake 3.20 or newer, C++20, Boost 1.74 or newer,
nlohmann_json 3.10.5 or newer, and OpenSSL 3.0 or newer.  Supported compiler
minima are GCC 11, Clang 14, AppleClang 15, and MSVC 2022 with the v143
toolset.  C++20 and C++23 consumers are tested independently.

Every ``0.x.y`` is a stable release unless its version has an ``-rc.N``
suffix, but SemVer does not promise binary compatibility before 1.0.  Each
stable 0.x release therefore has an exact shared-library/DLL identity and an
exact CMake package-version match.  Rebuild or relink consumers when changing
0.x versions; do not replace one 0.x shared binary underneath an executable
built against another.  A 1.x dispatch is deliberately blocked until the
project adopts and validates ABI policy for every released platform family.

The native-package build matrix targets:

* Ubuntu 22.04, 24.04, and 26.04 on amd64 and arm64;
* Debian 12 and 13 on amd64 and arm64;
* Fedora 43 and 44 on x86_64 and aarch64;
* Enterprise Linux 9 and 10 on x86_64 and aarch64; AlmaLinux, Rocky Linux,
  and other compatible derivatives can use the matching Enterprise Linux
  route but are not independently tested;
* current Arch Linux on x86_64;
* current supported Homebrew macOS and Linux bottle runners; and
* Windows x64 with MSVC 2022 and the dynamic CRT.

Compatible Ubuntu and Debian derivatives can use the route for their base
distribution, compatible Fedora derivatives can use the matching Fedora or
Enterprise Linux route, and Arch derivatives can build the AUR package.  Only
the exact systems listed above are release-build baselines, so verify the
derivative's base version, architecture, dependency versions, and ABI before
deployment.

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

Replace the last target with ``mcp::sdk_static`` for static linkage.  Do not
select a variant by guessing a physical library filename.

Conan 2
-------

Conan packages are delivered through a recipe contribution to ConanCenter;
there is no private Conan server.  Install only after the exact version appears
in ConanCenter's public package index:

.. code-block:: bash

   conan install --requires=mcp-cpp-sdk/0.2.0 \
     --options=mcp-cpp-sdk/*:shared=True --build=missing

Use ``shared=False`` for the static package ID.  Pin the version and retain the
generated lockfile when reproducibility matters.  ConanCenter review may finish
after the GitHub Release; an open recipe pull request is not an installable
route.

APT / DEB
---------

The public APT repository is hosted by Cloudsmith.  The publisher reads each
exact package identity back before recording the route ``PUBLISHED``.  Once
the requested version appears for your distribution, obtain the repository
setup instructions from the public repository and inspect the downloaded
script before running it.  The canonical setup endpoint is:

.. code-block:: text

   https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/cfg/setup/bash.deb.sh

Then install shared development files, with the optional static archive:

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

The public RPM repository is also hosted by Cloudsmith.  The publisher reads
each exact package identity back before recording the route ``PUBLISHED``.
Use its setup instructions only after the requested version appears for the
matching Fedora or Enterprise Linux route.  The canonical setup endpoint is:

.. code-block:: text

   https://dl.cloudsmith.io/public/mcp-cpp-sdk/mcp-cpp-sdk/cfg/setup/bash.rpm.sh

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

The official AUR release validation baseline is x86_64.  The AUR package base
is ``mcp-cpp-sdk`` and produces shared and static split
packages.  The publisher records ``PUBLISHED`` only after reading the exact
remote package tree back.  After the package base shows the requested
``pkgver``, inspect the ``PKGBUILD`` and verify the source signature before
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
dependencies, but they are not independent release-build baselines.

Homebrew
--------

Homebrew publication is a provider-controlled handoff.  Install only after the
requested formula version appears in the destination tap; an open formula pull
request is not installable:

.. code-block:: bash

   brew tap yurirocha15/mcp-cpp-sdk
   brew install mcp-cpp-sdk

The formula installs both library variants and their CMake metadata.  Use
``brew uninstall mcp-cpp-sdk`` to remove it.  Homebrew falls back to a source
build where a release has no verified bottle for the host.

Chocolatey
----------

Install the x64 developer SDK from an elevated shell only after Chocolatey
moderation completes and the exact version appears in the public Chocolatey
Community Repository.  A pending submission is not installable:

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
signature before building.  The publisher records ``PUBLISHED`` only after
reading the exact immutable release and assets back.  Do not use GitHub's
automatically generated source archives as release inputs.

For a source checkout with Conan 2 dependencies:

.. code-block:: bash

   python3 scripts/build.py --cppstd 20 --linkage both
   cmake --install build/release --prefix /your/install/prefix

Use ``--linkage shared`` or ``--linkage static`` when only one library variant
is required.  The build helper creates an exact Conan 2 dependency profile and
configures the corresponding CMake package exports.

AppImage and Snap
-----------------

AppImage and Snap are not produced.  The repository currently delivers a C++
library rather than an end-user application, so neither format has a valid
application payload.
