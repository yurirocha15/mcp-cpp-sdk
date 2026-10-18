ARG BASE_IMAGE
FROM ${BASE_IMAGE}

COPY release/aur_package_installer.py /usr/local/libexec/mcp-install-aur-packages
RUN pacman --sync --refresh --sysupgrade --noconfirm \
    && pacman --sync --needed --noconfirm \
        base-devel binutils boost cmake git gnupg gtest ninja nlohmann-json \
        openssl python sudo \
    && useradd --create-home --uid 1001 builder \
    && chown root:root /usr/local/libexec/mcp-install-aur-packages \
    && chmod 0755 /usr/local/libexec/mcp-install-aur-packages \
    && printf 'builder ALL=(root) NOPASSWD: /usr/local/libexec/mcp-install-aur-packages\n' >/etc/sudoers.d/release-builder \
    && chmod 0440 /etc/sudoers.d/release-builder \
    && pacman --sync --clean --clean --noconfirm

USER builder
LABEL org.opencontainers.image.source="https://github.com/yurirocha15/mcp-cpp-sdk" \
      org.opencontainers.image.title="mcp-cpp-sdk AUR validation builder"
