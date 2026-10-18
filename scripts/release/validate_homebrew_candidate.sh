#!/usr/bin/env bash
set -euo pipefail

if (($# != 3)); then
  echo "usage: validate_homebrew_candidate.sh VERSION CORE_DIR WORK_DIR" >&2
  exit 2
fi

readonly version="$1"
readonly core_dir="$2"
readonly work_dir="$3"
readonly formula="${core_dir}/homebrew-mcp-cpp-sdk.rb"
readonly archive="${core_dir}/mcp-cpp-sdk-${version}.tar.gz"
readonly tap="mcp-cpp-sdk/validation"

[[ "${version}" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]
test -f "${formula}"
test ! -L "${formula}"
test -f "${archive}"
test ! -L "${archive}"
test ! -e "${work_dir}"
command -v brew >/dev/null
command -v python3 >/dev/null

export HOMEBREW_NO_ANALYTICS=1
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1

brew tap-new "${tap}"
tap_root="$(brew --repository "${tap}")"
test -d "${tap_root}/Formula"
production="${tap_root}/Formula/mcp-cpp-sdk.rb"
cp "${formula}" "${production}"
python3 -I -S scripts/run_release_tool.py release.package_validation compare-files \
  --expected "${formula}" --actual "${production}"

# Audit and install the exact production formula bytes.  The immutable GitHub
# Release does not exist yet, so put the byte-identical source archive at the
# URL-derived cache path documented by Homebrew.
brew style "${production}"
brew audit --strict --formula "${tap}/mcp-cpp-sdk"
python3 -I -S scripts/run_release_tool.py release.package_validation compare-files \
  --expected "${formula}" --actual "${production}"
cache_path="$(brew --cache --build-from-source "${tap}/mcp-cpp-sdk")"
test -n "${cache_path}"
python3 -I -S scripts/run_release_tool.py release.package_validation copy-exact-file \
  --source "${archive}" --destination "${cache_path}"

brew install --build-from-source "${tap}/mcp-cpp-sdk"
brew test "${tap}/mcp-cpp-sdk"
mkdir -p "${work_dir}"
python3 -I -S scripts/run_release_tool.py release.package_validation homebrew-consumer \
  --version "${version}" --prefix "$(brew --prefix "${tap}/mcp-cpp-sdk")" \
  --work "${work_dir}/consumers"
