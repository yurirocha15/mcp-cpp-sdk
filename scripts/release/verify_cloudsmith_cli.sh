#!/usr/bin/env bash
set -euo pipefail

readonly cli_version="1.30.0"
readonly cli_target="linux-x86_64-gnu"
# cloudsmith-1.30.0-linux-x86_64-gnu.tar.gz from the Cloudsmith CLI release.
readonly expected_archive_sha256="21898e015ea167f5916a14ebd87b4a5c299f1484013cb45b0f82ec0c14aa000b"
# SHA-256 of the sorted "sha256  path" list of every file the archive installs.
readonly expected_tree_sha256="de4b256c37564e937501827b6a895bf4ce6649b10a7dfd4ce991da2f275b5d1d"

: "${RUNNER_TEMP:?RUNNER_TEMP is required}"
: "${CLOUDSMITH_CLI_PATH:?CLOUDSMITH_CLI_PATH is required}"
readonly install_dir="${RUNNER_TEMP}/cloudsmith-cli/${cli_version}/${cli_target}/cloudsmith"
readonly cli_path="${install_dir}/cloudsmith"
readonly metadata="${install_dir}/.cloudsmith-installation"

test "${CLOUDSMITH_CLI_PATH}" = "${cli_path}"
test -d "${install_dir}"
test ! -L "${install_dir}"
test -f "${cli_path}"
test -f "${metadata}"
test ! -L "${metadata}"
grep -Fxq "version=${cli_version}" "${metadata}"
grep -Fxq "target=${cli_target}" "${metadata}"
grep -Fxq "archive_sha256=${expected_archive_sha256}" "${metadata}"

if [[ -n "$(find "${install_dir}" ! -type f ! -type d -print -quit)" ]]; then
  echo "Cloudsmith CLI installation contains an entry that is not a file or directory" >&2
  exit 1
fi
tree_sha256="$(
  cd "${install_dir}"
  find . -type f ! -path ./.cloudsmith-installation -print0 \
    | LC_ALL=C sort -z \
    | xargs -0 sha256sum \
    | sha256sum \
    | cut -d ' ' -f 1
)"
if [[ "${tree_sha256}" != "${expected_tree_sha256}" ]]; then
  echo "Cloudsmith CLI installation differs from the reviewed release" >&2
  exit 1
fi

resolved_cli="$(command -v cloudsmith)"
test "$(readlink -f "${resolved_cli}")" = "$(readlink -f "${cli_path}")"

mapfile -t version_lines < <("${cli_path}" --version)
if ((${#version_lines[@]} != 3)) \
  || [[ "${version_lines[0]}" != "Versions:" ]] \
  || [[ "${version_lines[1]}" != "CLI Package Version: ${cli_version}" ]] \
  || [[ "${version_lines[2]}" != "API Package Version: 2.0.38" ]]; then
  echo "Cloudsmith CLI version payload differs from the reviewed release" >&2
  exit 1
fi
