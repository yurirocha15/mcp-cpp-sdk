#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$SCRIPT_DIR/results/$(date +%Y%m%d_%H%M%S)"
UPSTREAM_BENCHMARK_DIR="$SCRIPT_DIR/benchmark-mcp-servers-v2"
UPSTREAM_BENCHMARK_REPO="https://github.com/thiagomendes/benchmark-mcp-servers-v2.git"
UPSTREAM_BENCHMARK_COMMIT="8a9a5f8ef505f46b6079072ef4603304ca672e33"
UPSTREAM_K6_SCRIPT="$UPSTREAM_BENCHMARK_DIR/benchmark/benchmark.js"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() {
    printf "%b[INFO]%b %s\n" "$BLUE" "$NC" "$*"
}

ok() {
    printf "%b[OK]%b %s\n" "$GREEN" "$NC" "$*"
}

warn() {
    printf "%b[WARN]%b %s\n" "$YELLOW" "$NC" "$*"
}

error() {
    printf "%b[ERROR]%b %s\n" "$RED" "$NC" "$*" >&2
}

declare -A SERVICE_NAME=(
    [cpp]="cpp-server"
    [python]="python-server"
    [go]="go-server"
    [rust]="rust-server"
)

declare -A CONTAINER_NAME=(
    [cpp]="mcp-cpp-server"
    [python]="mcp-python-server"
    [go]="mcp-go-server"
    [rust]="mcp-rust-server"
)

declare -A MCP_URL=(
    [cpp]="http://localhost:8080/mcp"
    [python]="http://localhost:8081/mcp"
    [go]="http://localhost:8082/mcp"
    [rust]="http://localhost:8083/mcp"
)

declare -A HEALTH_URL=(
    [cpp]="http://localhost:8080/health"
    [python]="http://localhost:8081/health"
    [go]="http://localhost:8082/health"
    [rust]="http://localhost:8083/health"
)

stats_pid=""

cleanup_on_exit() {
    if [[ -n "${stats_pid:-}" ]] && kill -0 "$stats_pid" 2>/dev/null; then
        warn "Stopping running stats collector (PID $stats_pid)..."
        kill "$stats_pid" 2>/dev/null || true
        wait "$stats_pid" 2>/dev/null || true
    fi
}

trap cleanup_on_exit EXIT

usage() {
    cat <<'USAGE'
Usage:
  ./run.sh                  # benchmark cpp, python, go, rust
  ./run.sh all              # benchmark cpp, python, go, rust
  ./run.sh cpp              # benchmark only cpp
  ./run.sh cpp python       # benchmark selected servers
  ./run.sh cpp,python       # benchmark selected servers (comma-separated)
USAGE
}

ensure_upstream_benchmark_servers() {
    command -v git >/dev/null || { error "git not found (required to clone upstream benchmark servers)"; return 1; }

    if [[ ! -d "$UPSTREAM_BENCHMARK_DIR/.git" ]]; then
        if [[ -e "$UPSTREAM_BENCHMARK_DIR" ]]; then
            error "Expected clone target exists but is not a git repo: $UPSTREAM_BENCHMARK_DIR"
            error "Please remove it or convert it into a valid clone of $UPSTREAM_BENCHMARK_REPO"
            return 1
        fi

        info "Cloning upstream benchmark servers to $UPSTREAM_BENCHMARK_DIR"
        git clone "$UPSTREAM_BENCHMARK_REPO" "$UPSTREAM_BENCHMARK_DIR"
        ok "Upstream benchmark servers cloned"
    fi

    if [[ -n "$(git -C "$UPSTREAM_BENCHMARK_DIR" status --porcelain --untracked-files=no)" ]]; then
        error "Upstream benchmark repo has local modifications: $UPSTREAM_BENCHMARK_DIR"
        error "Please clean it before running benchmark to keep pinned reproducibility"
        return 1
    fi

    info "Pinning upstream benchmark repo to commit $UPSTREAM_BENCHMARK_COMMIT"
    git -C "$UPSTREAM_BENCHMARK_DIR" fetch --depth 1 origin "$UPSTREAM_BENCHMARK_COMMIT"
    git -C "$UPSTREAM_BENCHMARK_DIR" checkout --detach "$UPSTREAM_BENCHMARK_COMMIT"

    local current_commit
    current_commit="$(git -C "$UPSTREAM_BENCHMARK_DIR" rev-parse HEAD)"
    if [[ "$current_commit" != "$UPSTREAM_BENCHMARK_COMMIT" ]]; then
        error "Failed to pin upstream benchmark repo to expected commit"
        error "Current: $current_commit"
        error "Expected: $UPSTREAM_BENCHMARK_COMMIT"
        return 1
    fi

    local required_paths=(
        "$UPSTREAM_BENCHMARK_DIR/api-service"
        "$UPSTREAM_BENCHMARK_DIR/infra/redis"
        "$UPSTREAM_BENCHMARK_DIR/benchmark"
        "$UPSTREAM_BENCHMARK_DIR/python-server"
        "$UPSTREAM_BENCHMARK_DIR/go-server"
        "$UPSTREAM_BENCHMARK_DIR/rust-server"
    )
    for p in "${required_paths[@]}"; do
        if [[ ! -d "$p" ]]; then
            error "Missing required upstream path after checkout: $p"
            return 1
        fi
    done

    if [[ ! -f "$UPSTREAM_K6_SCRIPT" ]]; then
        error "Missing required upstream k6 script: $UPSTREAM_K6_SCRIPT"
        return 1
    fi

    ok "Using upstream benchmark servers repo pinned at $UPSTREAM_BENCHMARK_COMMIT"
}

wait_for_http() {
    local name="$1"
    local url="$2"
    local timeout_secs="$3"
    local start_ts
    start_ts="$(date +%s)"

    info "Waiting for $name at $url (timeout ${timeout_secs}s)..."
    while true; do
        if curl -fsS "$url" >/dev/null 2>&1; then
            ok "$name is healthy"
            return 0
        fi
        if (( $(date +%s) - start_ts >= timeout_secs )); then
            error "Timeout waiting for $name at $url"
            return 1
        fi
        sleep 1
    done
}

warmup_single_session() {
    local server_url="$1"
    local tool_name="$2"
    local args_json="$3"

    local hdr
    hdr="$(mktemp)"

    curl -fsS -D "$hdr" -o /dev/null -X POST "$server_url" \
        -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" \
        -H "MCP-Protocol-Version: 2025-11-25" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"benchmark-warmup","version":"1.0"}}}'

    local session_id
    session_id="$(tr -d '\r' < "$hdr" | grep -i '^mcp-session-id:' | sed 's/^[^:]*: *//' | head -n1 || true)"
    rm -f "$hdr"

    curl -fsS -o /dev/null -X POST "$server_url" \
        -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" \
        -H "MCP-Protocol-Version: 2025-11-25" \
        ${session_id:+-H "Mcp-Session-Id: $session_id"} \
        -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'

    curl -fsS -o /dev/null -X POST "$server_url" \
        -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" \
        -H "MCP-Protocol-Version: 2025-11-25" \
        ${session_id:+-H "Mcp-Session-Id: $session_id"} \
        -d "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"${tool_name}\",\"arguments\":${args_json}}}"

    if [[ -n "$session_id" ]]; then
        curl -fsS -o /dev/null -X DELETE "$server_url" \
            -H "Accept: application/json, text/event-stream" \
            -H "MCP-Protocol-Version: 2025-11-25" \
            -H "Mcp-Session-Id: $session_id"
    fi
}

run_warmup() {
    local server_name="$1"
    local server_url="$2"

    info "Warmup for $server_name: 5 initialize requests"
    for _ in {1..5}; do
        local hdr
        hdr="$(mktemp)"

        curl -fsS -D "$hdr" -o /dev/null -X POST "$server_url" \
            -H "Content-Type: application/json" \
            -H "Accept: application/json, text/event-stream" \
            -H "MCP-Protocol-Version: 2025-11-25" \
            -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"benchmark-warmup","version":"1.0"}}}'

        local session_id
        session_id="$(tr -d '\r' < "$hdr" | grep -i '^mcp-session-id:' | sed 's/^[^:]*: *//' | head -n1 || true)"
        rm -f "$hdr"

        if [[ -n "$session_id" ]]; then
            curl -fsS -o /dev/null -X DELETE "$server_url" \
                -H "Accept: application/json, text/event-stream" \
                -H "MCP-Protocol-Version: 2025-11-25" \
                -H "Mcp-Session-Id: $session_id"
        fi
    done

    info "Warmup for $server_name: 3 full sessions per tool"
    for _ in {1..3}; do
        warmup_single_session "$server_url" "search_products" '{"category":"Electronics","min_price":50,"max_price":500,"limit":10}'
        warmup_single_session "$server_url" "get_user_cart" '{"user_id":"user-00001"}'
        warmup_single_session "$server_url" "checkout" '{"user_id":"user-00001","items":[{"product_id":42,"quantity":2},{"product_id":1337,"quantity":1}]}'
    done

    ok "Warmup finished for $server_name"
}

selected_servers=()
if [[ "$#" -eq 0 ]]; then
    selected_servers=(cpp python go rust)
elif [[ "$#" -eq 1 && "$1" == "all" ]]; then
    selected_servers=(cpp python go rust)
elif [[ "$#" -eq 1 && "$1" == *","* ]]; then
    IFS=',' read -r -a selected_servers <<< "$1"
else
    selected_servers=("$@")
fi

for server in "${selected_servers[@]}"; do
    case "$server" in
        cpp|python|go|rust) ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            error "Unknown server '$server'. Allowed: cpp, python, go, rust, all"
            usage
            exit 1
            ;;
    esac
done

mkdir -p "$RESULTS_DIR"

info "Step 1/6: Pre-flight checks"
docker compose version >/dev/null || { error "docker compose not found"; exit 1; }
command -v python3 >/dev/null    || { error "python3 not found"; exit 1; }
command -v jq >/dev/null         || { error "jq not found"; exit 1; }
ensure_upstream_benchmark_servers || exit 1
ok "Pre-flight checks passed"

info "Step 2/6: Start infrastructure"
docker compose -f "$SCRIPT_DIR/docker-compose.yml" up -d redis api-service
wait_for_http "api-service" "http://localhost:8100/health" 30

info "Waiting for redis readiness"
start_redis_wait="$(date +%s)"
while true; do
    if docker compose -f "$SCRIPT_DIR/docker-compose.yml" exec -T redis redis-cli ping >/dev/null 2>&1; then
        ok "redis is healthy"
        break
    fi
    if (( $(date +%s) - start_redis_wait >= 30 )); then
        error "Timeout waiting for redis"
        exit 1
    fi
    sleep 1
done

info "Step 3/6: Seed Redis"
docker rm -f mcp-redis-seeder >/dev/null 2>&1 || true
docker compose -f "$SCRIPT_DIR/docker-compose.yml" --profile seeder run --rm redis-seeder
ok "Redis seeding completed"

info "Step 4/6: Benchmark selected servers: ${selected_servers[*]}"
for server_name in "${selected_servers[@]}"; do
    service_name="${SERVICE_NAME[$server_name]}"
    container_name="${CONTAINER_NAME[$server_name]}"
    mcp_url="${MCP_URL[$server_name]}"
    health_url="${HEALTH_URL[$server_name]}"
    server_results="$RESULTS_DIR/$server_name"
    mkdir -p "$server_results"

    info "[$server_name] Reset Redis"
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" exec -T redis redis-cli FLUSHDB >/dev/null
    docker rm -f mcp-redis-seeder >/dev/null 2>&1 || true
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" --profile seeder run --rm redis-seeder >/dev/null
    ok "[$server_name] Redis reset/reseed done"

    info "[$server_name] Stop all MCP servers"
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" stop cpp-server python-server go-server rust-server>/dev/null

    info "[$server_name] Start target server: $service_name"
    docker compose -f "$SCRIPT_DIR/docker-compose.yml" up -d "$service_name"

    health_timeout=30
    if [[ "$server_name" == "cpp" ]]; then
        health_timeout=60
    fi
    wait_for_http "$server_name server" "$health_url" "$health_timeout"

    run_warmup "$server_name" "$mcp_url"

    info "[$server_name] Start stats collector"
    python3 "$SCRIPT_DIR/collect_stats.py" "$container_name" "$server_results/stats.json" 1.0 &
    stats_pid=$!

    K6_RUNS=3
    info "[$server_name] Run k6 benchmark ($K6_RUNS iterations)"
    for run_idx in $(seq 1 "$K6_RUNS"); do
        info "[$server_name] k6 run $run_idx/$K6_RUNS"

        if (( run_idx > 1 )); then
            info "[$server_name] Re-seed Redis before run $run_idx"
            docker compose -f "$SCRIPT_DIR/docker-compose.yml" exec -T redis redis-cli FLUSHDB >/dev/null
            docker rm -f mcp-redis-seeder >/dev/null 2>&1 || true
            docker compose -f "$SCRIPT_DIR/docker-compose.yml" --profile seeder run --rm redis-seeder >/dev/null
            run_warmup "$server_name" "$mcp_url"
        fi

        docker run --rm \
            --network host \
            --user "$(id -u):$(id -g)" \
            -v "$UPSTREAM_BENCHMARK_DIR/benchmark:/scripts:ro" \
            -v "$server_results:/results" \
            -e SERVER_URL="$mcp_url" \
            -e SERVER_NAME="$server_name" \
            -e OUTPUT_PATH="/results/k6_summary_run${run_idx}.json" \
            grafana/k6:latest run /scripts/benchmark.js \
            2>&1 | tee "$server_results/k6_console_run${run_idx}.log"
    done

    # Pick the median run by RPS and select as the canonical k6_summary.json
    python3 "$SCRIPT_DIR/select_median_run.py" "$server_results" "$K6_RUNS"

    info "[$server_name] Stop stats collector"
    if kill -0 "$stats_pid" 2>/dev/null; then
        kill "$stats_pid" 2>/dev/null || true
        wait "$stats_pid" 2>/dev/null || true
    fi
    stats_pid=""

    ok "[$server_name] Benchmark complete ($K6_RUNS runs, median selected)"
done

info "Step 5/6: Generate comparison summary"
comparison_file="$RESULTS_DIR/comparison.txt"

{
    printf "Benchmark comparison\n"
    printf "Results: %s\n\n" "$RESULTS_DIR"
    printf "%-10s %-12s %-10s %-8s %-12s %-12s %-12s %-12s\n" "Server" "Requests" "RPS" "CV%" "p50(ms)" "p95(ms)" "p99(ms)" "ErrorRate"
    printf "%-10s %-12s %-10s %-8s %-12s %-12s %-12s %-12s\n" "----------" "------------" "----------" "--------" "------------" "------------" "------------" "------------"

    for server_name in "${selected_servers[@]}"; do
        summary="$RESULTS_DIR/$server_name/k6_summary.json"
        multi_stats="$RESULTS_DIR/$server_name/k6_multi_run_stats.json"
        if [[ ! -f "$summary" ]]; then
            printf "%-10s %-12s %-10s %-8s %-12s %-12s %-12s %-12s\n" "$server_name" "N/A" "N/A" "N/A" "N/A" "N/A" "N/A" "N/A"
            continue
        fi

        requests="$(jq -r '.mcp.total_mcp_requests // 0' "$summary")"
        rps="$(jq -r '.http.rps // 0' "$summary")"
        p50="$(jq -r '.http.latency.p50 // 0' "$summary")"
        p95="$(jq -r '.http.latency.p95 // 0' "$summary")"
        p99="$(jq -r '.http.latency.p99 // 0' "$summary")"
        err_rate="$(jq -r '.mcp.error_rate // 0' "$summary")"

        cv_pct="N/A"
        if [[ -f "$multi_stats" ]]; then
            cv_pct="$(jq -r '.cv_pct' "$multi_stats")"
        fi

        printf "%-10s %-12s %-10.2f %-8s %-12.2f %-12.2f %-12.2f %-12.4f\n" \
            "$server_name" "$requests" "$rps" "${cv_pct}%" "$p50" "$p95" "$p99" "$err_rate"
    done
} | tee "$comparison_file"

ok "Comparison saved to $comparison_file"

info "Step 6/6: Cleanup"
ok "Benchmark finished. Results directory: $RESULTS_DIR"
warn "Docker services left running intentionally for inspection"
