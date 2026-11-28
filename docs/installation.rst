Installation
============

Pick your platform in the table and follow its page.  Each page installs the
SDK for CMake's ``find_package``; :doc:`installation/cmake` then shows how a
project links it.

.. list-table::
   :header-rows: 1
   :class: wrap

   * - Platform
     - Architectures
   * - :doc:`Ubuntu 22.04, 24.04, 26.04 <installation/ubuntu-debian>`
     - amd64, arm64
   * - :doc:`Debian 12, 13 <installation/ubuntu-debian>`
     - amd64, arm64
   * - :doc:`Fedora 43, 44 <installation/fedora>`
     - x86_64, aarch64
   * - :doc:`AlmaLinux, Rocky Linux, RHEL 9, 10 <installation/enterprise-linux>`
     - x86_64, aarch64
   * - :doc:`Arch Linux <installation/arch>`
     - x86_64
   * - :doc:`macOS with Homebrew <installation/homebrew>`
     - Intel, Apple silicon
   * - :doc:`Linux with Homebrew <installation/homebrew>`
     - x86_64, arm64
   * - :doc:`Windows with MSVC 2022 <installation/windows>`
     - x64

Other systems
-------------

Only the systems in the table are built and tested.  On any other system,
including distributions based on one of them, :doc:`installation/source`
builds the SDK from source or as part of your project.

The SDK is not in ConanCenter yet; :doc:`installation/source` can take its
dependencies from Conan 2.

Requirements
------------

A project that uses the SDK needs CMake 3.20 or newer and a C++20 compiler: GCC
11, Clang 14, AppleClang 15 or MSVC 2022 with the v143 toolset.  C++20 and C++23
projects are tested.  The SDK depends on Boost 1.74, nlohmann_json 3.10.5 and
OpenSSL 3.0 or newer, which the Linux and macOS packages install with it.

Versions
--------

Versions ``0.x.y`` are stable releases unless they carry an ``-rc.N`` suffix, but
they are not binary compatible with each other.  Rebuild your project when you
change versions, and do not swap one 0.x shared library for another under an
existing executable.

.. toctree::
   :hidden:

   installation/ubuntu-debian
   installation/fedora
   installation/enterprise-linux
   installation/arch
   installation/homebrew
   installation/windows
   installation/source
   installation/cmake
