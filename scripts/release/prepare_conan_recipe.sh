#!/usr/bin/env bash
set -euo pipefail

: "${APP_OWNER:?}" "${APP_REPOSITORY:?}" "${EXPECTED_REPOSITORY_ID:?}"
: "${VERSION:?}" "${GITHUB_OUTPUT:?}" "${GH_TOKEN:?}"

target="${APP_OWNER}/${APP_REPOSITORY}"
[[ "${target}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]
[[ "${EXPECTED_REPOSITORY_ID}" =~ ^[1-9][0-9]*$ ]]
repository="$(gh api "repos/${target}")"
test "$(jq -r '.id|tostring' <<<"${repository}")" = "${EXPECTED_REPOSITORY_ID}"
test "$(jq -r '.full_name' <<<"${repository}")" = "${target}"
test "$(jq -r '.fork,.parent.full_name,.default_branch' <<<"${repository}")" = $'true\nconan-io/conan-center-index\nmaster'
fork_base="$(gh api "repos/${target}/git/ref/heads/master" --jq '.object.sha')"
upstream_base="$(gh api 'repos/conan-io/conan-center-index/git/ref/heads/master' --jq '.object.sha')"
test "${fork_base}" = "${upstream_base}"

rm -rf upstream merged-recipe merge-plan.json upstream-error.txt
mkdir -p upstream

fetch_api_optional() {
  local endpoint="$1" destination="$2" status
  set +e
  gh api "${endpoint}" >"${destination}" 2>upstream-error.txt
  status=$?
  set -e
  if [[ "${status}" == 0 ]]; then
    test -s "${destination}"
    return 0
  fi
  if [[ "${status}" == 1 ]] && grep -Fq 'HTTP 404' upstream-error.txt; then
    : >"${destination}"
    return 1
  fi
  cat upstream-error.txt >&2
  return "${status}"
}

fetch_raw() {
  local path="$1" destination="$2"
  gh api -H 'Accept: application/vnd.github.raw+json' \
    "repos/conan-io/conan-center-index/contents/${path}?ref=${upstream_base}" \
    >"${destination}"
  test -s "${destination}"
}

if fetch_api_optional \
  "repos/conan-io/conan-center-index/contents/recipes/mcp-cpp-sdk?ref=${upstream_base}" \
  upstream/root.json; then
  jq -e '
    type == "array" and length == 2 and
    ([.[] | {name,type}] | sort_by(.name)) ==
      [{"name":"all","type":"dir"},{"name":"config.yml","type":"file"}]
  ' upstream/root.json >/dev/null
  fetch_raw recipes/mcp-cpp-sdk/config.yml upstream/config.yml
  gh api "repos/conan-io/conan-center-index/contents/recipes/mcp-cpp-sdk/all?ref=${upstream_base}" \
    >upstream/all.json
  jq -e '
    type == "array" and length == 3 and
    ([.[] | {name,type}] | sort_by(.name)) ==
      [{"name":"conandata.yml","type":"file"},{"name":"conanfile.py","type":"file"},{"name":"test_package","type":"dir"}]
  ' upstream/all.json >/dev/null
  gh api "repos/conan-io/conan-center-index/contents/recipes/mcp-cpp-sdk/all/test_package?ref=${upstream_base}" \
    >upstream/test-package.json
  jq -e '
    type == "array" and length == 3 and
    ([.[] | {name,type}] | sort_by(.name)) ==
      [{"name":"CMakeLists.txt","type":"file"},{"name":"conanfile.py","type":"file"},{"name":"test_package.cpp","type":"file"}]
  ' upstream/test-package.json >/dev/null
  mkdir -p upstream/all/test_package
  fetch_raw recipes/mcp-cpp-sdk/all/conandata.yml upstream/conandata.yml
  cp upstream/conandata.yml upstream/all/conandata.yml
  fetch_raw recipes/mcp-cpp-sdk/all/conanfile.py upstream/all/conanfile.py
  fetch_raw recipes/mcp-cpp-sdk/all/test_package/CMakeLists.txt upstream/all/test_package/CMakeLists.txt
  fetch_raw recipes/mcp-cpp-sdk/all/test_package/conanfile.py upstream/all/test_package/conanfile.py
  fetch_raw recipes/mcp-cpp-sdk/all/test_package/test_package.cpp upstream/all/test_package/test_package.cpp
  printf '%s\n' '{"schema_version":1,"folders":["all"]}' >upstream/folder-inventory.json
  existing=(--existing-all upstream/all)
else
  : >upstream/config.yml
  : >upstream/conandata.yml
  printf '%s\n' '{"schema_version":1,"folders":[]}' >upstream/folder-inventory.json
  existing=()
fi

python3 -I -S release/conan_center_merge.py \
  --version "${VERSION}" \
  --upstream-config upstream/config.yml \
  --upstream-conandata upstream/conandata.yml \
  --folder-inventory upstream/folder-inventory.json \
  --config-entry release-assets/conan-recipe-config-entry.json \
  --conandata-entry release-assets/conan-recipe-conandata-entry.json \
  --conanfile release-assets/conan-recipe-conanfile.py \
  --test-cmakelists release-assets/conan-recipe-test-CMakeLists.txt \
  --test-conanfile release-assets/conan-recipe-test-conanfile.py \
  --test-source release-assets/conan-recipe-test-test_package.cpp \
  "${existing[@]}" --output-root merged-recipe --result merge-plan.json \
  --github-output "${GITHUB_OUTPUT}"
echo "fork_base=${fork_base}" >>"${GITHUB_OUTPUT}"
