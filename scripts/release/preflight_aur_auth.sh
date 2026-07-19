#!/usr/bin/env bash
set -euo pipefail

source scripts/release/aur_ssh_agent.sh
trap aur_ssh_cleanup EXIT
aur_ssh_setup aur-preflight

# Arch documents `help` as the authenticated, non-mutating SSH interface probe.
${GIT_SSH_COMMAND} aur@aur.archlinux.org help >/dev/null 2>&1
