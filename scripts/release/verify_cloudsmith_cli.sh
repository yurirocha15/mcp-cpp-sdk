#!/usr/bin/env bash
set -euo pipefail

readonly cli_path="bin/cloudsmith"
readonly expected_sha256="c076e4b002ee07f26774c0f8a9134f52a73b16a3fb10adb31891475485e28038"

test -f "${cli_path}"
test ! -L "${cli_path}"
printf '%s  %s\n' "${expected_sha256}" "${cli_path}" | sha256sum --check --status

resolved_cli="$(command -v cloudsmith)"
test "$(readlink -f "${resolved_cli}")" = "$(readlink -f "${cli_path}")"

mapfile -t version_lines < <("${cli_path}" --version)
if ((${#version_lines[@]} != 3)) \
  || [[ "${version_lines[0]}" != "Versions:" ]] \
  || [[ "${version_lines[1]}" != "CLI Package Version: 1.19.0" ]] \
  || [[ "${version_lines[2]}" != "API Package Version: 2.0.27" ]]; then
  echo "Cloudsmith CLI version payload differs from the reviewed release" >&2
  exit 1
fi
