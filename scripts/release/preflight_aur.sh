#!/usr/bin/env bash
set -euo pipefail

: "${AUR_PACKAGE_BASE:?}" "${AUR_CLONE_URL:?}"
test "${AUR_PACKAGE_BASE}" = "mcp-cpp-sdk"
test "${AUR_CLONE_URL}" = "ssh://aur@aur.archlinux.org/mcp-cpp-sdk.git"

exec python3 -I -S scripts/run_release_tool.py release.aur_preflight \
  --package "${AUR_PACKAGE_BASE}"
