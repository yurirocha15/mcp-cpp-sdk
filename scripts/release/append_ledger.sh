#!/usr/bin/env bash
set -euo pipefail

: "${GH_TOKEN:?}" "${REPOSITORY:?}" "${LEDGER_ISSUE:?}" "${REPOSITORY_OWNER_ID:?}"
: "${LEDGER_BOT_ID:?}" "${LEDGER_BOT_LOGIN:?}" "${LEDGER_BOT_ASSOCIATION:?}"
: "${LEDGER_READINESS_SHA256:?}" "${LEDGER_READINESS_FILE:?}" "${LEDGER_UPDATES_FILE:?}"

gh api "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}" >ledger-issue.before.json
gh api --paginate --slurp \
  "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}/comments?per_page=100" >ledger-comments.before.json
python3 -I -S scripts/run_release_tool.py release.ledger recheck \
  --readiness "${LEDGER_READINESS_FILE}" \
  --expected-readiness-sha256 "${LEDGER_READINESS_SHA256}" \
  --issue ledger-issue.before.json --comments ledger-comments.before.json \
  --issue-number "${LEDGER_ISSUE}" --owner-id "${REPOSITORY_OWNER_ID}" \
  --bot-id "${LEDGER_BOT_ID}" --bot-login "${LEDGER_BOT_LOGIN}" --bot-type Bot \
  --bot-association "${LEDGER_BOT_ASSOCIATION}" --workflow-run-id "${GITHUB_RUN_ID}" \
  --workflow-run-attempt "${GITHUB_RUN_ATTEMPT}"
python3 -I -S scripts/run_release_tool.py release.ledger render-merge \
  --readiness "${LEDGER_READINESS_FILE}" \
  --expected-readiness-sha256 "${LEDGER_READINESS_SHA256}" \
  --updates "${LEDGER_UPDATES_FILE}" --snapshot-output ledger-snapshot.json \
  --comment-output ledger-comment.md
jq -n --rawfile body ledger-comment.md '{body:$body}' | \
  gh api --method POST "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}/comments" --input - \
    >ledger-created-comment.json
gh api "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}" >ledger-issue.after.json
gh api --paginate --slurp \
  "repos/${REPOSITORY}/issues/${LEDGER_ISSUE}/comments?per_page=100" >ledger-comments.after.json
python3 -I -S scripts/run_release_tool.py release.ledger verify-append \
  --readiness "${LEDGER_READINESS_FILE}" \
  --expected-readiness-sha256 "${LEDGER_READINESS_SHA256}" --snapshot ledger-snapshot.json \
  --before-issue ledger-issue.before.json --before-comments ledger-comments.before.json \
  --after-issue ledger-issue.after.json --after-comments ledger-comments.after.json \
  --issue-number "${LEDGER_ISSUE}" --owner-id "${REPOSITORY_OWNER_ID}" \
  --bot-id "${LEDGER_BOT_ID}" --bot-login "${LEDGER_BOT_LOGIN}" --bot-type Bot \
  --bot-association "${LEDGER_BOT_ASSOCIATION}" --workflow-run-id "${GITHUB_RUN_ID}" \
  --workflow-run-attempt "${GITHUB_RUN_ATTEMPT}"
