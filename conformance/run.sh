#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$PROJECT_DIR/build/release}"
OUTPUT_DIR="${2:-$PROJECT_DIR/build/conformance-results}"
RUNNER="$SCRIPT_DIR/runner/node_modules/.bin/conformance"
SERVER="$BUILD_DIR/conformance/mcp-conformance-everything-server"
CLIENT="$BUILD_DIR/conformance/mcp-conformance-everything-client"
BASELINE="$SCRIPT_DIR/expected-failures.yml"
SPEC_VERSION="2025-11-25"
PORT="${MCP_CONFORMANCE_PORT:-3001}"
SERVER_TIMEOUT="${MCP_CONFORMANCE_SERVER_TIMEOUT:-15m}"
CLIENT_TIMEOUT="${MCP_CONFORMANCE_CLIENT_TIMEOUT:-10m}"

if [[ ! -x "$RUNNER" ]]; then
    echo "Conformance runner is not installed; run: npm ci --prefix conformance/runner" >&2
    exit 2
fi
runner_version="$("$RUNNER" --version)"
if [[ "$runner_version" != "0.1.16" ]]; then
    echo "Unexpected conformance runner version: $runner_version (expected 0.1.16)" >&2
    exit 2
fi
if [[ ! -x "$SERVER" || ! -x "$CLIENT" ]]; then
    echo "Conformance fixtures are missing; run: python scripts/build.py --conformance" >&2
    exit 2
fi
if [[ -d "$OUTPUT_DIR" ]] && [[ -n "$(find "$OUTPUT_DIR" -mindepth 1 -print -quit)" ]]; then
    echo "Conformance output directory must be empty: $OUTPUT_DIR" >&2
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
timeout --signal=INT --kill-after=10s "$SERVER_TIMEOUT" "$RUNNER" server \
    --url "http://127.0.0.1:$PORT/mcp" \
    --suite active \
    --spec-version "$SPEC_VERSION" \
    --expected-failures "$BASELINE" \
    --output-dir "$OUTPUT_DIR/server/results" \
    2>&1 | tee "$OUTPUT_DIR/server/runner.log"
server_status=${PIPESTATUS[0]}

timeout --signal=INT --kill-after=10s "$CLIENT_TIMEOUT" "$RUNNER" client \
    --command "exec $CLIENT" \
    --suite core \
    --timeout 30000 \
    --spec-version "$SPEC_VERSION" \
    --expected-failures "$BASELINE" \
    --output-dir "$OUTPUT_DIR/client/results" \
    2>&1 | tee "$OUTPUT_DIR/client/runner.log"
client_status=${PIPESTATUS[0]}
set -e

python3 "$SCRIPT_DIR/summarize.py" \
    --server-results "$OUTPUT_DIR/server/results" \
    --client-results "$OUTPUT_DIR/client/results" \
    --output-dir "$OUTPUT_DIR" \
    --server-status "$server_status" \
    --client-status "$client_status"

if [[ "$server_status" -ne 0 || "$client_status" -ne 0 ]]; then
    echo "Conformance regression detected (server=$server_status, client=$client_status)" >&2
    exit 1
fi
