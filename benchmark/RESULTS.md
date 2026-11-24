# Benchmark results

Two runs put the same MCP/Redis/HTTP workload on every target, on one machine:

- The **paced run** (`production-full`, 2026-11-15) gives every target the same
  offered load. Each of the 50 virtual users pauses 50 ms after every iteration
  of four operations, so the rate cannot exceed 4,000 operations per second. A
  target that keeps up shows how much latency, CPU and memory that load costs
  it.
- The **capacity run** (`capacity-full`, 2026-11-23) removes the pause. Each
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
pass the `adapter-exact-v1` adapter checks in preflight and postflight.

| | Paced run | Capacity run |
|---|---|---|
| Profile | `production-full` | `capacity-full` |
| Targets | 24 | 27 |
| Target CPUs | 2-CPU quota on any host CPU | CPUs 10 and 11; 22 and 23 idle |
| Pause per iteration | 50 ms | none |
| k6 limits | 4 CPUs, 2 GiB | 8 CPUs, 4 GiB |
| API service CPU limit | 2 CPUs | 8 CPUs |
| Redis CPU limit | 0.5 CPU | 1 CPU |
| Order seed | 1794759633 | 1795448281 |
| Commit | `a81d0b112052753f547c69f695121346a9be3739` | `ea807fcc2cf2a5e1e7d965d73fba062b58502f10` |

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
| This SDK, coroutines | stateless | 2024-11-05 | 19,407.45 | 1.14% | 1.49 | 3.05 | 3.90 | 0.093 | 190.16% | 8.66 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 16,717.41 | 1.87% | 1.28 | 3.57 | 4.99 | 0.110 | 192.14% | 40.84 MiB | 0 |
| This SDK, sync mode | stateless | 2024-11-05 | 16,325.85 | 1.30% | 1.26 | 3.72 | 5.11 | 0.101 | 170.14% | 37.71 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 17,098.69 | 0.89% | 1.78 | 3.34 | 4.96 | 0.106 | 192.96% | 15.00 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,684.24 | 1.05% | 7.14 | 17.24 | 22.90 | 0.508 | 191.51% | 16.70 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,085.23 | 0.26% | 28.98 | 45.67 | 82.49 | 1.851 | 202.55% | 302.80 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 13,654.14 | 0.69% | 2.28 | 3.47 | 4.41 | 0.142 | 202.46% | 9.62 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 12,117.48 | 1.27% | 1.78 | 3.49 | 4.89 | 0.160 | 200.09% | 37.54 MiB | 0 |
| This SDK, sync mode | stateful | 2024-11-05 | 12,219.74 | 0.48% | 1.78 | 3.73 | 5.36 | 0.128 | 162.56% | 36.92 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 12,851.85 | 0.20% | 1.91 | 3.45 | 4.99 | 0.146 | 197.45% | 23.47 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 4,274.62 | 0.07% | 4.99 | 11.33 | 15.07 | 0.442 | 192.94% | 16.82 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 25,211.46 | 0.61% | 2.09 | 3.70 | 4.70 | 0.074 | 196.14% | 9.05 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 20,911.50 | 0.29% | 2.26 | 4.67 | 5.91 | 0.088 | 188.87% | 37.29 MiB | 0 |
| This SDK, sync mode | sessionless | 2026-07-28 | 20,790.54 | 1.09% | 2.22 | 4.79 | 5.98 | 0.081 | 172.51% | 37.05 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 22,239.54 | 1.01% | 2.35 | 4.14 | 6.03 | 0.083 | 196.34% | 15.20 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 5,058.76 | 0.15% | 10.69 | 21.72 | 28.05 | 0.375 | 192.57% | 28.08 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 1,948.21 | 1.14% | 29.10 | 66.41 | 97.99 | 1.029 | 201.99% | 270.80 MiB | 0 |

The coroutines build reached 19,407.45 operations per second stateless (Rust:
17,098.69), 25,211.46 sessionless (Rust: 22,239.54) and 13,654.14 stateful
(Rust: 12,851.85). Its p99 stayed between 3.90 and 4.70 ms (Rust: 4.96 to
6.03 ms), and its target p95 memory between 8.66 and 9.62 MiB (Rust: 15.00 to
23.47 MiB). The threads and sync-mode builds reached 93.5% to 97.8% of Rust's
rate in each mode. The sync-mode build's target p95 CPU was 162.56% to 172.51%;
every other target in this group reached 188.87% or more.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 19,407.45 | 19,604.12 | 19,161.09 | 1 |
| This SDK, threads | stateless | 16,717.41 | 16,366.33 | 16,987.72 | 1 |
| This SDK, sync mode | stateless | 16,000.48 | 16,398.14 | 16,325.85 | 3 |
| Rust SDK | stateless | 17,098.69 | 17,148.63 | 16,863.45 | 1 |
| Go SDK | stateless | 3,631.15 | 3,706.28 | 3,684.24 | 3 |
| Python SDK | stateless | 1,085.23 | 1,084.81 | 1,089.99 | 1 |
| This SDK, coroutines | stateful | 13,500.75 | 13,672.89 | 13,654.14 | 3 |
| This SDK, threads | stateful | 12,139.74 | 12,117.48 | 11,863.94 | 2 |
| This SDK, sync mode | stateful | 12,219.74 | 12,310.46 | 12,200.14 | 1 |
| Rust SDK | stateful | 12,856.25 | 12,809.24 | 12,851.85 | 3 |
| Go SDK | stateful | 4,274.62 | 4,272.43 | 4,278.08 | 1 |
| This SDK, coroutines | sessionless | 24,956.17 | 25,211.46 | 25,231.74 | 2 |
| This SDK, threads | sessionless | 20,900.62 | 20,911.50 | 21,011.57 | 2 |
| This SDK, sync mode | sessionless | 20,480.27 | 20,790.54 | 20,919.48 | 2 |
| Rust SDK | sessionless | 22,074.44 | 22,520.64 | 22,239.54 | 3 |
| Go SDK | sessionless | 5,058.76 | 5,067.90 | 5,053.28 | 1 |
| Python SDK | sessionless | 1,984.87 | 1,948.21 | 1,944.67 | 2 |


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
scope](README.md#comparable-c-scope)). This SDK runs it in its threads build.
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
| hkr04/cpp-mcp | stateful | 2025-03-26 | 678.77 | 0.01% | 40.77 | 41.82 | 41.92 | 0.281 | 20.65% | 29.63 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,122.04 | 0.04% | 0.40 | 41.78 | 42.04 | 0.336 | 40.25% | 24.42 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 3,628.96 | 0.07% | 0.37 | 1.69 | 2.50 | 0.308 | 116.25% | 29.61 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 675.99 | 0.00% | 40.81 | 41.85 | 42.02 | 0.507 | 36.63% | 48.57 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 3,739.82 | 0.05% | 0.51 | 2.43 | 3.48 | 0.191 | 75.75% | 32.95 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 3,751.46 | 0.06% | 0.48 | 2.14 | 3.11 | 0.266 | 106.32% | 34.71 MiB | 0 |

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 3,541.27 | 3,597.60 | 3,595.64 | 3 |
| hkr04/cpp-mcp | stateful | 678.73 | 678.83 | 678.77 | 3 |
| FastMCPP | stateful | 1,122.35 | 1,122.04 | 1,121.46 | 2 |
| cxxmcp | stateful | 3,631.79 | 3,626.91 | 3,628.96 | 3 |
| Neumann-Labs/mcp-cpp | stateful | 675.95 | 675.99 | 676.01 | 2 |
| This SDK, threads | sessionless | 3,739.82 | 3,738.65 | 3,742.47 | 1 |
| cxxmcp | sessionless | 3,752.44 | 3,748.03 | 3,751.46 | 3 |

### Capacity run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | 2024-11-05 | 12,117.48 | 1.27% | 1.78 | 3.49 | 4.89 | 0.160 | 200.09% | 37.54 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 706.56 | 0.07% | 40.77 | 41.83 | 41.89 | 0.178 | 13.11% | 32.14 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,323.84 | 0.10% | 0.30 | 41.21 | 42.03 | 0.222 | 31.37% | 28.35 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | failed | - | - | - | - | - | - | - | - |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 706.88 | 0.09% | 40.80 | 41.85 | 41.90 | 0.339 | 25.11% | 53.03 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 20,911.50 | 0.29% | 2.26 | 4.67 | 5.91 | 0.088 | 188.87% | 37.29 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | failed | - | - | - | - | - | - | - | - |
cxxmcp's server exited on signal 11 (a segmentation fault) in round 2 of both
of its cells, so the run records those cells as failed: it reports no median
for them and skipped their third round. The rates of the rounds they completed
are below.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 12,139.74 | 12,117.48 | 11,863.94 | 2 |
| hkr04/cpp-mcp | stateful | 706.56 | 706.75 | 705.75 | 1 |
| FastMCPP | stateful | 1,325.07 | 1,322.51 | 1,323.84 | 3 |
| cxxmcp | stateful | 10,626.51 | failed (measurement) | skipped | - |
| Neumann-Labs/mcp-cpp | stateful | 707.75 | 706.49 | 706.88 | 3 |
| This SDK, threads | sessionless | 20,900.62 | 20,911.50 | 21,011.57 | 2 |
| cxxmcp | sessionless | 14,385.05 | failed (warmup) | skipped | - |


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
| This SDK, threads | stateful | default | 2024-11-05 | 12,117.48 | 1.27% | 1.78 | 3.49 | 4.89 | 0.160 | 200.09% | 37.54 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 20,911.50 | 0.29% | 2.26 | 4.67 | 5.91 | 0.088 | 188.87% | 37.29 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 706.56 | 0.07% | 40.77 | 41.83 | 41.89 | 0.178 | 13.11% | 32.14 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | 13,526.16 | 0.56% | 1.40 | 3.78 | 6.73 | 0.145 | 204.68% | 37.51 MiB | 0 |
| FastMCPP | stateful | default | 2024-11-05 | 1,323.84 | 0.10% | 0.30 | 41.21 | 42.03 | 0.222 | 31.37% | 28.35 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | 2024-11-05 | 10,554.36 | 2.10% | 0.74 | 1.55 | 2.12 | 0.184 | 201.28% | 35.38 MiB | 0 |
| cxxmcp | stateful | default | 2024-11-05 | failed | - | - | - | - | - | - | - | - |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | failed | - | - | - | - | - | - | - | - |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 706.88 | 0.09% | 40.80 | 41.85 | 41.90 | 0.339 | 25.11% | 53.03 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | failed | - | - | - | - | - | - | - | - |
| cxxmcp | sessionless | default | 2026-07-28 | failed | - | - | - | - | - | - | - | - |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | failed | - | - | - | - | - | - | - | - |
The TCP_NODELAY builds of cxxmcp and of Neumann-Labs/mcp-cpp also exited on
signal 11 in round 2, so those cells failed in the same way.

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 12,139.74 | 12,117.48 | 11,863.94 | 2 |
| This SDK, threads | sessionless | default | 20,900.62 | 20,911.50 | 21,011.57 | 2 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 13,526.16 | 13,563.15 | 13,416.80 | 1 |
| FastMCPP | stateful | TCP_NODELAY | 10,554.36 | 10,263.78 | 10,697.38 | 1 |
| cxxmcp | stateful | TCP_NODELAY | 10,612.51 | failed (warmup) | skipped | - |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 6,377.24 | failed (measurement) | skipped | - |
| cxxmcp | sessionless | TCP_NODELAY | 14,262.71 | failed (measurement) | skipped | - |


## Audit evidence

The paced run completed with `publishable_candidate` set and no failed cells. It
passed:

- 144/144 protocol gates (preflight and postflight for 72 measured rounds) under
  the universal contract, with the required `adapter-exact-v1` checks passing
  in all 96 gates of the 16 C++ targets;
- 72/72 warmups and 72/72 measurements with zero MCP, HTTP, or check failures;
- 360/360 collector audits and 72/72 per-round resource/headroom reports.

The capacity run completed with `publishable_candidate` set and the five failed
cells described above. It passed:

- 147/147 protocol gates (76 preflight, 71 postflight) under the universal
  contract, with the required `adapter-exact-v1` checks passing in all 99 gates
  of the 19 C++ targets;
- 74 of 76 warmups and 71 of 74 measurements with zero MCP, HTTP, or check
  failures; the two other warmups and three other measurements are those of the
  failed cells;
- 370/370 collector audits and 71/71 per-round resource/headroom reports.

Both runs used the `baseline-cohort-retire-v1` host network policy and measured
a clean working tree.
