ARG BASE_IMAGE
FROM ${BASE_IMAGE}

# json-devel is supplied through the reviewed CRB + EPEL repository topology
# on Enterprise Linux.  The resulting dependency-complete image is later used
# only by immutable digest and with networking disabled.
RUN dnf install --assumeyes dnf-plugins-core epel-release \
    && dnf config-manager --set-enabled crb \
    && dnf install --assumeyes \
        binutils boost-devel cmake cpio gcc-c++ git gtest-devel json-devel \
        ninja-build openssl-devel pkgconf-pkg-config python3 rpm-build \
    && dnf clean all

LABEL org.opencontainers.image.source="https://github.com/yurirocha15/mcp-cpp-sdk" \
      org.opencontainers.image.title="mcp-cpp-sdk Enterprise Linux release builder"
