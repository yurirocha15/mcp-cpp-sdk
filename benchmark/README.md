# MCP benchmark

This directory contains a reproducible, end-to-end benchmark for MCP servers.
The workload sends MCP requests over HTTP and exercises the same Redis and HTTP
application operations for every comparable C++ SDK.

The application workload is based on
[TM Dev Lab v2](https://github.com/thiagomendes/benchmark-mcp-servers-v2), pinned
to commit `8a9a5f8ef505f46b6079072ef4603304ca672e33`. That repository supplies the API
service, Redis seeder, and language-baseline server sources. Load generation
uses the audited local profile in `alternatives/benchmark.js`; it does not use
the upstream k6 script.

## Prerequisites

- A Linux host with Docker cgroup accounting enabled; the harness reads Linux
  CPU affinity and cgroup files and uses host networking
- Docker with Compose v2 (`docker compose`)
- `python3`, `jq`, and `git`

k6 runs from a container image pinned by digest, so a host k6 installation is
not required. A full-duration run also requires at least nine CPUs in the
orchestrator's effective affinity set; this leaves measurable capacity for the
8.5 CPUs assigned across the target and shared services.

## Quick start

```bash
cd benchmark

# Run the five comparable C++ SDKs.
./run.sh cpp-sdks

# Run the language baselines as a diagnostic (not a publishable ranking).
./run.sh baseline

# Run selected servers.
./run.sh ours-comparable hkr04
```

The publishable C++ comparison uses the production defaults: three measured
runs per SDK, 50 virtual users, and five minutes of constant load per measured
run. Environment variables exposed by `run.sh` may shorten a local smoke test,
but results from a shortened profile must not be published as benchmark data.
Only one `run.sh` invocation may use the host at a time. An exclusive lock in
`/tmp` rejects overlapping runs across worktrees before they can share the
fixed containers or ports.

The result-directory suffix and `run_manifest.json` identify the effective
profile:

| Invocation | Result profile | Publication status |
|---|---|---|
| `./run.sh cpp-sdks` with exact defaults | `production` | Publishable candidate after every gate passes |
| `./run.sh baseline` with exact defaults | `baseline-diagnostic` | Diagnostic only; never an equal-work ranking |
| Any parameter override or other server selection | `smoke` | Harness validation only |

`publishable_candidate` remains false while a run is in progress and after any
failure. It becomes true only when the exact production profile reaches
`status: "complete"`; consumers must require both fields. Generated result
directories are ignored by default so retaining or publishing an audited bundle
requires an explicit action.

## Comparable C++ scope

`cpp-sdks` contains five public-API adapters:

| Benchmark name | SDK | Host port |
|---|---|---:|
| `ours-comparable` | this SDK | 8089 |
| `hkr04` | hkr04/cpp-mcp | 8084 |
| `fastmcpp` | FastMCPP | 8085 |
| `cxxmcp` | cxxmcp | 8086 |
| `neumann` | Neumann-Labs/mcp-cpp | 8087 |

External source repositories and exact commits are recorded in
`alternatives/sources.tsv`. The adapters use each SDK's public API. They do not
copy, vendor, patch, or bypass another SDK's protocol or server implementation.

The harness pins direct source commits and top-level container images, but it
does not claim a bit-for-bit hermetic rebuild. Ubuntu package repositories and
transitive dependencies resolved by the alternatives' CMake builds are not all
content-addressed; their resolved image identities are therefore captured with
each run.

Gopher MCP remains recorded in the source manifest but excluded from this
group. Its public server transport uses legacy HTTP+SSE rather than the single
Streamable HTTP endpoint exercised here, so including it would test a different
transport contract. cxxmcp is included: the local client sends the required
negotiated protocol-version header after initialization.

The baseline group is separate from the comparable C++ group. The pinned
upstream repository supplies the Python, Go, and Rust baseline servers and the
shared infrastructure. The optimized `cpp` baseline is retained as a separate
application implementation and must not be presented as an SDK-adapter result.

Python, Go, and Rust are rerun after harness changes, but their results remain
diagnostic. Their upstream Docker builds contain floating inputs: Python uses
version ranges, Go regenerates dependency resolution during the image build,
Rust does not copy its lockfile into the build, and the baseline Dockerfiles use
unpinned base-image tags. Exact built image identities are retained with every
run, but these inputs prevent a source commit from defining a bit-for-bit
rebuild.

Every target uses the same `upstream-v2-strict-mcp-v1` measurement eligibility
contract. It preserves the pinned benchmark's observable business predicates
while adding uniform MCP lifecycle, zero-error, resource, and Redis-side-effect
gates. The five C++ adapters also must pass `adapter-exact-v1` during preflight
and postflight because their glue and shared workload are controlled here; that
supplemental check does not execute in the measured k6 path.

The supplemental diagnostics intentionally expose two upstream limitations
without excluding otherwise working language baselines. Python accepts the
canonical checkout call but advertises unconstrained array elements for its
untyped `items: list` parameter. Rust executes all three Redis mutations in one
pipeline, but returns the pipeline's `ZADD` result as `rate_limit_count` instead
of the `INCR` result. Both pass the inherited observable predicates and the
direct Redis side-effect gate; Python and Rust simply record the corresponding
`adapter-exact-v1` diagnostic as false. These interface, build, and execution
differences are why language baselines must not appear in the publishable C++
SDK ranking.

## Workload

Every comparable adapter registers the same three tools and uses the shared
`benchmark_workload` implementation:

| Tool | End-to-end operations |
|---|---|
| `search_products` | HTTP product search in parallel with Redis popularity lookup |
| `get_user_cart` | Redis cart lookup followed by HTTP product lookup and Redis history lookup |
| `checkout` | HTTP total calculation, Redis rate-limit increment, history append, and popularity update submitted concurrently |

Each virtual user repeatedly opens MCP sessions, calls all three tools, calls
`tools/list`, and closes stateful sessions. The headline operations rate counts
`tools/call` and `tools/list` responses only after their shape and universal
contract checks have run. Raw HTTP request rate is reported separately because it also
includes initialization, notification, and session cleanup traffic. Tool-call
latency is collected in one combined k6 trend across the three tools, with each
tool also reported separately.

This is an end-to-end Redis/HTTP workload. It is not a parser-only,
serialization-only, or in-process SDK microbenchmark.

## Concurrency and resource controls

Each comparable server container is limited to 2 CPUs and 2 GiB of memory.
Top-level blocking request/handler capacity is normalized to 50, matching the
production profile's 50 VUs. This prevents an HTTP implementation whose workers
own persistent keep-alive connections from being limited to fewer active
clients than the load profile. It is an admission-capacity control, not a claim
that every process has the same number of OS threads or 50 CPUs.

All five adapters call the same application workload, whose internal pool has
64 threads for the Redis and HTTP work. This SDK retains two asynchronous I/O
threads. hkr04's separate two-thread async pool is not used by its synchronous
Streamable HTTP path. SDK/runtime-owned I/O, session, timer, logging, and other
auxiliary threads otherwise remain native to each implementation and can
differ. Those differences are not hidden; they are part of the process measured
by Docker CPU, memory, and network sampling. The harness records the effective
container limits and image identity for every measured run.

Redis is limited to 0.5 CPU and 512 MiB, the API service to 2 CPUs and 2 GiB,
and k6 to 4 CPUs and 2 GiB. Every measured container is constrained to the same
host CPU-affinity set inherited by the harness. The preflight rejects a Docker
or effective cgroup CPU set that differs from that recorded set.

One-second collectors cover the target, Redis, API service, k6, and host for
the measurement window. Collector audit files must show complete sampling,
zero failed attempts, sufficient duration, and bounded gaps. The target's
utilization is reported rather than rejected because target saturation is a
result; Redis, API, k6, and host p95 CPU and memory must each remain below 90%
of the recorded limit or the run is invalid.

Host network counters use the interface cohort present when measurement starts.
If Docker retires one of those interfaces during the run, its last counters are
frozen so the aggregate stays monotonic and the retirement is recorded in the
collector audit. Interfaces created after the baseline are not adopted, and a
counter reset on an interface that remains present still invalidates collection.

## Corrected run procedure

The C++ SDK comparison follows this sequence:

1. Pin and verify every source checkout, build the selected images once, and
   capture host, toolchain, source-tree, image, and Compose provenance.
2. Start the shared Redis and API services and generate a deterministic,
   seed-recorded counterbalanced order for three rounds. Only one MCP server is
   under load at a time.
3. Before each SDK/run pair, reset and re-seed Redis and run the standalone
   protocol and workload verifier. It checks the Redis counter, history, and
   popularity deltas around checkout; C++ adapters also run supplemental exact
   adapter validation.
4. Run a separate warmup invocation: ramp from 0 to 50 VUs for 15 seconds, then
   hold 50 VUs for 60 seconds. Warmup metrics are never merged into measured
   metrics. Ramp-down and graceful-stop time are both zero.
5. Reset and re-seed Redis again so the measurement starts from the canonical
   fixture state.
6. Start per-run resource collection and run exactly five minutes at a constant
   50 VUs. There is no measured ramp-up or ramp-down phase, and graceful-stop
   time is zero, so the load generator cannot extend the measurement boundary.
7. Stop resource collection immediately after k6 exits, re-seed, and verify the
   server again. Any eligibility, required supplemental, or correctness-threshold
   failure makes that run fail.
8. After all three counterbalanced rounds, select each SDK's median run by
   measured operations per second. Copy the resource samples and container/image
   evidence from that same run; do not combine resources from another run.

The counterbalanced schedule varies server position and neighboring servers
across rounds, reducing fixed-order thermal and cache bias. Its seed and exact
per-round order are retained with the result bundle.

Sessions unfinished at the hard time boundary are right-censored. An operation
is counted only after its HTTP response, shape check, contract check, and metric
recording complete; a response interrupted before that point is not counted.
An unfinished session contributes neither a completion result nor full-session
latency. Check totals may exceed the conservative operation counter if k6 stops
a VU in the tiny interval after its checks but before the counter update.
Started and completed session counts are reported separately so this boundary
behavior remains visible.

## Protocol and correctness gate

The client requests MCP revision `2024-11-05` during `initialize`. Each server
may select a supported revision; that selected revision is recorded in the
preflight, postflight, k6 summary, median-run metadata, and comparison table.
It must remain stable across all rounds for that server, and every subsequent
request uses the selected value in `MCP-Protocol-Version`.

The standalone verifier and local k6 profile use the same universal eligibility
contract for every target. They check:

- the negotiated `initialize` result;
- acceptance of `notifications/initialized`;
- exactly the three expected tools from `tools/list`, each with an input-schema
  object, followed by successful canonical calls;
- the pinned workload's observable predicates: search count/list sizes, the
  requested user's nonempty cart and five history entries, and a confirmed
  two-item checkout with a positive total and numeric rate-limit field;
- direct pre/post checkout Redis evidence that the rate counter, history length,
  and product popularity score each increase by one; and
- session deletion when the server issued a session ID.

The gate also validates JSON-RPC response identity, MCP response media type and
framing, session headers, and the required empty `202` response to the
initialized notification. Post-initialization requests include
`MCP-Protocol-Version` with the selected version and `Mcp-Session-Id` when
applicable. The k6 thresholds require a 100% check pass rate, zero HTTP request
failures, and zero MCP errors. Threshold evaluation aborts a load phase as soon
as a violation is observed because that phase is already invalid; it is never
eligible for a comparison table.

`adapter-exact-v1` is separate, out-of-band validation. It checks detailed input
schemas, deterministic rows and histories, exact checkout fields, and
server-type markers. Its outcome is recorded for every target and required for
the five authored C++ adapters. It is advisory for the pinned language
baselines, so schema precision or a mislabeled response field cannot silently
change the inherited workload eligibility. Because this harness counts only
completed `tools/call` and `tools/list` operations, its operations/s values must
not be compared directly with the upstream repository's RPS metric, which also
counted initialization requests.

## Reported metrics

- Operations per second: `tools/call` plus `tools/list`, excluding lifecycle
  messages from the headline rate
- Raw HTTP requests per second, including lifecycle traffic
- Combined tool-call p50/p90/p95/p99 and per-tool latency distributions
- Session success/failure counts and strict correctness/error rates
- Per-run Docker CPU, memory, and network samples
- Three-run sample coefficient of variation and, for the default three runs, a
  95% t-interval for mean operations rate

Percentiles come directly from the selected run's raw k6 trend. Percentiles are
not averaged or reconstructed from pre-aggregated per-tool quantiles.

## Result and provenance files

Results are written under `results/<timestamp>_<profile>/`; a baseline run uses
the explicit `baseline-diagnostic` suffix. Before any checkout or image build,
the harness stages read-only copies of the resolved runtime inputs under
`harness/`: `docker-compose.yml`, `benchmark.js`, and `SHA256SUMS`. Their
in-memory expected hashes are checked before Compose and k6 operations and once
more before completion.

The root bundle also contains `source_snapshot.tar`, `environment.json`,
`run_order.json`, `compose.resolved.yml`, `compose_images.json`, `build.log`, and
`run_manifest.json`. The manifest names the universal eligibility contract and
the targets for which supplemental adapter validation is required. The
deterministic source snapshot covers the local SDK,
adapters, and harness inputs; its digest is checked after builds and again
before aggregation and completion. Environment metadata records the local
worktree state, exact alternative and upstream commits/origins, host affinity,
tool versions, and pinned k6 image identity. Each server directory retains, for
every round:

- `warmup_summary_runN.json` and `warmup_console_runN.log`;
- `k6_summary_runN.json` and `k6_console_runN.log`;
- `protocol_preflight_runN.json` and `protocol_postflight_runN.json`, with
  eligibility checks, supplemental diagnostics, Redis side-effect observations,
  and matching `.log` files that preserve verifier output even when JSON
  evidence cannot be produced;
- `stats_runN.json`, `redis_stats_runN.json`, `api_stats_runN.json`,
  `k6_stats_runN.json`, and `host_stats_runN.json`, each paired with an
  `.audit.json` collector record. Host audits identify the initial, active,
  retired, and ignored-new interface sets under the recorded
  `baseline-cohort-retire-v1` policy;
- `resource_headroom_runN.json` with the integrity and headroom verdict;
- `container_inspect_runN.json`, `image_inspect_runN.json`,
  `cgroup_runN.json`, and `server_runN.log`; and
- matching k6 container, image, and cgroup evidence under `k6/`.

After median selection, `k6_summary.json`, `stats.json`,
`container_inspect.json`, and `image_inspect.json` are copies from the same
selected run. The selected protocol, cgroup, resource-headroom, collector-audit,
and k6 provenance records are paired the same way. `resource_summary.json`
summarizes only those selected resource samples. Its host network observation
is explicitly scoped to the baseline interface cohort and labels coverage
partial when an interface retires or a new interface is ignored.
`k6_multi_run_stats.json` records the three measured rates and selection
statistics.
Failed manifests also record the failing stage, exit code, final diagnostic,
and relative evidence-log path when the verifier identified the failure.

## Manual protocol check

The benchmark requests protocol revision `2024-11-05`. An initialize request
does not need a protocol-version header:

```bash
curl -i -sS -X POST http://localhost:8089/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{
    "jsonrpc":"2.0",
    "id":1,
    "method":"initialize",
    "params":{
      "protocolVersion":"2024-11-05",
      "clientInfo":{"name":"manual-check","version":"1.0"},
      "capabilities":{}
    }
  }'
```

Read the server-selected version from the initialize result. Use that value and
the returned `Mcp-Session-Id` on every later request:

```bash
curl -sS -X POST http://localhost:8089/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: <server-selected-version>' \
  -H 'Mcp-Session-Id: <session-id>' \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
```

## Teardown

```bash
# Stop and remove benchmark containers.
docker compose down

# Optional full cleanup, including images and volumes.
docker compose down --rmi all --volumes
```

See [RESULTS.md](RESULTS.md) for publication status. A reduced smoke test proves
that the harness works; it is not a performance result.
