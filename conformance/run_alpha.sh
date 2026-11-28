#!/usr/bin/env bash
# NEVER TIER EVIDENCE.
#
# Runs the alpha channel of the official MCP conformance runner against the same already-built
# fixtures as conformance/run.sh, once per requirement set (`--requirements 2026-07-28` and
# `--requirements 2025-11-25`). Each set gets a fresh fixture server process and its own results
# directory under the output directory. The runner's `--expected-failures` baseline,
# conformance/expected-failures-alpha.yml, lists the scenarios that are expected to fail; the
# runner fails on a new failure and on a stale entry.
#
# The alpha runner is installed from conformance/runner-alpha/package-lock.json
# (`npm ci --ignore-scripts --prefix conformance/runner-alpha`). conformance/runner stays reserved
# for the pinned runner that conformance/run.sh gates on: no shared install, baseline, results
# directory or summarizer.
#
# The exit status is 0 when every requirement-set run exits 0 and 2 when the harness cannot start
# (missing runner or fixtures, busy output directory); any other value is the highest non-zero
# runner status. The workflow job that calls this script reports the result without gating on it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$PROJECT_DIR/build/release}"
OUTPUT_DIR="${2:-$PROJECT_DIR/build/conformance-results-alpha}"
RUNNER="$SCRIPT_DIR/runner-alpha/node_modules/.bin/conformance"
SERVER="$BUILD_DIR/conformance/mcp-conformance-everything-server"
CLIENT="$BUILD_DIR/conformance/mcp-conformance-everything-client"
BASELINE="$SCRIPT_DIR/expected-failures-alpha.yml"
REQUIREMENT_SETS=(2026-07-28 2025-11-25)
PORT="${MCP_CONFORMANCE_ALPHA_PORT:-3002}"
SERVER_TIMEOUT="${MCP_CONFORMANCE_SERVER_TIMEOUT:-15m}"
CLIENT_TIMEOUT="${MCP_CONFORMANCE_CLIENT_TIMEOUT:-10m}"

if [[ ! -x "$RUNNER" ]]; then
    echo "Alpha conformance runner is not installed; run: npm ci --ignore-scripts --prefix conformance/runner-alpha" >&2
    exit 2
fi
runner_version="$("$RUNNER" --version)"
expected_version="$(python3 -c '
import json, sys
print(json.load(open(sys.argv[1]))["dependencies"]["@modelcontextprotocol/conformance"])
' "$SCRIPT_DIR/runner-alpha/package.json")"
if [[ "$runner_version" != "$expected_version" ]]; then
    echo "Unexpected alpha conformance runner version: $runner_version (expected $expected_version)" >&2
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

mkdir -p "$OUTPUT_DIR"

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
    echo "# NEVER TIER EVIDENCE"
    echo "runner=@modelcontextprotocol/conformance@$runner_version"
    echo "requirement_sets=${REQUIREMENT_SETS[*]}"
    echo "source_revision=$revision"
    echo "source_tree_dirty=$dirty"
    echo "started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >"$OUTPUT_DIR/metadata.txt"

server_pid=""
stop_server() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    server_pid=""
}
trap stop_server EXIT

# Starts a fresh fixture server and waits until it answers. The Skills fixtures are served only
# when the second argument is non-empty, the switch conformance/everything_server.cpp reads.
start_server() {
    local set_dir="$1" skills="$2"
    MCP_CONFORMANCE_SKILLS="$skills" "$SERVER" "$PORT" \
        >"$set_dir/server/stdout.txt" 2>"$set_dir/server/stderr.txt" &
    server_pid=$!
    for _ in {1..100}; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "Conformance server exited before becoming ready" >&2
            return 1
        fi
        if curl --silent --show-error --connect-timeout 1 --max-time 1 --output /dev/null \
            "http://127.0.0.1:$PORT/mcp"; then
            return 0
        fi
        sleep 0.1
    done
    echo "Conformance server did not become ready" >&2
    return 1
}

overall_status=0
note_status() {
    if [[ "$1" -gt "$overall_status" ]]; then
        overall_status="$1"
    fi
}

for set in "${REQUIREMENT_SETS[@]}"; do
    set_dir="$OUTPUT_DIR/$set"
    mkdir -p "$set_dir/server" "$set_dir/client"
    skills=""
    if [[ "$set" == "2026-07-28" ]]; then
        skills=1
    fi
    start_server "$set_dir" "$skills" || exit 1

    set +e
    timeout --signal=INT --kill-after=10s "$SERVER_TIMEOUT" "$RUNNER" server \
        --url "http://127.0.0.1:$PORT/mcp" \
        --requirements "$set" \
        --expected-failures "$BASELINE" \
        --output-dir "$set_dir/server/results" \
        2>&1 | tee "$set_dir/server/runner.log"
    server_status=${PIPESTATUS[0]}

    timeout --signal=INT --kill-after=10s "$CLIENT_TIMEOUT" "$RUNNER" client \
        --command "exec $CLIENT" \
        --timeout 30000 \
        --requirements "$set" \
        --expected-failures "$BASELINE" \
        --output-dir "$set_dir/client/results" \
        2>&1 | tee "$set_dir/client/runner.log"
    client_status=${PIPESTATUS[0]}
    set -e
    note_status "$server_status"
    note_status "$client_status"

    {
        echo "requirements=$set"
        echo "server_runner_exit_status=$server_status"
        echo "client_runner_exit_status=$client_status"
    } >"$set_dir/status.txt"

    # Scenarios outside the requirement set run one by one (--force runs an extension scenario
    # at a revision it is not on the timeline of). Their logs stay apart from the
    # requirement-set logs, and their statuses do not count towards the result.
    set +e
    if [[ "$set" == "2026-07-28" ]]; then
        skills_server_scenarios=(sep-2640-skills-directory sep-2640-skills-enumeration
            sep-2640-skills-manifest)
        skills_client_scenarios=(sep-2640-client-no-prefetch sep-2640-client-verify-digest
            sep-2640-client-verify-size sep-2640-client-verify-frontmatter)
        for scenario in "${skills_server_scenarios[@]}"; do
            timeout --signal=INT --kill-after=10s "$SERVER_TIMEOUT" "$RUNNER" server \
                --url "http://127.0.0.1:$PORT/mcp" \
                --scenario "$scenario" --spec-version "$set" --force \
                --output-dir "$set_dir/server/skills-results" \
                2>&1 | tee -a "$set_dir/server/skills-runner.log"
        done
        for scenario in "${skills_client_scenarios[@]}"; do
            timeout --signal=INT --kill-after=10s "$CLIENT_TIMEOUT" "$RUNNER" client \
                --command "exec $CLIENT" \
                --scenario "$scenario" --spec-version "$set" --force --timeout 30000 \
                --output-dir "$set_dir/client/skills-results" \
                2>&1 | tee -a "$set_dir/client/skills-runner.log"
        done
    else
        # The requirement set reports server-sse-polling without running its checks, so the
        # scenario runs on its own.
        timeout --signal=INT --kill-after=10s "$SERVER_TIMEOUT" "$RUNNER" server \
            --url "http://127.0.0.1:$PORT/mcp" \
            --scenario server-sse-polling --spec-version "$set" --force \
            --output-dir "$set_dir/server/sse-polling-results" \
            2>&1 | tee "$set_dir/server/sse-polling-runner.log"
    fi
    set -e
    stop_server
done

{
    echo "result_exit_status=$overall_status"
    echo "finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >>"$OUTPUT_DIR/metadata.txt"

# The runner prints "Total: N passed, M failed" and the list of scenarios it does not score.
{
    echo "runner=@modelcontextprotocol/conformance@$runner_version"
    for set in "${REQUIREMENT_SETS[@]}"; do
        echo "== requirements $set"
        cat "$OUTPUT_DIR/$set/status.txt"
        for side in server client; do
            echo "-- $side"
            grep -E '^(Total:|Not scored for)' "$OUTPUT_DIR/$set/$side/runner.log" ||
                echo "no totals in the $side runner log"
        done
    done
    # One "Passed: N/M, F failed" line per scenario, in the order they ran.
    for side in server client; do
        echo "== $side, Skills scenarios at 2026-07-28 (not in the requirement set)"
        grep -E '^Passed:' "$OUTPUT_DIR/2026-07-28/$side/skills-runner.log" ||
            echo "no totals in the $side Skills log"
    done
    echo "== server, server-sse-polling at 2025-11-25"
    grep -E '^Passed:' "$OUTPUT_DIR/2025-11-25/server/sse-polling-runner.log" ||
        echo "no totals in the server-sse-polling log"
} >"$OUTPUT_DIR/summary.txt"
cat "$OUTPUT_DIR/summary.txt"

echo
echo "Alpha conformance run complete (never tier evidence)." >&2
echo "Result (highest runner exit status of the requirement-set runs): $overall_status" >&2
echo "Results: $OUTPUT_DIR" >&2
echo "Totals are in $OUTPUT_DIR/summary.txt; per-scenario results are in the runner logs and results directories" >&2

exit "$overall_status"
