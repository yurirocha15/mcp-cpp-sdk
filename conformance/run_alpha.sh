#!/usr/bin/env bash
# NON-GATING, NEVER TIER EVIDENCE.
#
# Runs the official MCP conformance runner's alpha channel against the same
# already-built fixtures as conformance/run.sh, with the requirement set of
# protocol revision 2026-07-28 (`--requirements 2026-07-28`, which selects the
# scenarios and the wire the revision defines). Results go to a distinct
# directory. CI runs this in a job that cannot fail the workflow, so its
# results are an informational report next to the gating pinned run.
#
# conformance/run.sh and its pinned 0.1.16 / spec-2025-11-25 runner stay the
# SDK's only gate: no shared runner install, no shared baseline file, no shared
# results directory, no shared summarizer (summarize.py's scenario catalog and
# its runner label are specific to the pinned suite).
#
# The alpha runner is fetched on demand via `npx` (network required) pinned to
# an exact version; nothing is installed into conformance/runner, which stays
# reserved for the pinned install.
#
# This script's exit code reflects whether it *ran to completion*, not whether
# alpha scenarios passed or failed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$PROJECT_DIR/build/release}"
OUTPUT_DIR="${2:-$PROJECT_DIR/build/conformance-results-alpha}"
RUNNER_VERSION="0.2.0-alpha.12"
RUNNER_PACKAGE="@modelcontextprotocol/conformance@$RUNNER_VERSION"
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
if [[ "$runner_version" != "$RUNNER_VERSION" ]]; then
    echo "Unexpected alpha conformance runner version: $runner_version (expected $RUNNER_VERSION)" >&2
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
    echo "# NON-GATING -- NEVER TIER EVIDENCE"
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

# The alpha runner's skills scenarios need the fixture to serve skills.
MCP_CONFORMANCE_SKILLS=1 "$SERVER" "$PORT" >"$OUTPUT_DIR/server/stdout.txt" 2>"$OUTPUT_DIR/server/stderr.txt" &
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
    --requirements "$SPEC_VERSION" \
    --output-dir "$OUTPUT_DIR/server/results" \
    2>&1 | tee "$OUTPUT_DIR/server/runner.log"
server_status=${PIPESTATUS[0]}

timeout --signal=INT --kill-after=10s "$CLIENT_TIMEOUT" npx --yes "$RUNNER_PACKAGE" client \
    --command "exec $CLIENT" \
    --timeout 30000 \
    --requirements "$SPEC_VERSION" \
    --output-dir "$OUTPUT_DIR/client/results" \
    2>&1 | tee "$OUTPUT_DIR/client/runner.log"
client_status=${PIPESTATUS[0]}
set -e

# The requirement set of 2026-07-28 names no Skills extension scenario, so run those one by one
# (--force runs an extension scenario at a revision it is not on the timeline of). Their logs stay
# apart from the requirement-set logs above.
SKILLS_SERVER_SCENARIOS=(sep-2640-skills-directory sep-2640-skills-enumeration sep-2640-skills-manifest)
SKILLS_CLIENT_SCENARIOS=(sep-2640-client-no-prefetch sep-2640-client-verify-digest
    sep-2640-client-verify-size sep-2640-client-verify-frontmatter)
set +e
for scenario in "${SKILLS_SERVER_SCENARIOS[@]}"; do
    timeout --signal=INT --kill-after=10s "$SERVER_TIMEOUT" npx --yes "$RUNNER_PACKAGE" server \
        --url "http://127.0.0.1:$PORT/mcp" \
        --scenario "$scenario" --spec-version "$SPEC_VERSION" --force \
        --output-dir "$OUTPUT_DIR/server/skills-results" \
        2>&1 | tee -a "$OUTPUT_DIR/server/skills-runner.log"
done
for scenario in "${SKILLS_CLIENT_SCENARIOS[@]}"; do
    timeout --signal=INT --kill-after=10s "$CLIENT_TIMEOUT" npx --yes "$RUNNER_PACKAGE" client \
        --command "exec $CLIENT" \
        --scenario "$scenario" --spec-version "$SPEC_VERSION" --force --timeout 30000 \
        --output-dir "$OUTPUT_DIR/client/skills-results" \
        2>&1 | tee -a "$OUTPUT_DIR/client/skills-runner.log"
done
# The requirement set reports server-sse-polling without running its checks. The scenario is on
# the timeline of 2025-11-25 only, so it runs on its own at that revision.
timeout --signal=INT --kill-after=10s "$SERVER_TIMEOUT" npx --yes "$RUNNER_PACKAGE" server \
    --url "http://127.0.0.1:$PORT/mcp" \
    --scenario server-sse-polling --spec-version 2025-11-25 --force \
    --output-dir "$OUTPUT_DIR/server/sse-polling-results" \
    2>&1 | tee "$OUTPUT_DIR/server/sse-polling-runner.log"
set -e

{
    echo "server_runner_exit_status=$server_status"
    echo "client_runner_exit_status=$client_status"
    echo "finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >>"$OUTPUT_DIR/metadata.txt"

# The runner prints "Total: N passed, M failed" and the list of scenarios it does not score.
{
    echo "runner=$RUNNER_PACKAGE requirements=$SPEC_VERSION"
    for side in server client; do
        echo "== $side"
        grep -E '^(Total:|Not scored for)' "$OUTPUT_DIR/$side/runner.log" || echo "no totals in the $side runner log"
    done
    # One "Passed: N/M, F failed" line per Skills scenario, in the order they ran.
    for side in server client; do
        echo "== $side, Skills scenarios (not in the requirement set)"
        grep -E '^Passed:' "$OUTPUT_DIR/$side/skills-runner.log" || echo "no totals in the $side Skills log"
    done
    echo "== server, server-sse-polling at 2025-11-25"
    grep -E '^Passed:' "$OUTPUT_DIR/server/sse-polling-runner.log" || echo "no totals in the server-sse-polling log"
} >"$OUTPUT_DIR/summary.txt"
cat "$OUTPUT_DIR/summary.txt"

echo
echo "Alpha conformance run complete (non-gating; not tier evidence)." >&2
echo "server-side runner exit: $server_status, client-side runner exit: $client_status" >&2
echo "Results: $OUTPUT_DIR" >&2
echo "Totals are in $OUTPUT_DIR/summary.txt; per-scenario results are in the runner logs and results directories" >&2

# Deliberately always exit 0 here: this harness never gates anything.
# A non-zero exit above (missing runner/fixtures/server-not-ready) already returned early.
exit 0
