ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        binutils build-essential ca-certificates cmake debhelper devscripts \
        dpkg-dev fakeroot g++ git libboost-dev libgtest-dev libssl-dev \
        ninja-build nlohmann-json3-dev pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*

LABEL org.opencontainers.image.source="https://github.com/yurirocha15/mcp-cpp-sdk" \
      org.opencontainers.image.title="mcp-cpp-sdk APT release builder"
