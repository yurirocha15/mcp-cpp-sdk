#!/usr/bin/env bash
# INFORMATIONAL-ONLY, NON-DEFAULT, NON-CI-GATING.
#
# Side-by-side alpha conformance harness. Runs the official MCP conformance
# runner's alpha channel (@modelcontextprotocol/conformance@0.2.0-alpha.11,
# which defaults to spec 2026-07-28) against the SAME already-built fixtures
# used by conformance/run.sh, but writes to a DISTINCT results directory and
# is never invoked by CI or any phase-exit script.
#
# conformance/run.sh and its pinned 0.1.16 / spec-2025-11-25 runner are the
# SDK's only tier-evidence instrument and are left completely untouched by
# this script: no shared runner install, no shared baseline file, no shared
# results directory, no shared summarizer (summarize.py's scenario catalog
# and "runner=0.1.16" label are specific to the pinned suite and do not
# apply to the alpha scenario set, which already differs materially per
# planning/spec-2026-07-28/RUNNER_NOTES.local.md).
#
# The alpha runner is fetched on demand via `npx` (network required) pinned
# to an exact version -- nothing is installed into conformance/runner, which
# stays reserved for the pinned 0.1.16 install.
#
# This script's exit code reflects whether it *ran to completion*, not
# whether alpha scenarios passed or failed. Conformance results here are
# never tier evidence -- see planning/ALPHA_BASELINE.local.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$PROJECT_DIR/build/release}"
OUTPUT_DIR="${2:-$PROJECT_DIR/build/conformance-results-alpha}"
RUNNER_PACKAGE="@modelcontextprotocol/conformance@0.2.0-alpha.11"
SERVER="$BUILD_DIR/conformance/mcp-conformance-everything-server"
CLIENT="$BUILD_DIR/conformance/mcp-conformance-everything-client"
SPEC_VERSION="2026-07-28"
PORT="${MCP_CONFORMANCE_ALPHA_PORT:-3002}"
SERVER_TIMEOUT="${MCP_CONFORMANCE_SERVER_TIMEOUT:-15m}"
CLIENT_TIMEOUT="${MCP_CONFORMANCE_CLIENT_TIMEOUT:-10m}"

if ! command -v npx >/dev/null 2>&1; then
    echo "npx is not available; the alpha harness fetches the runner on demand and cannot run without it" >&2
    exit 2
fi

echo "Resolving pinned alpha runner: $RUNNER_PACKAGE (network access required)..." >&2
runner_version="$(npx --yes "$RUNNER_PACKAGE" --version)"
if [[ "$runner_version" != "0.2.0-alpha.11" ]]; then
    echo "Unexpected alpha conformance runner version: $runner_version (expected 0.2.0-alpha.11)" >&2
    exit 2
fi
if [[ ! -x "$SERVER" || ! -x "$CLIENT" ]]; then
    echo "Conformance fixtures are missing at $BUILD_DIR/conformance;" \
         "this harness runs against the already-built fixtures and does NOT rebuild them." >&2
    echo "Build them first with: python3 scripts/build.py --conformance" >&2
    exit 2
fi
if [[ -d "$OUTPUT_DIR" ]] && [[ -n "$(find "$OUTPUT_DIR" -mindepth 1 -print -quit)" ]]; then
    echo "Alpha conformance output directory must be empty: $OUTPUT_DIR" >&2
    exit 2
fi

mkdir -p "$OUTPUT_DIR/server" "$OUTPUT_DIR/client"

revision="unknown"
if revision_value="$(git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null)"; then
    revision="$revision_value"
fi
dirty="unknown"
if status_output="$(git -C "$PROJECT_DIR" status --porcelain 2>/dev/null)"; then
    if [[ -n "$status_output" ]]; then
        dirty="true"
    else
        dirty="false"
    fi
fi
{
    echo "# INFORMATIONAL ONLY -- NEVER TIER EVIDENCE -- NEVER CI-GATING"
    echo "runner=@modelcontextprotocol/conformance@$runner_version"
    echo "protocol_version=$SPEC_VERSION"
    echo "source_revision=$revision"
    echo "source_tree_dirty=$dirty"
    echo "started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >"$OUTPUT_DIR/metadata.txt"

server_pid=""
cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"$SERVER" "$PORT" >"$OUTPUT_DIR/server/stdout.txt" 2>"$OUTPUT_DIR/server/stderr.txt" &
server_pid=$!

ready=0
for _ in {1..100}; do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "Conformance server exited before becoming ready" >&2
        exit 1
    fi
    if curl --silent --show-error --connect-timeout 1 --max-time 1 --output /dev/null \
        "http://127.0.0.1:$PORT/mcp"; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
    echo "Conformance server did not become ready" >&2
    exit 1
fi

set +e
timeout --signal=INT --kill-after=10s "$SERVER_TIMEOUT" npx --yes "$RUNNER_PACKAGE" server \
    --url "http://127.0.0.1:$PORT/mcp" \
    --suite active \
    --spec-version "$SPEC_VERSION" \
    --output-dir "$OUTPUT_DIR/server/results" \
    2>&1 | tee "$OUTPUT_DIR/server/runner.log"
server_status=${PIPESTATUS[0]}

timeout --signal=INT --kill-after=10s "$CLIENT_TIMEOUT" npx --yes "$RUNNER_PACKAGE" client \
    --command "exec $CLIENT" \
    --suite core \
    --timeout 30000 \
    --spec-version "$SPEC_VERSION" \
    --output-dir "$OUTPUT_DIR/client/results" \
    2>&1 | tee "$OUTPUT_DIR/client/runner.log"
client_status=${PIPESTATUS[0]}
set -e

{
    echo "server_runner_exit_status=$server_status"
    echo "client_runner_exit_status=$client_status"
    echo "finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >>"$OUTPUT_DIR/metadata.txt"

echo
echo "Alpha conformance run complete (informational only; not tier evidence)." >&2
echo "server-side runner exit: $server_status, client-side runner exit: $client_status" >&2
echo "Results: $OUTPUT_DIR" >&2
echo "Per-scenario pass/fail totals are in $OUTPUT_DIR/server/runner.log and $OUTPUT_DIR/client/runner.log" >&2

# Deliberately always exit 0 here: this harness never gates anything (do-not-do #9).
# A non-zero exit above (missing runner/fixtures/server-not-ready) already returned early.
exit 0
