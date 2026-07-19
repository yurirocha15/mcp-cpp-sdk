# syntax=docker/dockerfile:1.7
FROM ubuntu@sha256:52df9b1ee71626e0088f7d400d5c6b5f7bb916f8f0c82b474289a4ece6cf3faf

ARG DEBIAN_FRONTEND=noninteractive

COPY ubuntu.sources /etc/apt/sources.list.d/ubuntu.sources

# The minimal Ubuntu base has no CA bundle, while the signed Ubuntu snapshot
# service redirects to HTTPS. Disable only TLS peer checking long enough to
# install ca-certificates from APT's GPG-authenticated, digest-bound snapshot;
# then remove the override and prove the remaining update works with TLS.
RUN printf 'Acquire::https::snapshot.ubuntu.com::Verify-Peer "false";\n' \
        >/etc/apt/apt.conf.d/99snapshot-ca-bootstrap \
    && apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates \
    && rm /etc/apt/apt.conf.d/99snapshot-ca-bootstrap \
    && apt-get update \
    && apt-get install --yes --no-install-recommends \
        abigail-tools \
        binutils \
        cmake \
        g++-13 \
        git \
        libboost-dev \
        libssl-dev \
        ninja-build \
        nlohmann-json3-dev \
        pkg-config \
        python3 \
    && abidw --version | grep --extended-regexp --quiet '(^|[^0-9])2\.4([^0-9]|$)' \
    && test "$(g++-13 -dumpversion | cut -d. -f1)" = 13 \
    && rm -rf /var/lib/apt/lists/*

LABEL org.opencontainers.image.title="mcp-cpp-sdk canonical ABI builder" \
      org.opencontainers.image.description="Ubuntu Noble amd64, GCC 13, and libabigail 2.4"
