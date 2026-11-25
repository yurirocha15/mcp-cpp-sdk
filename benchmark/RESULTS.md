# Benchmark results

Two runs put the same MCP/Redis/HTTP workload on every target, on one machine:

- The **paced run** (`production-full`, 2026-11-15) gives every target the same
  offered load. Each of the 50 virtual users pauses 50 ms after every iteration
  of four operations, so the rate cannot exceed 4,000 operations per second. A
  target that keeps up shows how much latency, CPU and memory that load costs
  it.
- The **capacity run** (`capacity-full`, 2026-11-24) removes the pause. Each
  target then runs as fast as its 2-CPU limit allows, which shows its maximum
  throughput and its latency at that point.

The two runs used different load models and are never combined in one table.
They also measured different commits, and only the capacity run gave each
target two CPUs of its own (see [Run configuration](#run-configuration)).
Each section below shows the paced table first and the capacity table second.
[Audit evidence](#audit-evidence) at the end lists what each run passed.

## Run configuration

The paced run measured 24 targets and the capacity run 27: the same 24 and this
SDK's sync-mode build. Each run measured its targets in three counterbalanced
rounds. Each round used 50 VUs, a separate 15-second ramp plus 60-second warmup,
and a five-minute constant-load measurement for every target. Every target
container had the same 2-CPU and 2-GiB limits. In the capacity run each target
also ran on two CPUs of its own, on separate physical cores, with the other
hardware threads of those cores left idle (see [Concurrency and resource
controls](README.md#concurrency-and-resource-controls)); in the paced run its
2-CPU limit applied across all host CPUs. Every row used the shared
`upstream-v2-strict-mcp-v1` measured contract, and every C++ target also had to
pass the `adapter-exact-v1` adapter checks in preflight and postflight. The
paced run built the C++ targets on Ubuntu 24.04 and the capacity run on Ubuntu
26.04 (see [Comparable C++ scope](README.md#comparable-c-scope)).

| | Paced run | Capacity run |
|---|---|---|
| Profile | `production-full` | `capacity-full` |
| Targets | 24 | 27 |
| Target CPUs | 2-CPU quota on any host CPU | CPUs 10 and 11; 22 and 23 idle |
| Pause per iteration | 50 ms | none |
| k6 limits | 4 CPUs, 2 GiB | 8 CPUs, 4 GiB |
| API service CPU limit | 2 CPUs | 8 CPUs |
| Redis CPU limit | 0.5 CPU | 1 CPU |
| Order seed | 1794759633 | 1795534930 |
| C++ target image base | Ubuntu 24.04 | Ubuntu 26.04 |
| Commit | `a81d0b112052753f547c69f695121346a9be3739` | `2601793d051331274d25ee4f5a61ab08195f501d` |

Every target, k6, Redis, and the API service ran on one machine: an AMD Ryzen 9
9900X (12 cores, 24 threads) with 30.4 GiB of memory, on Ubuntu 24.04 with Linux
7.0.0 and Docker 29.9.2, using the `powersave` CPU governor. Absolute numbers
will differ on other hardware and between runs. Sample CV shows how much the
three rounds of a run varied.

Operations are `tools/call` and `tools/list` requests; lifecycle messages are
not counted. Latency is the combined tool-call latency of the selected round.
CPU follows Docker's convention: 100% is one fully used core. Resource values
are from the round selected for that target's median throughput. CPU ms/op is
the target's mean CPU in that round divided by its operation rate. Target
saturation is reported but not gated. Headroom gates apply to Redis, the API
service, k6, and the host, and every one of them passed in every completed
round.

These numbers describe this pinned MCP/Redis/HTTP workload and environment; they
are not a general-purpose application-performance claim.

## This SDK and the official Rust, Go and Python SDKs

The Rust, Go and Python targets are the pinned upstream benchmark's server
applications, ported to the official SDKs: Rust SDK (`rmcp` 3.8.0), Go
SDK (`go-sdk` v1.8.0) and Python SDK (`mcp` 2.3.0). See [Language
baselines](README.md#language-baselines). Each is its own application with its
own Redis and HTTP clients, so a difference between rows includes the
application code as well as the SDK.

This SDK appears in three builds; the paced run has the first two. The
coroutines build runs its tool handlers as C++20 coroutines over asynchronous
Redis and HTTP clients, without a handler thread pool. The threads build runs
the shared, synchronous C++ workload that the [C++ SDKs](#c-sdks) section uses.
See [Coroutine workload](README.md#coroutine-workload). The sync-mode build runs
the same shared workload as a sync mode program. See [Sync mode
workload](README.md#sync-mode-workload).

Stateless and stateful rows use protocol `2024-11-05`. Sessionless rows use
`2026-07-28`, which has no sessions. Python has no stateful row (see [Session
modes](README.md#session-modes)).

### Paced run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 2024-11-05 | 3,687.44 | 0.11% | 0.37 | 1.79 | 2.51 | 0.138 | 53.47% | 8.22 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 3,680.38 | 0.05% | 0.36 | 1.85 | 2.66 | 0.197 | 75.53% | 32.08 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 3,671.84 | 0.03% | 0.45 | 1.79 | 2.56 | 0.153 | 59.50% | 14.03 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,085.04 | 0.10% | 1.62 | 4.92 | 7.12 | 0.569 | 184.60% | 14.37 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,060.32 | 0.33% | 15.25 | 64.95 | 80.11 | 1.891 | 204.00% | 270.20 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 3,548.51 | 0.70% | 0.51 | 2.09 | 3.00 | 0.230 | 87.93% | 9.07 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 3,595.64 | 0.89% | 0.43 | 1.63 | 2.44 | 0.284 | 105.96% | 32.23 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 3,596.79 | 0.02% | 0.48 | 1.71 | 2.45 | 0.197 | 74.32% | 13.92 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 3,217.48 | 0.46% | 1.12 | 3.43 | 4.94 | 0.512 | 172.28% | 13.84 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 3,730.68 | 0.08% | 0.57 | 2.49 | 3.66 | 0.123 | 49.55% | 8.69 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 3,739.82 | 0.05% | 0.51 | 2.43 | 3.48 | 0.191 | 75.75% | 32.95 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 3,735.59 | 0.07% | 0.60 | 2.28 | 3.28 | 0.129 | 52.25% | 13.95 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 3,431.73 | 0.04% | 1.51 | 5.76 | 8.46 | 0.432 | 154.79% | 14.05 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 1,964.00 | 0.10% | 14.36 | 34.69 | 62.94 | 1.021 | 203.14% | 269.20 MiB | 0 |

In every mode, both builds of this SDK and Rust reached rates within 1.4% of
each other without reaching their CPU limit. The coroutines build used 0.138 ms
of CPU per operation stateless (Rust: 0.153), 0.123 ms sessionless (Rust: 0.129)
and 0.230 ms stateful (Rust: 0.197). Its target p95 memory was 8.22 to
9.07 MiB, against 13.92 to 14.03 MiB for Rust.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 3,691.46 | 3,683.63 | 3,687.44 | 3 |
| This SDK, threads | stateless | 3,681.38 | 3,680.38 | 3,677.55 | 2 |
| Rust SDK | stateless | 3,672.84 | 3,671.84 | 3,670.44 | 2 |
| Go SDK | stateless | 3,081.10 | 3,086.86 | 3,085.04 | 3 |
| Python SDK | stateless | 1,060.32 | 1,061.70 | 1,055.04 | 1 |
| This SDK, coroutines | stateful | 3,543.86 | 3,548.51 | 3,589.36 | 2 |
| This SDK, threads | stateful | 3,541.27 | 3,597.60 | 3,595.64 | 3 |
| Rust SDK | stateful | 3,596.79 | 3,598.13 | 3,596.55 | 1 |
| Go SDK | stateful | 3,240.04 | 3,217.48 | 3,212.48 | 2 |
| This SDK, coroutines | sessionless | 3,730.68 | 3,727.83 | 3,733.98 | 1 |
| This SDK, threads | sessionless | 3,739.82 | 3,738.65 | 3,742.47 | 1 |
| Rust SDK | sessionless | 3,733.36 | 3,735.59 | 3,738.23 | 2 |
| Go SDK | sessionless | 3,433.64 | 3,431.73 | 3,431.00 | 2 |
| Python SDK | sessionless | 1,964.00 | 1,967.31 | 1,963.96 | 1 |

### Capacity run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 2024-11-05 | 19,794.09 | 6.39% | 1.34 | 3.03 | 3.84 | 0.087 | 182.80% | 9.18 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 18,349.61 | 0.13% | 1.11 | 3.24 | 4.50 | 0.101 | 192.90% | 32.05 MiB | 0 |
| This SDK, sync mode | stateless | 2024-11-05 | 17,675.10 | 0.93% | 1.14 | 3.33 | 4.62 | 0.093 | 170.66% | 30.22 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 16,869.17 | 0.78% | 1.81 | 3.35 | 4.93 | 0.109 | 194.57% | 15.03 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,695.13 | 0.33% | 7.12 | 17.21 | 22.84 | 0.507 | 191.27% | 25.71 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,089.37 | 1.29% | 29.57 | 42.83 | 80.60 | 1.843 | 202.45% | 270.50 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 14,407.06 | 0.70% | 2.06 | 3.33 | 4.37 | 0.133 | 200.65% | 10.29 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 12,856.24 | 0.85% | 1.62 | 3.33 | 4.66 | 0.151 | 201.33% | 33.70 MiB | 0 |
| This SDK, sync mode | stateful | 2024-11-05 | 13,304.01 | 0.22% | 1.59 | 3.36 | 4.70 | 0.118 | 162.33% | 29.65 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 12,895.22 | 0.24% | 1.91 | 3.46 | 5.05 | 0.145 | 197.41% | 23.55 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 4,279.85 | 0.49% | 5.00 | 11.33 | 15.10 | 0.443 | 192.95% | 26.87 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 26,282.43 | 0.36% | 1.96 | 3.64 | 4.61 | 0.069 | 193.17% | 9.53 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 22,188.79 | 3.54% | 2.14 | 4.48 | 5.77 | 0.084 | 192.02% | 32.25 MiB | 0 |
| This SDK, sync mode | sessionless | 2026-07-28 | 23,004.23 | 0.26% | 2.01 | 4.31 | 5.58 | 0.074 | 173.61% | 30.42 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 22,564.93 | 1.23% | 2.32 | 4.08 | 5.89 | 0.082 | 195.61% | 15.66 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 5,062.42 | 0.17% | 10.66 | 21.76 | 28.14 | 0.375 | 192.76% | 17.79 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 1,985.07 | 1.20% | 21.93 | 96.06 | 164.41 | 1.010 | 202.14% | 288.00 MiB | 0 |

The coroutines build reached 19,794.09 operations per second stateless (Rust:
16,869.17), 26,282.43 sessionless (Rust: 22,564.93) and 14,407.06 stateful
(Rust: 12,895.22). Its p99 stayed between 3.84 and 4.61 ms (Rust: 4.93 to
5.89 ms), and its target p95 memory between 9.18 and 10.29 MiB (Rust: 15.03 to
23.55 MiB). The threads build reached 98.3% to 108.8% of Rust's rate in each
mode, and the sync-mode build 101.9% to 104.8%. The sync-mode build's target
p95 CPU was 162.33% to 173.61%; every other target in this group reached
182.80% or more.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 17,780.93 | 19,997.91 | 19,794.09 | 3 |
| This SDK, threads | stateless | 18,349.61 | 18,310.07 | 18,353.94 | 1 |
| This SDK, sync mode | stateless | 17,913.09 | 17,675.10 | 17,596.17 | 2 |
| Rust SDK | stateless | 16,869.17 | 17,087.87 | 16,851.49 | 1 |
| Go SDK | stateless | 3,695.13 | 3,711.62 | 3,687.80 | 1 |
| Python SDK | stateless | 1,066.50 | 1,089.37 | 1,091.82 | 2 |
| This SDK, coroutines | stateful | 14,407.06 | 14,272.32 | 14,468.17 | 1 |
| This SDK, threads | stateful | 12,813.86 | 13,020.34 | 12,856.24 | 3 |
| This SDK, sync mode | stateful | 13,304.01 | 13,272.60 | 13,332.10 | 1 |
| Rust SDK | stateful | 12,850.69 | 12,911.48 | 12,895.22 | 3 |
| Go SDK | stateful | 4,279.85 | 4,257.11 | 4,298.63 | 1 |
| This SDK, coroutines | sessionless | 26,282.43 | 26,245.72 | 26,423.75 | 1 |
| This SDK, threads | sessionless | 23,192.91 | 21,632.52 | 22,188.79 | 3 |
| This SDK, sync mode | sessionless | 22,913.27 | 23,026.24 | 23,004.23 | 3 |
| Rust SDK | sessionless | 22,564.93 | 22,103.18 | 22,595.67 | 1 |
| Go SDK | sessionless | 5,062.42 | 5,053.87 | 5,071.55 | 1 |
| Python SDK | sessionless | 1,985.07 | 2,008.49 | 1,960.92 | 1 |


### Supplemental checks

All three language SDKs passed every eligibility gate in both runs, including
the direct Redis side-effect checks. The `adapter-exact-v1` checks are advisory
for them. Rust passed them. Go and Python failed one check in every gate of both
runs: the input schema each advertises for the `checkout` tool differs from the
exact schema the C++ adapters advertise. Their search, cart and checkout results
matched the exact fixtures.

## C++ SDKs

These targets are adapters that call one shared, synchronous application
workload through each SDK's public API (see [Comparable C++
scope](README.md#comparable-c-scope)). This SDK runs it in its threads and
sync-mode builds. The tables also list this SDK's coroutines build, whose
handlers suspend while they wait for Redis and the API service instead of
holding a thread (see [Coroutine workload](README.md#coroutine-workload)). The
other four SDKs take a tool handler as a function that returns its result, so
each of their handlers holds a thread until the result is ready. The
coroutines build reaches the services through its own asio clients rather
than libcurl and hiredis, so a difference between its rows and another SDK's
includes those clients as well as the SDK.
hkr04/cpp-mcp, FastMCPP and Neumann-Labs/mcp-cpp only serve sessions and answer
`initialize` with the revision they choose, so the stateful rows do not share
one revision. cxxmcp is the only other SDK here that serves `2026-07-28`.

hkr04/cpp-mcp, FastMCPP, cxxmcp, and Neumann-Labs/mcp-cpp leave `TCP_NODELAY`
off. The same adapters built with it on are in [TCP_NODELAY
builds](#tcp_nodelay-builds).

### Paced run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | 2024-11-05 | 3,595.64 | 0.89% | 0.43 | 1.63 | 2.44 | 0.284 | 105.96% | 32.23 MiB | 0 |
| This SDK, coroutines (async workload) | stateful | 2024-11-05 | 3,548.51 | 0.70% | 0.51 | 2.09 | 3.00 | 0.230 | 87.93% | 9.07 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 678.77 | 0.01% | 40.77 | 41.82 | 41.92 | 0.281 | 20.65% | 29.63 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,122.04 | 0.04% | 0.40 | 41.78 | 42.04 | 0.336 | 40.25% | 24.42 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 3,628.96 | 0.07% | 0.37 | 1.69 | 2.50 | 0.308 | 116.25% | 29.61 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 675.99 | 0.00% | 40.81 | 41.85 | 42.02 | 0.507 | 36.63% | 48.57 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 3,739.82 | 0.05% | 0.51 | 2.43 | 3.48 | 0.191 | 75.75% | 32.95 MiB | 0 |
| This SDK, coroutines (async workload) | sessionless | 2026-07-28 | 3,730.68 | 0.08% | 0.57 | 2.49 | 3.66 | 0.123 | 49.55% | 8.69 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 3,751.46 | 0.06% | 0.48 | 2.14 | 3.11 | 0.266 | 106.32% | 34.71 MiB | 0 |

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 3,541.27 | 3,597.60 | 3,595.64 | 3 |
| This SDK, coroutines (async workload) | stateful | 3,543.86 | 3,548.51 | 3,589.36 | 2 |
| hkr04/cpp-mcp | stateful | 678.73 | 678.83 | 678.77 | 3 |
| FastMCPP | stateful | 1,122.35 | 1,122.04 | 1,121.46 | 2 |
| cxxmcp | stateful | 3,631.79 | 3,626.91 | 3,628.96 | 3 |
| Neumann-Labs/mcp-cpp | stateful | 675.95 | 675.99 | 676.01 | 2 |
| This SDK, threads | sessionless | 3,739.82 | 3,738.65 | 3,742.47 | 1 |
| This SDK, coroutines (async workload) | sessionless | 3,730.68 | 3,727.83 | 3,733.98 | 1 |
| cxxmcp | sessionless | 3,752.44 | 3,748.03 | 3,751.46 | 3 |

### Capacity run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | 2024-11-05 | 12,856.24 | 0.85% | 1.62 | 3.33 | 4.66 | 0.151 | 201.33% | 33.70 MiB | 0 |
| This SDK, coroutines (async workload) | stateful | 2024-11-05 | 14,407.06 | 0.70% | 2.06 | 3.33 | 4.37 | 0.133 | 200.65% | 10.29 MiB | 0 |
| This SDK, sync mode | stateful | 2024-11-05 | 13,304.01 | 0.22% | 1.59 | 3.36 | 4.70 | 0.118 | 162.33% | 29.65 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 706.01 | 0.06% | 40.78 | 41.84 | 41.89 | 0.169 | 12.51% | 22.50 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,324.10 | 0.10% | 0.29 | 41.18 | 42.02 | 0.206 | 28.91% | 24.35 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 11,293.79 | 0.38% | 2.30 | 3.83 | 4.85 | 0.170 | 196.88% | 36.79 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 706.72 | 0.07% | 40.80 | 41.85 | 41.90 | 0.314 | 23.12% | 48.01 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 22,188.79 | 3.54% | 2.14 | 4.48 | 5.77 | 0.084 | 192.02% | 32.25 MiB | 0 |
| This SDK, coroutines (async workload) | sessionless | 2026-07-28 | 26,282.43 | 0.36% | 1.96 | 3.64 | 4.61 | 0.069 | 193.17% | 9.53 MiB | 0 |
| This SDK, sync mode | sessionless | 2026-07-28 | 23,004.23 | 0.26% | 2.01 | 4.31 | 5.58 | 0.074 | 173.61% | 30.42 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 15,756.32 | 4.58% | 3.03 | 5.70 | 7.49 | 0.122 | 195.91% | 42.36 MiB | 0 |

This SDK's threads build reached 12,856.24 operations per second stateful and
22,188.79 sessionless, its coroutines build 14,407.06 and 26,282.43, and its
sync-mode build 13,304.01 and 23,004.23. cxxmcp, the fastest of the other SDKs
in their default builds, reached 11,293.79 and 15,756.32. The default builds
of hkr04/cpp-mcp, FastMCPP and Neumann-Labs/mcp-cpp reached 706.01 to
1,324.10; [TCP_NODELAY builds](#tcp_nodelay-builds) has their builds with
`TCP_NODELAY` on.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 12,813.86 | 13,020.34 | 12,856.24 | 3 |
| This SDK, coroutines (async workload) | stateful | 14,407.06 | 14,272.32 | 14,468.17 | 1 |
| This SDK, sync mode | stateful | 13,304.01 | 13,272.60 | 13,332.10 | 1 |
| hkr04/cpp-mcp | stateful | 705.98 | 706.72 | 706.01 | 3 |
| FastMCPP | stateful | 1,324.81 | 1,324.10 | 1,322.24 | 2 |
| cxxmcp | stateful | 11,249.86 | 11,335.77 | 11,293.79 | 3 |
| Neumann-Labs/mcp-cpp | stateful | 706.61 | 706.72 | 707.47 | 2 |
| This SDK, threads | sessionless | 23,192.91 | 21,632.52 | 22,188.79 | 3 |
| This SDK, coroutines (async workload) | sessionless | 26,282.43 | 26,245.72 | 26,423.75 | 1 |
| This SDK, sync mode | sessionless | 22,913.27 | 23,026.24 | 23,004.23 | 3 |
| cxxmcp | sessionless | 14,562.89 | 15,807.23 | 15,756.32 | 3 |


## TCP_NODELAY builds

Each `-nodelay` target is the same adapter as the default build, compiled with
`CPPHTTPLIB_TCP_NODELAY=true` (see the [README](README.md#tcp_nodelay-builds)).
Each pair below comes from the same run. Each table starts with this SDK's
threads build from that run, over the same shared workload; it has no separate
TCP_NODELAY build.

### Paced run

| SDK | Mode | Build | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 2024-11-05 | 3,595.64 | 0.89% | 0.43 | 1.63 | 2.44 | 0.284 | 105.96% | 32.23 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 3,739.82 | 0.05% | 0.51 | 2.43 | 3.48 | 0.191 | 75.75% | 32.95 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 678.77 | 0.01% | 40.77 | 41.82 | 41.92 | 0.281 | 20.65% | 29.63 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | 3,630.37 | 0.14% | 0.33 | 1.51 | 2.28 | 0.232 | 87.50% | 23.90 MiB | 0 |
| FastMCPP | stateful | default | 2024-11-05 | 1,122.04 | 0.04% | 0.40 | 41.78 | 42.04 | 0.336 | 40.25% | 24.42 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | 2024-11-05 | 3,525.12 | 0.26% | 0.38 | 1.50 | 2.28 | 0.296 | 109.20% | 25.16 MiB | 0 |
| cxxmcp | stateful | default | 2024-11-05 | 3,628.96 | 0.07% | 0.37 | 1.69 | 2.50 | 0.308 | 116.25% | 29.61 MiB | 0 |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | 3,627.81 | 0.05% | 0.37 | 1.66 | 2.46 | 0.305 | 115.95% | 30.54 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 675.99 | 0.00% | 40.81 | 41.85 | 42.02 | 0.507 | 36.63% | 48.57 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | 3,554.26 | 0.18% | 0.38 | 1.65 | 2.54 | 0.506 | 189.01% | 39.10 MiB | 0 |
| cxxmcp | sessionless | default | 2026-07-28 | 3,751.46 | 0.06% | 0.48 | 2.14 | 3.11 | 0.266 | 106.32% | 34.71 MiB | 0 |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | 3,751.96 | 0.02% | 0.48 | 2.17 | 3.13 | 0.261 | 106.56% | 34.78 MiB | 0 |

With `TCP_NODELAY` on, the p50 of hkr04/cpp-mcp fell from 40.77 ms to 0.33 ms,
and that of Neumann-Labs/mcp-cpp from 40.81 ms to 0.38 ms. FastMCPP's p95 fell
from 41.78 ms to 1.50 ms. cxxmcp's median throughput changed by less than 0.1%.

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 3,541.27 | 3,597.60 | 3,595.64 | 3 |
| This SDK, threads | sessionless | default | 3,739.82 | 3,738.65 | 3,742.47 | 1 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 3,630.37 | 3,630.67 | 3,621.68 | 1 |
| FastMCPP | stateful | TCP_NODELAY | 3,525.12 | 3,539.90 | 3,522.86 | 1 |
| cxxmcp | stateful | TCP_NODELAY | 3,630.72 | 3,627.81 | 3,627.19 | 2 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 3,554.26 | 3,561.41 | 3,548.98 | 1 |
| cxxmcp | sessionless | TCP_NODELAY | 3,751.87 | 3,752.94 | 3,751.96 | 3 |

### Capacity run

| SDK | Mode | Build | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 2024-11-05 | 12,856.24 | 0.85% | 1.62 | 3.33 | 4.66 | 0.151 | 201.33% | 33.70 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 22,188.79 | 3.54% | 2.14 | 4.48 | 5.77 | 0.084 | 192.02% | 32.25 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 706.01 | 0.06% | 40.78 | 41.84 | 41.89 | 0.169 | 12.51% | 22.50 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | failed | - | - | - | - | - | - | - | - |
| FastMCPP | stateful | default | 2024-11-05 | 1,324.10 | 0.10% | 0.29 | 41.18 | 42.02 | 0.206 | 28.91% | 24.35 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | 2024-11-05 | 11,230.83 | 0.27% | 0.71 | 1.53 | 2.12 | 0.172 | 199.78% | 24.90 MiB | 0 |
| cxxmcp | stateful | default | 2024-11-05 | 11,293.79 | 0.38% | 2.30 | 3.83 | 4.85 | 0.170 | 196.88% | 36.79 MiB | 0 |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | 11,278.86 | 0.38% | 2.31 | 3.87 | 4.92 | 0.169 | 196.84% | 36.80 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 706.72 | 0.07% | 40.80 | 41.85 | 41.90 | 0.314 | 23.12% | 48.01 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | 7,160.30 | 0.48% | 1.45 | 5.14 | 7.37 | 0.269 | 196.35% | 56.79 MiB | 0 |
| cxxmcp | sessionless | default | 2026-07-28 | 15,756.32 | 4.58% | 3.03 | 5.70 | 7.49 | 0.122 | 195.91% | 42.36 MiB | 0 |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | 15,846.74 | 0.47% | 3.02 | 5.71 | 7.52 | 0.122 | 196.18% | 43.23 MiB | 0 |

In round 3, hkr04/cpp-mcp's TCP_NODELAY build did not answer one session
`DELETE` of 2,278,253 sessions with a 2xx or 405 status, so k6 stopped that
round and the run records the cell as failed: it reports no median for it.
The rates of its first two rounds are below.

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 12,813.86 | 13,020.34 | 12,856.24 | 3 |
| This SDK, threads | sessionless | default | 23,192.91 | 21,632.52 | 22,188.79 | 3 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 14,217.90 | 14,269.21 | failed (measurement) | - |
| FastMCPP | stateful | TCP_NODELAY | 11,272.40 | 11,230.83 | 11,214.63 | 2 |
| cxxmcp | stateful | TCP_NODELAY | 11,306.98 | 11,278.86 | 11,222.61 | 2 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 7,160.30 | 7,126.25 | 7,194.52 | 1 |
| cxxmcp | sessionless | TCP_NODELAY | 15,856.44 | 15,846.74 | 15,722.45 | 2 |


## Audit evidence

The paced run completed with `publishable_candidate` set and no failed cells. It
passed:

- 144/144 protocol gates (preflight and postflight for 72 measured rounds) under
  the universal contract, with the required `adapter-exact-v1` checks passing
  in all 96 gates of the 16 C++ targets;
- 72/72 warmups and 72/72 measurements with zero MCP, HTTP, or check failures;
- 360/360 collector audits and 72/72 per-round resource/headroom reports.

The capacity run completed with `publishable_candidate` set and the one failed
cell described above. It passed:

- 161/161 protocol gates (81 preflight, 80 postflight) under the universal
  contract, with the required `adapter-exact-v1` checks passing in all 113
  gates of the 19 C++ targets;
- 81/81 warmups and 80 of 81 measurements with zero MCP, HTTP, or check
  failures; the other measurement is the failed round;
- 405/405 collector audits and 80/80 per-round resource/headroom reports.

Both runs used the `baseline-cohort-retire-v1` host network policy and measured
a clean working tree.
