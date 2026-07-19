#!/usr/bin/env bash
set -euo pipefail

: "${GH_TOKEN:?}" "${REPOSITORY:?}" "${REPOSITORY_ID:?}" "${TAG:?}" "${VERSION:?}" "${COMMIT:?}"
: "${LEDGER_ISSUE:?}" "${DISPATCH_MODE:?}" "${RETRY_AUTHORIZED:?}"
: "${EXPECTED_ANCHOR_EXISTS:?}"
: "${REPOSITORY_OWNER_ID:?}" "${LEDGER_BOT_ID:?}"
: "${SELECTED_CHANNELS-}"
: "${LEDGER_BOT_LOGIN:?}" "${LEDGER_BOT_ASSOCIATION:?}" "${GITHUB_OUTPUT:?}"

anchor_exists="$(
  python3 -I -S scripts/run_release_tool.py release.github_publication \
    probe-existing-release --repository "${REPOSITORY}" \
    --repository-id "${REPOSITORY_ID}" --tag "${TAG}"
)"
[[ "${anchor_exists}" == true || "${anchor_exists}" == false ]]
[[ "${anchor_exists}" == "${EXPECTED_ANCHOR_EXISTS}" ]]
gh api "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}" >ledger-issue.json
gh api --paginate --slurp \
  "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}/comments?per_page=100" >ledger-comments.json
python3 -I -S scripts/run_release_tool.py release.ledger readiness \
  --issue ledger-issue.json --comments ledger-comments.json --tag "${TAG}" \
  --version "${VERSION}" --commit "${COMMIT}" --channels "${SELECTED_CHANNELS}" \
  --mode "${DISPATCH_MODE}" --retry-authorized "${RETRY_AUTHORIZED}" \
  --anchor-exists "${anchor_exists}" --issue-number "${LEDGER_ISSUE}" \
  --owner-id "${REPOSITORY_OWNER_ID}" --bot-id "${LEDGER_BOT_ID}" \
  --bot-login "${LEDGER_BOT_LOGIN}" --bot-type Bot \
  --bot-association "${LEDGER_BOT_ASSOCIATION}" --workflow-run-id "${GITHUB_RUN_ID}" \
  --workflow-run-attempt "${GITHUB_RUN_ATTEMPT}" --output ledger-readiness.json \
  --github-output "${GITHUB_OUTPUT}"
echo "anchor_exists=${anchor_exists}" >>"${GITHUB_OUTPUT}"
