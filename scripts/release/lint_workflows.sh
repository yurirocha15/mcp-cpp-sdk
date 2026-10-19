#!/usr/bin/env bash
set -euo pipefail

readonly ACTIONLINT_VERSION="1.7.12"
readonly ACTIONLINT_ARCHIVE="actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz"
readonly ACTIONLINT_SHA256="8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8"
readonly ACTIONLINT_URL="https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/${ACTIONLINT_ARCHIVE}"

# actionlint silently skips its script checks when shellcheck is missing.
if ! command -v shellcheck >/dev/null; then
  echo "shellcheck is required to lint workflow scripts" >&2
  exit 1
fi

repo_root="$(git rev-parse --show-toplevel)"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

curl \
  --fail \
  --location \
  --proto '=https' \
  --retry 3 \
  --show-error \
  --silent \
  --tlsv1.2 \
  --output "${work_dir}/${ACTIONLINT_ARCHIVE}" \
  "${ACTIONLINT_URL}"

printf '%s  %s\n' "${ACTIONLINT_SHA256}" "${work_dir}/${ACTIONLINT_ARCHIVE}" \
  | sha256sum --check --status
tar -xzf "${work_dir}/${ACTIONLINT_ARCHIVE}" -C "${work_dir}" actionlint

mapfile -t workflows < <(
  find "${repo_root}/.github/workflows" -type f \( -name '*.yml' -o -name '*.yaml' \) -print \
    | sort
)

if ((${#workflows[@]} == 0)); then
  echo "No GitHub Actions workflows found" >&2
  exit 1
fi

# Self-hosted runner labels are declared in .github/actionlint.yaml.
cd "${repo_root}"
"${work_dir}/actionlint" -no-color "${workflows[@]}"
