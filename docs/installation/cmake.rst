Use the SDK with CMake
======================

An installed SDK is found with ``find_package``:

.. code-block:: cmake

   find_package(mcp-cpp-sdk 0.4.0 EXACT CONFIG REQUIRED)
   target_link_libraries(my_application PRIVATE mcp::sdk)

The package exports three targets:

``mcp::sdk``
   The shared library.  ``find_package(mcp-cpp-sdk CONFIG REQUIRED COMPONENTS
   static)`` makes it the static library.

``mcp::sdk_shared``
   The shared library.

``mcp::sdk_static``
   The static library, where it is installed.  A program linked to it must be
   relinked when the SDK is updated.

Link these targets rather than a library file name.

pkg-config
----------

The packages also install pkg-config files: ``mcp-cpp-sdk`` (the shared
library), ``mcp-cpp-sdk-shared`` and ``mcp-cpp-sdk-static``.  ``pkg-config --libs
mcp-cpp-sdk-static`` links the static SDK against the system's shared OpenSSL.
``--static`` adds OpenSSL's own static dependencies, which on Fedora, Enterprise
Linux, Debian 13 and Ubuntu 26.04 include libraries (zlib, zstd) that the
OpenSSL development package does not install.
