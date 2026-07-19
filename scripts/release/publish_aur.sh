#!/usr/bin/env bash
set -euo pipefail

: "${VERSION:?}" "${TAG:?}" "${GITHUB_OUTPUT:?}" "${RUNNER_TEMP:?}"
: "${AUR_PACKAGE_BASE:?}" "${AUR_CLONE_URL:?}"
test "${AUR_PACKAGE_BASE}" = "mcp-cpp-sdk"
test "${AUR_CLONE_URL}" = "ssh://aur@aur.archlinux.org/mcp-cpp-sdk.git"

source scripts/release/aur_ssh_agent.sh
trap aur_ssh_cleanup EXIT
aur_ssh_setup aur-publish
rm -rf aur-repository
git clone --no-checkout "${AUR_CLONE_URL}" aur-repository
if git -C aur-repository rev-parse --verify HEAD >/dev/null 2>&1; then
  git -C aur-repository checkout HEAD -- .
  git -C aur-repository ls-tree -r --name-only HEAD >existing-tree-files
  existing_tree=(--tree-inventory existing-tree-files)
else
  test -z "$(find aur-repository -mindepth 1 -maxdepth 1 ! -name .git -print -quit)"
  existing_tree=()
fi
python3 -I -S scripts/run_release_tool.py release.aur_publish \
  --existing aur-repository/PKGBUILD --target "${VERSION}" "${existing_tree[@]}"
rm -f existing-tree-files

verify_remote_state() {
  local expected_head="$1" remote_line remote_head remote_tree
  remote_line="$(git -C aur-repository ls-remote --refs origin refs/heads/master)"
  test "$(printf '%s\n' "${remote_line}" | sed '/^$/d' | wc -l)" -eq 1
  remote_head="${remote_line%%[[:space:]]*}"
  test "${remote_head}" = "${expected_head}"
  git -C aur-repository fetch --no-tags --force origin \
    "${remote_head}:refs/remotes/origin/release-verify"
  test "$(git -C aur-repository rev-parse refs/remotes/origin/release-verify)" = "${expected_head}"
  remote_tree="$(git -C aur-repository rev-parse refs/remotes/origin/release-verify^{tree})"
  test "${remote_tree}" = "$(git -C aur-repository rev-parse "${expected_head}^{tree}")"
  git -C aur-repository ls-tree -r --name-only refs/remotes/origin/release-verify \
    >remote-tree-files
  git -C aur-repository show refs/remotes/origin/release-verify:PKGBUILD >remote-PKGBUILD
  git -C aur-repository show refs/remotes/origin/release-verify:.SRCINFO >remote-SRCINFO
  python3 -I -S scripts/run_release_tool.py release.aur_publish \
    --existing remote-PKGBUILD --target "${VERSION}" --tree-inventory remote-tree-files
  cmp --silent release-assets/aur-PKGBUILD remote-PKGBUILD
  cmp --silent release-assets/aur-SRCINFO remote-SRCINFO
  rm -f remote-PKGBUILD remote-SRCINFO remote-tree-files
}

if [[ -f aur-repository/PKGBUILD ]] && grep -Fx "pkgver=${VERSION}" aur-repository/PKGBUILD >/dev/null; then
  if cmp -s aur-repository/PKGBUILD release-assets/aur-PKGBUILD && \
    cmp -s aur-repository/.SRCINFO release-assets/aur-SRCINFO; then
    verify_remote_state "$(git -C aur-repository rev-parse HEAD)"
    echo 'result=SKIPPED_ALREADY_IDENTICAL' >>"${GITHUB_OUTPUT}"
    exit 0
  fi
  echo 'AUR already contains this version with different metadata' >&2
  exit 1
fi

install -m 644 release-assets/aur-PKGBUILD aur-repository/PKGBUILD
install -m 644 release-assets/aur-SRCINFO aur-repository/.SRCINFO
git -C aur-repository config user.name mcp-cpp-sdk-release
git -C aur-repository config user.email releases@yurirocha.com
git -C aur-repository add -- PKGBUILD .SRCINFO
git -C aur-repository commit -m "mcp-cpp-sdk ${TAG}"
head="$(git -C aur-repository rev-parse HEAD)"
git -C aur-repository push origin HEAD:master
verify_remote_state "${head}"
echo 'result=PUBLISHED' >>"${GITHUB_OUTPUT}"
