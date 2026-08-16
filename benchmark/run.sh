#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"
RUNTIME_COMPOSE_FILE="$COMPOSE_FILE"
UPSTREAM_BENCHMARK_DIR="$SCRIPT_DIR/benchmark-mcp-servers-v2"
UPSTREAM_BENCHMARK_REPO="https://github.com/thiagomendes/benchmark-mcp-servers-v2.git"
UPSTREAM_BENCHMARK_COMMIT="8a9a5f8ef505f46b6079072ef4603304ca672e33"
K6_SCRIPT="$SCRIPT_DIR/alternatives/benchmark.js"
K6_RUNTIME_SCRIPT_DIR="$SCRIPT_DIR/alternatives"
RUNTIME_COMPOSE_SHA256=""
RUNTIME_K6_SHA256=""
ALTERNATIVE_MANIFEST="$SCRIPT_DIR/alternatives/sources.tsv"
ALTERNATIVE_SOURCE_DIR="$SCRIPT_DIR/alternative-sdks"
MCP_PROTOCOL_VERSION="2024-11-05"
ELIGIBILITY_CONTRACT="upstream-v2-strict-mcp-v1"
SUPPLEMENTAL_CONTRACT="adapter-exact-v1"
HOST_NETWORK_POLICY="baseline-cohort-retire-v1"
K6_IMAGE="grafana/k6@sha256:82e44a45a38ed22bf5636fe50fe8a07967c3074f7aa66567c6a7501ab9bb3a9f"

BENCHMARK_RUNS="${BENCHMARK_RUNS:-3}"
BENCHMARK_VUS="${BENCHMARK_VUS:-50}"
BENCHMARK_RAMP_DURATION="${BENCHMARK_RAMP_DURATION:-15s}"
BENCHMARK_WARMUP_DURATION="${BENCHMARK_WARMUP_DURATION:-60s}"
BENCHMARK_MEASURE_DURATION="${BENCHMARK_MEASURE_DURATION:-5m}"
BENCHMARK_ORDER_SEED="${BENCHMARK_ORDER_SEED:-$(date +%s)}"
# The load-generator contract is fixed so a run cannot be labeled publishable
# while silently using limits that differ from the recorded expectations.
readonly BENCHMARK_K6_CPUS=4
readonly BENCHMARK_K6_MEMORY="2g"
readonly BENCHMARK_K6_MEMORY_BYTES=2147483648
readonly BENCHMARK_REQUEST_CONCURRENCY=50
readonly BENCHMARK_LOCK_FILE="/tmp/mcp-cpp-sdk-benchmark.lock"
RESULTS_DIR=""
RUN_SUCCEEDED=0
BENCHMARK_ACTIVE=0
CURRENT_SERVER=""
CURRENT_SERVER_RESULTS=""
CURRENT_K6_CONTAINER=""
HOST_CPU_LIMIT=""
HOST_MEMORY_BYTES=""
HOST_CPUSET=""
FAILURE_STAGE=""
FAILURE_REASON=""
FAILURE_EVIDENCE_LOG=""

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { printf "%b[INFO]%b %s\n" "$BLUE" "$NC" "$*"; }
ok() { printf "%b[OK]%b %s\n" "$GREEN" "$NC" "$*"; }
error() { printf "%b[ERROR]%b %s\n" "$RED" "$NC" "$*" >&2; }

acquire_benchmark_lock() {
    command -v flock >/dev/null || {
        error "flock not found; cannot protect the fixed benchmark ports and containers"
        return 1
    }

    exec 9>> "$BENCHMARK_LOCK_FILE"
    if ! flock -n 9; then
        local holder=""
        holder="$(head -n 1 "$BENCHMARK_LOCK_FILE" 2>/dev/null || true)"
        error "Another benchmark invocation is active (PID ${holder:-unknown})"
        return 1
    fi
    printf '%s\n' "$$" > "$BENCHMARK_LOCK_FILE"
}

declare -A SERVICE_NAME=(
    [cpp]="cpp-server"
    [ours-comparable]="ours-comparable-server"
    [python]="python-server"
    [go]="go-server"
    [rust]="rust-server"
    [hkr04]="hkr04-server"
    [fastmcpp]="fastmcpp-server"
    [cxxmcp]="cxxmcp-server"
    [neumann]="neumann-server"
)

declare -A CONTAINER_NAME=(
    [cpp]="mcp-cpp-server"
    [ours-comparable]="mcp-ours-comparable-server"
    [python]="mcp-python-server"
    [go]="mcp-go-server"
    [rust]="mcp-rust-server"
    [hkr04]="mcp-hkr04-server"
    [fastmcpp]="mcp-fastmcpp-server"
    [cxxmcp]="mcp-cxxmcp-server"
    [neumann]="mcp-neumann-server"
)

declare -A MCP_URL=(
    [cpp]="http://localhost:8080/mcp"
    [ours-comparable]="http://localhost:8089/mcp"
    [python]="http://localhost:8081/mcp"
    [go]="http://localhost:8082/mcp"
    [rust]="http://localhost:8083/mcp"
    [hkr04]="http://localhost:8084/mcp"
    [fastmcpp]="http://localhost:8085/mcp"
    [cxxmcp]="http://localhost:8086/mcp"
    [neumann]="http://localhost:8087/mcp"
)

declare -A HEALTH_URL=(
    [cpp]="http://localhost:8080/health"
    [ours-comparable]="http://localhost:8089/mcp"
    [python]="http://localhost:8081/health"
    [go]="http://localhost:8082/health"
    [rust]="http://localhost:8083/health"
    [hkr04]="http://localhost:8084/mcp"
    [fastmcpp]="http://localhost:8085/mcp"
    [cxxmcp]="http://localhost:8086/mcp"
    [neumann]="http://localhost:8087/mcp"
)

declare -A HEALTH_KIND=(
    [cpp]="get"
    [ours-comparable]="mcp"
    [python]="get"
    [go]="get"
    [rust]="get"
    [hkr04]="mcp"
    [fastmcpp]="mcp"
    [cxxmcp]="mcp"
    [neumann]="mcp"
)

declare -A EXPECTED_SERVER_TYPE=(
    [cpp]="cpp"
    [ours-comparable]="cpp-sdk"
    [hkr04]="cpp-sdk"
    [fastmcpp]="cpp-sdk"
    [cxxmcp]="cpp-sdk"
    [neumann]="cpp-sdk"
    [python]="python"
    [go]="go"
    [rust]="rust"
)

ALL_MCP_SERVICES=(
    cpp-server ours-comparable-server python-server go-server rust-server
    hkr04-server fastmcpp-server cxxmcp-server neumann-server
)
CPP_SDK_SERVERS=(ours-comparable hkr04 fastmcpp cxxmcp neumann)
BASELINE_SERVERS=(python go rust)

declare -A REQUIRE_SUPPLEMENTAL=(
    [ours-comparable]=1
    [hkr04]=1
    [fastmcpp]=1
    [cxxmcp]=1
    [neumann]=1
)

declare -A ALTERNATIVE_REPO=()
declare -A ALTERNATIVE_COMMIT=()
declare -A ALTERNATIVE_STATUS=()
collector_pids=()
collector_ready_files=()

usage() {
    cat <<'USAGE'
Usage:
  ./run.sh                  # five comparable C++ SDKs
  ./run.sh cpp-sdks         # five comparable C++ SDKs
  ./run.sh baseline         # Python, Go, and Rust baselines
  ./run.sh all              # all corrected C++ and language baselines
  ./run.sh python go rust   # selected servers
  ./run.sh hkr04,cxxmcp     # selected servers (comma-separated)

Production profile: 3 rounds, 50 VUs, 15s ramp + 60s warmup, then a
separate constant-50-VU 5m measurement. Any override is labeled smoke and
is never a publishable result.
USAGE
}

verify_runtime_harness() {
    [[ -n "$RUNTIME_COMPOSE_SHA256" && -n "$RUNTIME_K6_SHA256" ]] || return 0
    local compose_sha256 k6_sha256
    compose_sha256="$(sha256sum "$RUNTIME_COMPOSE_FILE" | awk '{print $1}')"
    k6_sha256="$(sha256sum "$K6_RUNTIME_SCRIPT_DIR/benchmark.js" | awk '{print $1}')"
    [[ "$compose_sha256" == "$RUNTIME_COMPOSE_SHA256" \
        && "$k6_sha256" == "$RUNTIME_K6_SHA256" ]] || {
        error "Immutable runtime harness changed during the benchmark"
        return 1
    }
}

compose() {
    verify_runtime_harness
    docker compose --project-directory "$SCRIPT_DIR" -f "$RUNTIME_COMPOSE_FILE" "$@"
}

stop_collectors() {
    local pid
    local failed=0
    for pid in "${collector_pids[@]:-}"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${collector_pids[@]:-}"; do
        if [[ -n "$pid" ]]; then
            wait "$pid" 2>/dev/null || failed=1
        fi
    done
    collector_pids=()
    collector_ready_files=()
    return "$failed"
}

finalize_manifest() {
    local exit_code="$1"
    [[ -n "$RESULTS_DIR" && -f "$RESULTS_DIR/run_manifest.json" ]] || return 0
    local status="failed"
    if [[ "$exit_code" -eq 0 && "$RUN_SUCCEEDED" -eq 1 ]]; then
        status="complete"
    fi
    local temporary="$RESULTS_DIR/run_manifest.json.tmp"
    jq --arg status "$status" --arg completed_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --arg failure_stage "$FAILURE_STAGE" \
        --arg failure_reason "$FAILURE_REASON" \
        --arg failure_log "$FAILURE_EVIDENCE_LOG" \
        --argjson exit_code "$exit_code" \
        '.status = $status | .completed_at = $completed_at
        | .publishable_candidate = ($status == "complete" and .profile == "production")
        | if $status == "failed" then
            .failure = {
              exit_code: $exit_code,
              stage: (if $failure_stage == "" then "unknown" else $failure_stage end),
              reason: (if $failure_reason == "" then null else $failure_reason end),
              evidence_log: (if $failure_log == "" then null else $failure_log end)
            }
          else del(.failure)
          end' \
        "$RESULTS_DIR/run_manifest.json" > "$temporary" \
        && mv "$temporary" "$RESULTS_DIR/run_manifest.json"
}

clear_failure_context() {
    FAILURE_STAGE=""
    FAILURE_REASON=""
    FAILURE_EVIDENCE_LOG=""
}

record_failure_from_log() {
    local stage="$1"
    local log_file="$2"
    local fallback_reason="$3"
    local final_line=""

    FAILURE_STAGE="$stage"
    FAILURE_EVIDENCE_LOG="${log_file#"$RESULTS_DIR"/}"
    if [[ -f "$log_file" ]]; then
        final_line="$(awk 'NF { line = $0 } END { print line }' "$log_file")"
    fi
    FAILURE_REASON="${final_line:-$fallback_reason}"
}

record_collector_failure() {
    local stage="$1"
    local log_file="$2"
    local fallback_reason="$3"
    shift 3
    local structured_reason=""

    FAILURE_STAGE="$stage"
    FAILURE_EVIDENCE_LOG="${log_file#"$RESULTS_DIR"/}"
    if structured_reason="$(
        python3 "$SCRIPT_DIR/summarize_collector_failures.py" "$@" 2>/dev/null
    )" && [[ -n "$structured_reason" ]]; then
        FAILURE_REASON="$structured_reason"
        return
    fi
    record_failure_from_log "$stage" "$log_file" "$fallback_reason"
}

record_resource_collection_failure() {
    local stage="$1"
    local server_results="$2"
    local run_idx="$3"
    local fallback_reason="$4"
    local collector_log="$server_results/resource_collection_run${run_idx}.log"
    local collector_file

    {
        for collector_file in \
            "$server_results/stats_run${run_idx}.log" \
            "$server_results/redis_stats_run${run_idx}.log" \
            "$server_results/api_stats_run${run_idx}.log" \
            "$server_results/k6_stats_run${run_idx}.log" \
            "$server_results/host_stats_run${run_idx}.log"; do
            if [[ -s "$collector_file" ]]; then
                printf '[%s]\n' "$(basename "$collector_file")"
                tail -n 20 "$collector_file"
            fi
        done
    } > "$collector_log"

    record_collector_failure "$stage" "$collector_log" "$fallback_reason" \
        "$server_results/stats_run${run_idx}.audit.json" \
        "$server_results/redis_stats_run${run_idx}.audit.json" \
        "$server_results/api_stats_run${run_idx}.audit.json" \
        "$server_results/k6_stats_run${run_idx}.audit.json" \
        "$server_results/host_stats_run${run_idx}.audit.json"
}

run_protocol_verifier() {
    local stage="$1"
    local log_file="$2"
    shift 2
    local -a pipeline_status
    local verifier_exit tee_exit

    set +e
    python3 "$SCRIPT_DIR/verify_server.py" "$@" 2>&1 | tee "$log_file"
    pipeline_status=("${PIPESTATUS[@]}")
    set -e
    verifier_exit="${pipeline_status[0]}"
    tee_exit="${pipeline_status[1]}"
    if [[ "$verifier_exit" -eq 0 && "$tee_exit" -eq 0 ]]; then
        return 0
    fi

    FAILURE_STAGE="$stage"
    FAILURE_EVIDENCE_LOG="${log_file#"$RESULTS_DIR"/}"
    FAILURE_REASON="$(tail -n 1 "$log_file" 2>/dev/null || true)"
    if [[ "$verifier_exit" -ne 0 ]]; then
        return "$verifier_exit"
    fi
    return "$tee_exit"
}

cleanup_on_exit() {
    local exit_code=$?
    trap - EXIT
    if [[ "$BENCHMARK_ACTIVE" -eq 1 ]]; then
        stop_collectors || true
        if [[ -n "$CURRENT_K6_CONTAINER" ]]; then
            docker rm -f "$CURRENT_K6_CONTAINER" >/dev/null 2>&1 || true
        fi
        if [[ -n "$CURRENT_SERVER" && -n "$CURRENT_SERVER_RESULTS" ]]; then
            docker logs "$CURRENT_SERVER" > "$CURRENT_SERVER_RESULTS/server_failure.log" 2>&1 || true
        fi
        compose stop "${ALL_MCP_SERVICES[@]}" >/dev/null 2>&1 || true
        compose stop redis api-service >/dev/null 2>&1 || true
    fi
    if ! finalize_manifest "$exit_code"; then
        error "Failed to finalize benchmark run manifest"
        if [[ "$exit_code" -eq 0 ]]; then
            exit_code=1
        fi
    fi
    exit "$exit_code"
}

trap cleanup_on_exit EXIT

load_alternative_manifest() {
    [[ -f "$ALTERNATIVE_MANIFEST" ]] || {
        error "Missing alternative source manifest: $ALTERNATIVE_MANIFEST"
        return 1
    }
    while IFS=$'\t' read -r id repository commit status; do
        [[ -z "$id" || "$id" == \#* ]] && continue
        ALTERNATIVE_REPO["$id"]="$repository"
        ALTERNATIVE_COMMIT["$id"]="$commit"
        ALTERNATIVE_STATUS["$id"]="$status"
    done < "$ALTERNATIVE_MANIFEST"
}

ensure_git_checkout() {
    local name="$1"
    local repository="$2"
    local commit="$3"
    local target="$4"
    local fresh_clone=0

    if [[ ! -d "$target/.git" ]]; then
        [[ ! -e "$target" ]] || {
            error "Clone target exists but is not a git repository: $target"
            return 1
        }
        info "Cloning $name from $repository"
        git clone --filter=blob:none --no-checkout "$repository" "$target"
        fresh_clone=1
    fi
    local actual_origin
    actual_origin="$(git -C "$target" remote get-url origin)"
    [[ "$actual_origin" == "$repository" ]] || {
        error "$name origin mismatch: expected $repository, found $actual_origin"
        return 1
    }
    if [[ "$fresh_clone" -eq 0 && -n "$(git -C "$target" status --porcelain)" ]]; then
        error "$name source has local modifications: $target"
        return 1
    fi
    if [[ "$fresh_clone" -eq 1 || "$(git -C "$target" rev-parse HEAD)" != "$commit" ]]; then
        git -C "$target" fetch --depth 1 origin "$commit"
        git -C "$target" checkout --detach "$commit"
    fi
    [[ "$(git -C "$target" rev-parse HEAD)" == "$commit" ]] || {
        error "Failed to pin $name to $commit"
        return 1
    }
    [[ -z "$(git -C "$target" status --porcelain)" ]] || {
        error "$name source is not clean after pinning"
        return 1
    }
    ok "Using $name pinned at $commit"
}

ensure_upstream_sources() {
    ensure_git_checkout \
        "upstream benchmark" "$UPSTREAM_BENCHMARK_REPO" \
        "$UPSTREAM_BENCHMARK_COMMIT" "$UPSTREAM_BENCHMARK_DIR"
    local required_paths=(api-service infra/redis python-server go-server rust-server)
    local relative
    for relative in "${required_paths[@]}"; do
        [[ -d "$UPSTREAM_BENCHMARK_DIR/$relative" ]] || {
            error "Missing upstream path: $UPSTREAM_BENCHMARK_DIR/$relative"
            return 1
        }
    done
}

ensure_alternative_source() {
    local id="$1"
    mkdir -p "$ALTERNATIVE_SOURCE_DIR"
    ensure_git_checkout \
        "$id" "${ALTERNATIVE_REPO[$id]}" "${ALTERNATIVE_COMMIT[$id]}" \
        "$ALTERNATIVE_SOURCE_DIR/$id"
}

wait_for_http() {
    local name="$1"
    local url="$2"
    local timeout_secs="$3"
    local start_ts
    start_ts="$(date +%s)"
    info "Waiting for $name at $url"
    until curl -fsS "$url" >/dev/null 2>&1; do
        if (( $(date +%s) - start_ts >= timeout_secs )); then
            error "Timeout waiting for $name at $url"
            return 1
        fi
        sleep 1
    done
    ok "$name is healthy"
}

wait_for_mcp() {
    local name="$1"
    local url="$2"
    local timeout_secs="$3"
    local start_ts
    start_ts="$(date +%s)"
    info "Waiting for $name MCP initialize at $url"
    while true; do
        if python3 "$SCRIPT_DIR/verify_server.py" "$url" \
            --name "$name-readiness" --initialize-only >/dev/null 2>&1; then
            ok "$name is accepting MCP requests"
            return 0
        fi
        if (( $(date +%s) - start_ts >= timeout_secs )); then
            error "Timeout waiting for $name at $url"
            return 1
        fi
        sleep 1
    done
}

reset_redis_dataset() {
    compose exec -T redis redis-cli FLUSHDB >/dev/null
    docker rm -f mcp-redis-seeder >/dev/null 2>&1 || true
    compose --profile seeder run --rm redis-seeder >/dev/null
}

start_collector() {
    local container="$1"
    local output="$2"
    python3 "$SCRIPT_DIR/collect_stats.py" "$container" "$output" 1.0 \
        > "${output%.json}.log" 2>&1 &
    collector_pids+=("$!")
    collector_ready_files+=("${output%.json}.ready.json")
}

wait_for_collectors_ready() {
    local timeout_secs="${1:-15}"
    local started_at
    local index
    local all_ready
    started_at="$(date +%s)"
    while true; do
        all_ready=1
        for index in "${!collector_pids[@]}"; do
            if ! kill -0 "${collector_pids[$index]}" 2>/dev/null; then
                error "Resource collector exited before readiness: ${collector_ready_files[$index]}"
                return 1
            fi
            if ! jq -e '.schema_version == 1 and .status == "ready"' \
                "${collector_ready_files[$index]}" >/dev/null 2>&1; then
                all_ready=0
            fi
        done
        if [[ "$all_ready" -eq 1 ]]; then
            # Close the small ready-marker/process-exit race before measurement.
            for index in "${!collector_pids[@]}"; do
                kill -0 "${collector_pids[$index]}" 2>/dev/null || {
                    error "Resource collector exited at readiness: ${collector_ready_files[$index]}"
                    return 1
                }
            done
            return 0
        fi
        if (( $(date +%s) - started_at >= timeout_secs )); then
            error "Timed out waiting for resource collectors to capture baselines"
            return 1
        fi
        sleep 0.1
    done
}

wait_for_container() {
    local container="$1"
    local attempt
    for attempt in $(seq 1 100); do
        if docker inspect "$container" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    error "Container did not start: $container"
    return 1
}

resume_k6() {
    local server="$1"
    local run_idx="$2"
    local attempt
    local payload='{"data":{"type":"status","id":"default","attributes":{"paused":false}}}'
    for attempt in $(seq 1 100); do
        if curl -fsS -X PATCH "http://127.0.0.1:6565/v1/status" \
            -H "Content-Type: application/json" \
            --data "$payload" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    error "[$server] could not resume paused k6 run $run_idx"
    return 1
}

run_k6_warmup() {
    local server="$1"
    local server_url="$2"
    local server_results="$3"
    local run_idx="$4"
    local negotiated_protocol_version="$5"
    verify_runtime_harness
    local container="mcp-k6-warmup-${server//[^a-zA-Z0-9]/-}-${run_idx}"
    local log_file="$server_results/warmup_console_run${run_idx}.log"
    local -a pipeline_status
    local k6_exit tee_exit

    FAILURE_STAGE="warmup:$server:run$run_idx"
    FAILURE_REASON="k6 warmup setup or execution failed"
    FAILURE_EVIDENCE_LOG="${log_file#"$RESULTS_DIR"/}"
    CURRENT_K6_CONTAINER="$container"
    set +e
    docker run --rm --name "$container" \
        --network host --cpus "$BENCHMARK_K6_CPUS" --memory "$BENCHMARK_K6_MEMORY" \
        --cpuset-cpus "$HOST_CPUSET" \
        --user "$(id -u):$(id -g)" \
        -v "$K6_RUNTIME_SCRIPT_DIR:/scripts:ro" \
        -v "$server_results:/results" \
        -e SERVER_URL="$server_url" \
        -e SERVER_NAME="$server" \
        -e MCP_PROTOCOL_VERSION="$MCP_PROTOCOL_VERSION" \
        -e EXPECTED_PROTOCOL_VERSION="$negotiated_protocol_version" \
        -e BENCHMARK_CONTRACT="$ELIGIBILITY_CONTRACT" \
        -e BENCHMARK_MODE=warmup \
        -e BENCHMARK_VUS="$BENCHMARK_VUS" \
        -e BENCHMARK_RAMP_DURATION="$BENCHMARK_RAMP_DURATION" \
        -e BENCHMARK_WARMUP_DURATION="$BENCHMARK_WARMUP_DURATION" \
        -e OUTPUT_PATH="/results/warmup_summary_run${run_idx}.json" \
        "$K6_IMAGE" run /scripts/benchmark.js \
        2>&1 | tee "$log_file"
    pipeline_status=("${PIPESTATUS[@]}")
    set -e
    CURRENT_K6_CONTAINER=""
    k6_exit="${pipeline_status[0]}"
    tee_exit="${pipeline_status[1]}"
    if [[ "$k6_exit" -ne 0 || "$tee_exit" -ne 0 ]]; then
        record_failure_from_log "warmup:$server:run$run_idx" "$log_file" \
            "k6 warmup failed correctness or load thresholds"
        if [[ "$k6_exit" -ne 0 ]]; then
            return "$k6_exit"
        fi
        return "$tee_exit"
    fi
    clear_failure_context
}

run_k6_measurement() {
    local server="$1"
    local server_url="$2"
    local server_results="$3"
    local run_idx="$4"
    local negotiated_protocol_version="$5"
    verify_runtime_harness
    local container="mcp-k6-measure-${server//[^a-zA-Z0-9]/-}-${run_idx}"
    local k6_pid k6_exit=0 collector_exit=0
    local log_file="$server_results/k6_console_run${run_idx}.log"
    local resource_log="$server_results/resource_headroom_run${run_idx}.log"
    local -a resource_pipeline_status
    local resource_exit tee_exit

    FAILURE_STAGE="measurement:$server:run$run_idx"
    FAILURE_REASON="k6 measurement setup or execution failed"
    FAILURE_EVIDENCE_LOG="${log_file#"$RESULTS_DIR"/}"
    CURRENT_K6_CONTAINER="$container"
    docker rm -f "$container" >/dev/null 2>&1 || true
    docker run --name "$container" \
        --network host --cpus "$BENCHMARK_K6_CPUS" --memory "$BENCHMARK_K6_MEMORY" \
        --cpuset-cpus "$HOST_CPUSET" \
        --user "$(id -u):$(id -g)" \
        -v "$K6_RUNTIME_SCRIPT_DIR:/scripts:ro" \
        -v "$server_results:/results" \
        -e SERVER_URL="$server_url" \
        -e SERVER_NAME="$server" \
        -e MCP_PROTOCOL_VERSION="$MCP_PROTOCOL_VERSION" \
        -e EXPECTED_PROTOCOL_VERSION="$negotiated_protocol_version" \
        -e BENCHMARK_CONTRACT="$ELIGIBILITY_CONTRACT" \
        -e BENCHMARK_MODE=measurement \
        -e BENCHMARK_VUS="$BENCHMARK_VUS" \
        -e BENCHMARK_MEASURE_DURATION="$BENCHMARK_MEASURE_DURATION" \
        -e OUTPUT_PATH="/results/k6_summary_run${run_idx}.json" \
        "$K6_IMAGE" run --paused --address 127.0.0.1:6565 /scripts/benchmark.js \
        2>&1 | tee "$log_file" &
    k6_pid=$!

    wait_for_container "$container"
    mkdir -p "$server_results/k6"
    python3 "$SCRIPT_DIR/capture_container.py" \
        --container "$container" --run "$run_idx" \
        --output-dir "$server_results/k6" \
        --expected-cpus "$BENCHMARK_K6_CPUS" \
        --expected-memory-bytes "$BENCHMARK_K6_MEMORY_BYTES" \
        --expected-cpuset "$HOST_CPUSET" \
        --skip-executable

    start_collector "${CONTAINER_NAME[$server]}" "$server_results/stats_run${run_idx}.json"
    start_collector mcp-redis "$server_results/redis_stats_run${run_idx}.json"
    start_collector mcp-api-service "$server_results/api_stats_run${run_idx}.json"
    start_collector "$container" "$server_results/k6_stats_run${run_idx}.json"
    start_collector @host "$server_results/host_stats_run${run_idx}.json"
    if ! wait_for_collectors_ready 15; then
        set +e
        stop_collectors
        docker rm -f "$container" >/dev/null 2>&1
        wait "$k6_pid" 2>/dev/null
        set -e
        CURRENT_K6_CONTAINER=""
        record_resource_collection_failure \
            "resource_collection:$server:run$run_idx:readiness" \
            "$server_results" "$run_idx" \
            "resource collectors did not become ready"
        error "[$server] resource collectors failed before run $run_idx"
        return 1
    fi
    resume_k6 "$server" "$run_idx"

    set +e
    wait "$k6_pid"
    k6_exit=$?
    stop_collectors
    collector_exit=$?
    set -e
    docker rm "$container" >/dev/null
    CURRENT_K6_CONTAINER=""

    if [[ "$k6_exit" -ne 0 ]]; then
        record_failure_from_log "measurement:$server:run$run_idx" "$log_file" \
            "k6 measurement failed correctness or load thresholds"
        error "[$server] measured k6 run $run_idx failed correctness or load thresholds"
        return "$k6_exit"
    fi
    if [[ "$collector_exit" -ne 0 ]]; then
        record_resource_collection_failure \
            "resource_collection:$server:run$run_idx" \
            "$server_results" "$run_idx" "resource collector failed"
        error "[$server] resource collector failed during run $run_idx"
        return "$collector_exit"
    fi
    set +e
    python3 "$SCRIPT_DIR/validate_resource_headroom.py" \
        "$server_results/resource_headroom_run${run_idx}.json" \
        --expected-duration "$BENCHMARK_MEASURE_DURATION" \
        --observed-resource "server:$server_results/stats_run${run_idx}.json:2:2147483648" \
        --resource "redis:$server_results/redis_stats_run${run_idx}.json:0.5:536870912" \
        --resource "api_service:$server_results/api_stats_run${run_idx}.json:2:2147483648" \
        --resource "load_generator:$server_results/k6_stats_run${run_idx}.json:4:2147483648" \
        --resource "host:$server_results/host_stats_run${run_idx}.json:$HOST_CPU_LIMIT:$HOST_MEMORY_BYTES" \
        2>&1 | tee "$resource_log"
    resource_pipeline_status=("${PIPESTATUS[@]}")
    set -e
    resource_exit="${resource_pipeline_status[0]}"
    tee_exit="${resource_pipeline_status[1]}"
    if [[ "$resource_exit" -ne 0 || "$tee_exit" -ne 0 ]]; then
        record_failure_from_log "resource_headroom:$server:run$run_idx" "$resource_log" \
            "resource headroom validation failed"
        if [[ "$resource_exit" -ne 0 ]]; then
            return "$resource_exit"
        fi
        return "$tee_exit"
    fi
    clear_failure_context
}

write_run_manifest() {
    local expected_images_json
    local supplemental_targets_json='[]'
    local target
    for target in "${selected_servers[@]}"; do
        if [[ "${REQUIRE_SUPPLEMENTAL[$target]:-0}" -eq 1 ]]; then
            supplemental_targets_json="$(
                jq -c --arg target "$target" '. + [$target]' \
                    <<< "$supplemental_targets_json"
            )"
        fi
    done
    expected_images_json="$(
        printf '%s\n' "${expected_image_containers[@]}" | jq -R . | jq -s .
    )"
    compose images --format json | jq -s --argjson expected "$expected_images_json" '
        (if length == 1 and (.[0] | type) == "array" then .[0] else . end)
        | map(select(.ContainerName as $name | $expected | index($name)))
        | . as $images
        | ($images | map(.ContainerName)) as $actual
        | if length > 0
            and all(.[];
              type == "object"
              and (.ContainerName | type) == "string"
              and (.ContainerName | length) > 0
              and (.ID | type) == "string"
              and (.ID | startswith("sha256:"))
              and (.ID | length) > 7)
            and (($actual | length) == ($actual | unique | length))
            and (($actual | sort) == ($expected | sort))
          then $images
          else error("compose images did not contain the exact selected container set with immutable IDs")
          end
    ' > "$RESULTS_DIR/compose_images.json"
    jq -n \
        --arg started_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --arg profile "$RUN_PROFILE" \
        --argjson runs "$BENCHMARK_RUNS" \
        --argjson vus "$BENCHMARK_VUS" \
        --argjson request_concurrency "$BENCHMARK_REQUEST_CONCURRENCY" \
        --arg ramp "$BENCHMARK_RAMP_DURATION" \
        --arg warmup "$BENCHMARK_WARMUP_DURATION" \
        --arg measurement "$BENCHMARK_MEASURE_DURATION" \
        --arg k6_image "$K6_IMAGE" \
        --arg eligibility_contract "$ELIGIBILITY_CONTRACT" \
        --arg supplemental_contract "$SUPPLEMENTAL_CONTRACT" \
        --arg host_network_policy "$HOST_NETWORK_POLICY" \
        --argjson supplemental_targets "$supplemental_targets_json" \
        --argjson k6_cpus "$BENCHMARK_K6_CPUS" \
        --argjson k6_memory_bytes "$BENCHMARK_K6_MEMORY_BYTES" \
        --slurpfile schedule "$RESULTS_DIR/run_order.json" \
        --slurpfile images "$RESULTS_DIR/compose_images.json" \
        '{schema_version: 2, status: "running", started_at: $started_at,
          profile: $profile, publishable_candidate: false,
          parameters: {runs: $runs, vus: $vus,
            request_concurrency: $request_concurrency, ramp_duration: $ramp,
            warmup_duration: $warmup, measurement_duration: $measurement,
            k6_cpus: $k6_cpus, k6_memory_bytes: $k6_memory_bytes},
          contracts: {
            eligibility: $eligibility_contract,
            supplemental: {
              name: $supplemental_contract,
              required_targets: $supplemental_targets
            }
          },
          resource_collection: {host_network_policy: $host_network_policy},
          k6_image: $k6_image, schedule: $schedule[0], images: $images[0]}' \
        > "$RESULTS_DIR/run_manifest.json"
}

generate_comparison() {
    local comparison_file="$RESULTS_DIR/comparison.txt"
    {
        printf "Benchmark comparison (%s profile)\n" "$RUN_PROFILE"
        printf "Only separate constant-VU measurement invocations are reported.\n"
        printf "Requested MCP revision: %s; each row records the server-selected revision.\n" \
            "$MCP_PROTOCOL_VERSION"
        printf "Eligibility contract: %s (identical measured predicates for every row).\n" \
            "$ELIGIBILITY_CONTRACT"
        printf "Supplemental adapter contract: %s (out-of-band evidence).\n" \
            "$SUPPLEMENTAL_CONTRACT"
        printf "Results: %s\n\n" "$RESULTS_DIR"
        printf "%-18s %-12s %-12s %-10s %-8s %-12s %-12s %-12s %-12s\n" \
            "Server" "Protocol" "Operations" "Ops/s" "CV%" "p50(ms)" "p95(ms)" "p99(ms)" "ErrorRate"
        printf "%-18s %-12s %-12s %-10s %-8s %-12s %-12s %-12s %-12s\n" \
            "------------------" "------------" "------------" "----------" "--------" \
            "------------" "------------" "------------" "------------"
        local server summary multi_stats protocol operations rps p50 p95 p99 err_rate cv_pct
        for server in "${selected_servers[@]}"; do
            summary="$RESULTS_DIR/$server/k6_summary.json"
            multi_stats="$RESULTS_DIR/$server/k6_multi_run_stats.json"
            operations="$(jq -r '.rates.operations.count' "$summary")"
            protocol="$(jq -r '.negotiated_protocol_version' "$multi_stats")"
            rps="$(jq -r '.rates.operations.per_second' "$summary")"
            p50="$(jq -r '.latency.combined_tool_call.p50_ms' "$summary")"
            p95="$(jq -r '.latency.combined_tool_call.p95_ms' "$summary")"
            p99="$(jq -r '.latency.combined_tool_call.p99_ms' "$summary")"
            err_rate="$(jq -r '.errors.mcp_rate' "$summary")"
            cv_pct="$(jq -r '.sample_cv_pct' "$multi_stats")"
            printf "%-18s %-12s %-12s %-10.2f %-8.2f %-12.2f %-12.2f %-12.2f %-12.4f\n" \
                "$server" "$protocol" "$operations" "$rps" "$cv_pct" "$p50" "$p95" "$p99" "$err_rate"
        done
    } | tee "$comparison_file"
}

selected_servers=()
if [[ "$#" -eq 0 ]]; then
    selected_servers=("${CPP_SDK_SERVERS[@]}")
elif [[ "$#" -eq 1 && "$1" == "cpp-sdks" ]]; then
    selected_servers=("${CPP_SDK_SERVERS[@]}")
elif [[ "$#" -eq 1 && "$1" == "baseline" ]]; then
    selected_servers=("${BASELINE_SERVERS[@]}")
elif [[ "$#" -eq 1 && "$1" == "all" ]]; then
    selected_servers=("${CPP_SDK_SERVERS[@]}" "${BASELINE_SERVERS[@]}")
elif [[ "$#" -eq 1 && "$1" == *","* ]]; then
    IFS=',' read -r -a selected_servers <<< "$1"
else
    selected_servers=("$@")
fi

load_alternative_manifest

declare -A seen_servers=()
for server in "${selected_servers[@]}"; do
    case "$server" in
        cpp|ours-comparable|python|go|rust|hkr04|fastmcpp|cxxmcp|neumann) ;;
        gopher-mcp)
            error "gopher-mcp exposes legacy HTTP+SSE rather than the shared /mcp Streamable HTTP contract"
            exit 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            error "Unknown server '$server'"
            usage
            exit 1
            ;;
    esac
    [[ -z "${seen_servers[$server]:-}" ]] || {
        error "Duplicate server selection: $server"
        exit 1
    }
    seen_servers[$server]=1
done

[[ "${#selected_servers[@]}" -gt 0 ]] || {
    error "At least one server must be selected"
    exit 1
}
[[ "$BENCHMARK_RUNS" =~ ^[1-9][0-9]*$ && $((BENCHMARK_RUNS % 2)) -eq 1 ]] || {
    error "BENCHMARK_RUNS must be a positive odd integer"
    exit 1
}
[[ "$BENCHMARK_VUS" =~ ^[1-9][0-9]*$ ]] || {
    error "BENCHMARK_VUS must be a positive integer"
    exit 1
}
[[ "$BENCHMARK_ORDER_SEED" =~ ^[0-9]+$ ]] || {
    error "BENCHMARK_ORDER_SEED must be a non-negative integer"
    exit 1
}

RUN_PROFILE="smoke"
baseline_selection=true
for server in "${selected_servers[@]}"; do
    case "$server" in
        python|go|rust) ;;
        *) baseline_selection=false ;;
    esac
done
if [[ "$BENCHMARK_RUNS" -eq 3 \
    && "$BENCHMARK_VUS" -eq 50 \
    && "$BENCHMARK_RAMP_DURATION" == "15s" \
    && "$BENCHMARK_WARMUP_DURATION" == "60s" \
    && "$BENCHMARK_MEASURE_DURATION" == "5m" ]]; then
    if [[ "${selected_servers[*]}" == "${CPP_SDK_SERVERS[*]}" ]]; then
        RUN_PROFILE="production"
    elif [[ "$baseline_selection" == true ]]; then
        RUN_PROFILE="baseline-diagnostic"
    fi
fi
if [[ "$RUN_PROFILE" == "production" \
    && "$BENCHMARK_VUS" -ne "$BENCHMARK_REQUEST_CONCURRENCY" ]]; then
    error "Production VUs must match request concurrency ($BENCHMARK_REQUEST_CONCURRENCY)"
    exit 1
fi

acquire_benchmark_lock

timestamp="$(date +%Y%m%d_%H%M%S)"
RESULTS_DIR="${BENCHMARK_RESULTS_DIR:-$SCRIPT_DIR/results/${timestamp}_${RUN_PROFILE}}"
if [[ -e "$RESULTS_DIR" \
    && -n "$(find "$RESULTS_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    error "Results directory is not empty: $RESULTS_DIR"
    exit 1
fi
mkdir -p "$RESULTS_DIR"

info "Preflight: corrected $RUN_PROFILE profile for ${selected_servers[*]}"
command -v docker >/dev/null || { error "docker not found"; exit 1; }
command -v git >/dev/null || { error "git not found"; exit 1; }
command -v jq >/dev/null || { error "jq not found"; exit 1; }
command -v python3 >/dev/null || { error "python3 not found"; exit 1; }
command -v curl >/dev/null || { error "curl not found"; exit 1; }
HOST_CPU_LIMIT="$(python3 -c 'import os; print(len(os.sched_getaffinity(0)))')"
HOST_CPUSET="$(python3 -c 'import os; print(",".join(str(cpu) for cpu in sorted(os.sched_getaffinity(0))))')"
HOST_MEMORY_BYTES="$(awk '/^MemTotal:/ {printf "%.0f", $2 * 1024; exit}' /proc/meminfo)"
[[ "$HOST_CPU_LIMIT" =~ ^[1-9][0-9]*$ ]] || { error "Could not determine host CPU affinity"; exit 1; }
[[ "$HOST_MEMORY_BYTES" =~ ^[1-9][0-9]*$ ]] || { error "Could not determine host memory"; exit 1; }
[[ -n "$HOST_CPUSET" ]] || { error "Could not determine host CPU set"; exit 1; }
export BENCHMARK_CPUSET="$HOST_CPUSET"
if [[ "$RUN_PROFILE" != "smoke" && "$HOST_CPU_LIMIT" -lt 9 ]]; then
    error "Full-duration runs require at least 9 host CPUs for 8.5 CPUs of configured limits"
    exit 1
fi
compose version >/dev/null
[[ -f "$K6_SCRIPT" ]] || { error "Missing local k6 profile: $K6_SCRIPT"; exit 1; }
for helper in benchmark_order.py capture_container.py capture_environment.py \
    collect_stats.py select_median_run.py summarize_collector_failures.py \
    validate_resource_headroom.py verify_server.py; do
    [[ -f "$SCRIPT_DIR/$helper" ]] || { error "Missing benchmark helper: $helper"; exit 1; }
done

mkdir -p "$RESULTS_DIR/harness"
cp "$COMPOSE_FILE" "$RESULTS_DIR/harness/docker-compose.yml"
cp "$K6_SCRIPT" "$RESULTS_DIR/harness/benchmark.js"
(
    cd "$RESULTS_DIR/harness"
    sha256sum benchmark.js docker-compose.yml > SHA256SUMS
)
RUNTIME_COMPOSE_FILE="$RESULTS_DIR/harness/docker-compose.yml"
K6_RUNTIME_SCRIPT_DIR="$RESULTS_DIR/harness"
RUNTIME_COMPOSE_SHA256="$(sha256sum "$RUNTIME_COMPOSE_FILE" | awk '{print $1}')"
RUNTIME_K6_SHA256="$(sha256sum "$K6_RUNTIME_SCRIPT_DIR/benchmark.js" | awk '{print $1}')"
chmod 0444 "$RUNTIME_COMPOSE_FILE" "$K6_RUNTIME_SCRIPT_DIR/benchmark.js" \
    "$K6_RUNTIME_SCRIPT_DIR/SHA256SUMS"

ensure_upstream_sources
for server in "${selected_servers[@]}"; do
    if [[ -n "${ALTERNATIVE_REPO[$server]:-}" ]]; then
        [[ "${ALTERNATIVE_STATUS[$server]}" == "supported" ]] || {
            error "$server is not runnable: ${ALTERNATIVE_STATUS[$server]}"
            exit 2
        }
        ensure_alternative_source "$server"
    fi
done

python3 "$SCRIPT_DIR/benchmark_order.py" \
    --seed "$BENCHMARK_ORDER_SEED" --runs "$BENCHMARK_RUNS" \
    "${selected_servers[@]}" > "$RESULTS_DIR/run_order.json"
compose config > "$RESULTS_DIR/compose.resolved.yml"

info "Pulling pinned Redis and k6 images"
compose --profile seeder pull redis
docker pull "$K6_IMAGE"

environment_args=(
    "$RESULTS_DIR/environment.json"
    --project-dir "$PROJECT_DIR"
    --upstream-dir "$UPSTREAM_BENCHMARK_DIR"
    --upstream-url "$UPSTREAM_BENCHMARK_REPO"
    --alternative-root "$ALTERNATIVE_SOURCE_DIR"
    --sources-manifest "$ALTERNATIVE_MANIFEST"
    --protocol-version "$MCP_PROTOCOL_VERSION"
    --eligibility-contract "$ELIGIBILITY_CONTRACT"
    --k6-image "$K6_IMAGE"
    --upstream-commit "$UPSTREAM_BENCHMARK_COMMIT"
    --order-seed "$BENCHMARK_ORDER_SEED"
    --runs "$BENCHMARK_RUNS"
    --vus "$BENCHMARK_VUS"
    --measurement-duration "$BENCHMARK_MEASURE_DURATION"
    --warmup-duration "$BENCHMARK_WARMUP_DURATION"
    --ramp-duration "$BENCHMARK_RAMP_DURATION"
    --servers "${selected_servers[@]}"
)
python3 "$SCRIPT_DIR/capture_environment.py" "${environment_args[@]}"

build_services=(api-service redis-seeder)
for server in "${selected_servers[@]}"; do
    build_services+=("${SERVICE_NAME[$server]}")
done
info "Building every selected server once before the schedule"
compose build "${build_services[@]}" 2>&1 | tee "$RESULTS_DIR/build.log"
python3 "$SCRIPT_DIR/capture_environment.py" \
    --verify-source "$RESULTS_DIR/environment.json" --project-dir "$PROJECT_DIR"

# Materialize stopped containers so Compose can report the exact built image
# IDs before execution. Every measured target is still force-recreated later.
image_services=(redis api-service)
expected_image_containers=(mcp-redis mcp-api-service mcp-redis-seeder)
for server in "${selected_servers[@]}"; do
    image_services+=("${SERVICE_NAME[$server]}")
    expected_image_containers+=("${CONTAINER_NAME[$server]}")
done
compose --profile seeder create "${image_services[@]}" redis-seeder >/dev/null

write_run_manifest

info "Starting shared Redis and API service"
BENCHMARK_ACTIVE=1
compose up -d --force-recreate redis api-service
wait_for_http "api-service" "http://localhost:8100/health" 60
start_redis_wait="$(date +%s)"
until compose exec -T redis redis-cli ping >/dev/null 2>&1; do
    if (( $(date +%s) - start_redis_wait >= 60 )); then
        error "Timeout waiting for Redis"
        exit 1
    fi
    sleep 1
done

mkdir -p "$RESULTS_DIR/shared/redis" "$RESULTS_DIR/shared/api_service"
python3 "$SCRIPT_DIR/capture_container.py" \
    --container mcp-redis --run 1 --output-dir "$RESULTS_DIR/shared/redis" \
    --expected-cpus 0.5 --expected-memory-bytes 536870912 \
    --expected-cpuset "$HOST_CPUSET"
python3 "$SCRIPT_DIR/capture_container.py" \
    --container mcp-api-service --run 1 \
    --output-dir "$RESULTS_DIR/shared/api_service" \
    --expected-cpus 2 --expected-memory-bytes 2147483648 \
    --expected-cpuset "$HOST_CPUSET"

sequence_index=0
for run_idx in $(seq 1 "$BENCHMARK_RUNS"); do
    mapfile -t round_servers < <(
        python3 "$SCRIPT_DIR/benchmark_order.py" \
            --seed "$BENCHMARK_ORDER_SEED" --runs "$BENCHMARK_RUNS" \
            --run "$run_idx" "${selected_servers[@]}"
    )
    for server in "${round_servers[@]}"; do
        sequence_index=$((sequence_index + 1))
        service="${SERVICE_NAME[$server]}"
        container="${CONTAINER_NAME[$server]}"
        server_url="${MCP_URL[$server]}"
        server_results="$RESULTS_DIR/$server"
        mkdir -p "$server_results"
        CURRENT_SERVER="$container"
        CURRENT_SERVER_RESULTS="$server_results"

        info "[$sequence_index] $server round $run_idx: recreate target"
        compose stop "${ALL_MCP_SERVICES[@]}" >/dev/null
        compose up -d --force-recreate --no-deps "$service"
        if [[ "${HEALTH_KIND[$server]}" == "mcp" ]]; then
            wait_for_mcp "$server" "${HEALTH_URL[$server]}" 90
        else
            wait_for_http "$server" "${HEALTH_URL[$server]}" 90
        fi

        python3 "$SCRIPT_DIR/capture_container.py" \
            --container "$container" --run "$run_idx" --output-dir "$server_results" \
            --expected-cpus 2 --expected-memory-bytes 2147483648 \
            --expected-cpuset "$HOST_CPUSET"

        reset_redis_dataset
        info "[$server] correctness and negotiation gate"
        verifier_contract_args=(
            --eligibility-contract "$ELIGIBILITY_CONTRACT"
            --redis-url "redis://127.0.0.1:6379/0"
        )
        if [[ "${REQUIRE_SUPPLEMENTAL[$server]:-0}" -eq 1 ]]; then
            verifier_contract_args+=(--require-supplemental)
        fi
        protocol_preflight="$server_results/protocol_preflight_run${run_idx}.json"
        preflight_log="$server_results/protocol_preflight_run${run_idx}.log"
        run_protocol_verifier "protocol_preflight:$server:run$run_idx" \
            "$preflight_log" "$server_url" --name "$server" \
            --expected-server-type "${EXPECTED_SERVER_TYPE[$server]}" \
            "${verifier_contract_args[@]}" \
            --output "$protocol_preflight"
        negotiated_protocol_version="$(
            jq -er '.negotiated_protocol_version | select(type == "string" and length > 0)' \
                "$protocol_preflight"
        )"
        reset_redis_dataset

        info "[$server] separate warmup (round $run_idx)"
        run_k6_warmup "$server" "$server_url" "$server_results" "$run_idx" \
            "$negotiated_protocol_version"

        # Warmup mutates checkout history and rate-limit state. Keep the process
        # alive for warm runtime caches, but restore the canonical dataset.
        reset_redis_dataset

        info "[$server] measured constant-VU run $run_idx/$BENCHMARK_RUNS"
        run_k6_measurement "$server" "$server_url" "$server_results" "$run_idx" \
            "$negotiated_protocol_version"

        # Re-seed before the postflight so the universal workload contract and
        # supplemental diagnostics are evaluated from the same canonical state.
        reset_redis_dataset
        postflight_log="$server_results/protocol_postflight_run${run_idx}.log"
        run_protocol_verifier "protocol_postflight:$server:run$run_idx" \
            "$postflight_log" "$server_url" \
            --name "$server-post-run" \
            --expected-protocol-version "$negotiated_protocol_version" \
            --expected-server-type "${EXPECTED_SERVER_TYPE[$server]}" \
            "${verifier_contract_args[@]}" \
            --output "$server_results/protocol_postflight_run${run_idx}.json"
        docker logs "$container" > "$server_results/server_run${run_idx}.log" 2>&1
        compose stop "$service" >/dev/null
        CURRENT_SERVER=""
        CURRENT_SERVER_RESULTS=""
        ok "[$server] round $run_idx passed"
    done
done

python3 "$SCRIPT_DIR/capture_environment.py" \
    --verify-source "$RESULTS_DIR/environment.json" --project-dir "$PROJECT_DIR"
for server in "${selected_servers[@]}"; do
    python3 "$SCRIPT_DIR/select_median_run.py" "$RESULTS_DIR/$server" "$BENCHMARK_RUNS"
done

generate_comparison
verify_runtime_harness
python3 "$SCRIPT_DIR/capture_environment.py" \
    --verify-source "$RESULTS_DIR/environment.json" --project-dir "$PROJECT_DIR"
RUN_SUCCEEDED=1
ok "Corrected $RUN_PROFILE benchmark complete: $RESULTS_DIR"
