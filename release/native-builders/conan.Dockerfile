ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
COPY release/native-builders/conan-requirements.txt /tmp/conan-requirements.txt
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        autoconf automake binutils build-essential ca-certificates cmake git \
        libtool make nasm ninja-build pkg-config python3 python3-venv \
    && python3 -m venv /opt/conan \
    && /opt/conan/bin/python -m pip install \
        --disable-pip-version-check --no-cache-dir --only-binary=:all: \
        --require-hashes --requirement /tmp/conan-requirements.txt \
    && test "$(/opt/conan/bin/conan --version)" = "Conan version 2.30.0" \
    && rm -f /tmp/conan-requirements.txt \
    && rm -rf /var/lib/apt/lists/*

ENV PATH="/opt/conan/bin:${PATH}"
LABEL org.opencontainers.image.source="https://github.com/yurirocha15/mcp-cpp-sdk" \
      org.opencontainers.image.title="mcp-cpp-sdk Conan candidate validator"
