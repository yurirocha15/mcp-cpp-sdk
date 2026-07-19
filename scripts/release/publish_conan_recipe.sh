#!/usr/bin/env bash
set -euo pipefail

: "${APP_OWNER:?}" "${APP_REPOSITORY:?}" "${EXPECTED_REPOSITORY_ID:?}"
: "${VERSION:?}" "${FORK_BASE:?}" "${GITHUB_OUTPUT:?}" "${GH_TOKEN:?}"

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
test "${fork_base}" = "${FORK_BASE}"

declare -A recipe_files=(
  [recipes/mcp-cpp-sdk/config.yml]=merged-recipe/config.yml
  [recipes/mcp-cpp-sdk/all/conandata.yml]=merged-recipe/all/conandata.yml
  [recipes/mcp-cpp-sdk/all/conanfile.py]=merged-recipe/all/conanfile.py
  [recipes/mcp-cpp-sdk/all/test_package/CMakeLists.txt]=merged-recipe/all/test_package/CMakeLists.txt
  [recipes/mcp-cpp-sdk/all/test_package/conanfile.py]=merged-recipe/all/test_package/conanfile.py
  [recipes/mcp-cpp-sdk/all/test_package/test_package.cpp]=merged-recipe/all/test_package/test_package.cpp
)
for asset in "${recipe_files[@]}"; do test -s "${asset}"; done

branch="package/mcp-cpp-sdk-${VERSION}"
refs="$(gh api "repos/${target}/git/matching-refs/heads/${branch}")"
exact_count="$(jq --arg ref "refs/heads/${branch}" '[.[] | select(.ref == $ref)] | length' <<<"${refs}")"
if [[ "${exact_count}" == 0 ]]; then
  base_tree="$(gh api "repos/${target}/git/commits/${fork_base}" --jq '.tree.sha')"
  tree_entries='[]'
  while IFS= read -r path; do
    asset="${recipe_files[${path}]}"
    content="$(base64 -w0 "${asset}")"
    blob="$(gh api --method POST "repos/${target}/git/blobs" -f content="${content}" -f encoding=base64 --jq .sha)"
    tree_entries="$(jq --arg path "${path}" --arg blob "${blob}" '. + [{path:$path,mode:"100644",type:"blob",sha:$blob}]' <<<"${tree_entries}")"
  done < <(printf '%s\n' "${!recipe_files[@]}" | sort)
  tree="$(jq -n --arg base "${base_tree}" --argjson entries "${tree_entries}" '{base_tree:$base,tree:$entries}' |
    gh api --method POST "repos/${target}/git/trees" --input - --jq .sha)"
  head="$(jq -n --arg message "mcp-cpp-sdk ${VERSION}" --arg tree "${tree}" --arg parent "${fork_base}" \
    '{message:$message,tree:$tree,parents:[$parent]}' |
    gh api --method POST "repos/${target}/git/commits" --input - --jq .sha)"
  gh api --method POST "repos/${target}/git/refs" -f ref="refs/heads/${branch}" -f sha="${head}" >/dev/null
elif [[ "${exact_count}" == 1 ]]; then
  head="$(jq -r --arg ref "refs/heads/${branch}" '.[] | select(.ref == $ref) | .object.sha' <<<"${refs}")"
  while IFS= read -r path; do
    asset="${recipe_files[${path}]}"
    gh api "repos/${target}/contents/${path}?ref=${branch}" --jq .content | tr -d '\n' | base64 --decode >remote-recipe-file
    cmp --silent "${asset}" remote-recipe-file
  done < <(printf '%s\n' "${!recipe_files[@]}" | sort)
else
  echo 'ambiguous Conan recipe branch identity' >&2
  exit 1
fi

commit="$(gh api "repos/${target}/git/commits/${head}")"
test "$(jq '.parents | length' <<<"${commit}")" = 1
test "$(jq -r '.parents[0].sha' <<<"${commit}")" = "${fork_base}"
gh api "repos/${target}/compare/master...${head}" --jq '.files[].filename' | sort >changed-files.txt
printf '%s\n' "${!recipe_files[@]}" | sort >allowed-files.txt
test -z "$(comm -23 changed-files.txt allowed-files.txt)"
grep -Fxq recipes/mcp-cpp-sdk/config.yml changed-files.txt
grep -Fxq recipes/mcp-cpp-sdk/all/conandata.yml changed-files.txt
git init --bare recipe.git
git -C recipe.git fetch --no-tags --depth=1 "https://github.com/${target}.git" "refs/heads/${branch}:refs/heads/recipe"
test "$(git -C recipe.git rev-parse refs/heads/recipe)" = "${head}"
git -C recipe.git archive --format=tar --prefix=recipe/ "refs/heads/recipe:recipes/mcp-cpp-sdk" >recipe.tar
tree_sha256="$(sha256sum recipe.tar | cut -d' ' -f1)"
printf 'branch=%s\nhead_sha=%s\ntree_sha256=%s\n' "${branch}" "${head}" "${tree_sha256}" >>"${GITHUB_OUTPUT}"
