ARG BASE_IMAGE
FROM ${BASE_IMAGE}

RUN dnf install --assumeyes \
        binutils boost-devel cmake cpio gcc-c++ git gtest-devel json-devel \
        ninja-build openssl-devel pkgconf-pkg-config python3 rpm-build \
    && dnf clean all

LABEL org.opencontainers.image.source="https://github.com/yurirocha15/mcp-cpp-sdk" \
      org.opencontainers.image.title="mcp-cpp-sdk Fedora release builder"
