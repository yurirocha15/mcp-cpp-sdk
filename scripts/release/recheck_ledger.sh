#!/usr/bin/env bash
set -euo pipefail

: "${GH_TOKEN:?}" "${REPOSITORY:?}" "${LEDGER_ISSUE:?}" "${REPOSITORY_OWNER_ID:?}"
: "${LEDGER_BOT_ID:?}" "${LEDGER_BOT_LOGIN:?}" "${LEDGER_BOT_ASSOCIATION:?}"
: "${LEDGER_READINESS_SHA256:?}" "${LEDGER_READINESS_FILE:?}"

gh api "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}" >ledger-issue.recheck.json
gh api --paginate --slurp \
  "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}/comments?per_page=100" >ledger-comments.recheck.json
python3 -I -S scripts/run_release_tool.py release.ledger recheck \
  --readiness "${LEDGER_READINESS_FILE}" \
  --expected-readiness-sha256 "${LEDGER_READINESS_SHA256}" \
  --issue ledger-issue.recheck.json --comments ledger-comments.recheck.json \
  --issue-number "${LEDGER_ISSUE}" --owner-id "${REPOSITORY_OWNER_ID}" \
  --bot-id "${LEDGER_BOT_ID}" --bot-login "${LEDGER_BOT_LOGIN}" --bot-type Bot \
  --bot-association "${LEDGER_BOT_ASSOCIATION}" --workflow-run-id "${GITHUB_RUN_ID}" \
  --workflow-run-attempt "${GITHUB_RUN_ATTEMPT}"
