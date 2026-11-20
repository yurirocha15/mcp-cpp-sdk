# Benchmark results

Two runs put the same MCP/Redis/HTTP workload on every target, on one machine:

- The **paced run** (`production-full`, 2026-11-15) gives every target the same
  offered load. Each of the 50 virtual users pauses 50 ms after every iteration
  of four operations, so the rate cannot exceed 4,000 operations per second. A
  target that keeps up shows how much latency, CPU and memory that load costs
  it.
- The **capacity run** (`capacity-full`, 2026-11-16) removes the pause. Each
  target then runs as fast as its 2-CPU limit allows, which shows its maximum
  throughput and its latency at that point.

The two runs used different load models and are never combined in one table.
Each section below shows the paced table first and the capacity table second.
[Audit evidence](#audit-evidence) at the end lists what each run passed.

## Run configuration

Each run measured 24 targets in three counterbalanced rounds. Each round used 50
VUs, a separate 15-second ramp plus 60-second warmup, and a five-minute
constant-load measurement for every target. Every target container had the same
2-CPU and 2-GiB limits. Every row used the shared `upstream-v2-strict-mcp-v1`
measured contract, and every C++ target also had to pass the `adapter-exact-v1`
adapter checks in preflight and postflight.

| | Paced run | Capacity run |
|---|---|---|
| Profile | `production-full` | `capacity-full` |
| Pause per iteration | 50 ms | none |
| k6 limits | 4 CPUs, 2 GiB | 8 CPUs, 4 GiB |
| API service CPU limit | 2 CPUs | 8 CPUs |
| Redis CPU limit | 0.5 CPU | 1 CPU |
| Order seed | 1794759633 | 1794787977 |
| Commit | `a81d0b112052753f547c69f695121346a9be3739` | `a81d0b112052753f547c69f695121346a9be3739` |

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
service, k6, and the host, and every one of them passed.

These numbers describe this pinned MCP/Redis/HTTP workload and environment; they
are not a general-purpose application-performance claim.

## This SDK and the official Rust, Go and Python SDKs

The Rust, Go and Python targets are the pinned upstream benchmark's server
applications, ported to the official SDKs: Rust SDK (`rmcp` 3.8.0), Go
SDK (`go-sdk` v1.8.0) and Python SDK (`mcp` 2.3.0). See [Language
baselines](README.md#language-baselines). Each is its own application with its
own Redis and HTTP clients, so a difference between rows includes the
application code as well as the SDK.

This SDK appears in two builds. The coroutines build runs its tool handlers as
C++20 coroutines over asynchronous Redis and HTTP clients, without a handler
thread pool. The threads build runs the shared, synchronous C++ workload that
the [C++ SDKs](#c-sdks) section uses. See [Coroutine
workload](README.md#coroutine-workload).

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
| This SDK, coroutines | stateless | 2024-11-05 | 14,089.99 | 0.21% | 2.35 | 4.45 | 5.05 | 0.139 | 201.53% | 8.71 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 9,175.98 | 0.31% | 1.07 | 3.19 | 50.66 | 0.220 | 206.17% | 34.68 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 13,122.55 | 0.26% | 2.51 | 4.27 | 4.98 | 0.149 | 200.49% | 14.61 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,271.00 | 0.27% | 7.91 | 17.42 | 23.71 | 0.597 | 197.89% | 15.95 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,033.50 | 0.39% | 16.73 | 70.87 | 97.85 | 1.935 | 202.82% | 273.80 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 8,885.39 | 0.27% | 3.01 | 5.41 | 6.05 | 0.222 | 202.02% | 9.99 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 7,055.55 | 0.12% | 1.70 | 3.28 | 39.83 | 0.286 | 205.89% | 35.66 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 9,698.48 | 0.31% | 2.54 | 4.27 | 5.11 | 0.201 | 200.61% | 20.82 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 3,628.64 | 0.48% | 6.06 | 12.38 | 16.72 | 0.542 | 198.92% | 16.64 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 18,651.40 | 0.07% | 2.87 | 5.13 | 5.75 | 0.105 | 200.31% | 9.66 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 10,985.22 | 1.08% | 1.84 | 51.57 | 59.60 | 0.183 | 205.51% | 34.43 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 18,020.10 | 0.30% | 3.02 | 5.11 | 6.04 | 0.109 | 200.47% | 16.44 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 4,430.97 | 0.28% | 12.81 | 21.26 | 25.68 | 0.442 | 198.45% | 17.56 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 1,843.28 | 0.24% | 18.05 | 81.67 | 102.46 | 1.083 | 202.64% | 270.70 MiB | 0 |

Every target in this group used its full 2-CPU limit. The coroutines build
reached 14,089.99 operations per second stateless (Rust: 13,122.55), 18,651.40
sessionless (Rust: 18,020.10) and 8,885.39 stateful (Rust: 9,698.48). Its p99
stayed between 5.05 and 6.05 ms, while the threads build's p99 was 39.83 to
59.60 ms.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 14,087.91 | 14,089.99 | 14,139.29 | 2 |
| This SDK, threads | stateless | 9,175.98 | 9,148.41 | 9,205.73 | 1 |
| Rust SDK | stateless | 13,179.44 | 13,122.55 | 13,116.67 | 2 |
| Go SDK | stateless | 3,280.43 | 3,271.00 | 3,263.01 | 2 |
| Python SDK | stateless | 1,040.24 | 1,033.50 | 1,033.09 | 2 |
| This SDK, coroutines | stateful | 8,885.39 | 8,853.24 | 8,899.69 | 1 |
| This SDK, threads | stateful | 7,055.55 | 7,059.44 | 7,042.96 | 1 |
| Rust SDK | stateful | 9,700.06 | 9,698.48 | 9,646.68 | 2 |
| Go SDK | stateful | 3,604.10 | 3,637.54 | 3,628.64 | 3 |
| This SDK, coroutines | sessionless | 18,651.40 | 18,644.80 | 18,671.58 | 1 |
| This SDK, threads | sessionless | 10,985.22 | 11,034.45 | 10,810.49 | 1 |
| Rust SDK | sessionless | 18,020.10 | 18,102.84 | 18,002.97 | 1 |
| Go SDK | sessionless | 4,438.76 | 4,430.97 | 4,414.56 | 2 |
| Python SDK | sessionless | 1,850.26 | 1,842.21 | 1,843.28 | 3 |

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
| This SDK, threads | stateful | 2024-11-05 | 7,055.55 | 0.12% | 1.70 | 3.28 | 39.83 | 0.286 | 205.89% | 35.66 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 704.65 | 0.03% | 40.78 | 41.83 | 41.88 | 0.284 | 21.02% | 29.97 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,308.38 | 0.13% | 0.36 | 41.79 | 42.02 | 0.329 | 45.13% | 30.38 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 5,001.74 | 0.67% | 1.19 | 60.49 | 69.81 | 0.404 | 206.51% | 35.92 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 700.70 | 0.04% | 40.80 | 41.85 | 41.95 | 0.513 | 37.45% | 44.96 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 10,985.22 | 1.08% | 1.84 | 51.57 | 59.60 | 0.183 | 205.51% | 34.43 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 6,399.56 | 0.57% | 2.04 | 73.23 | 76.13 | 0.315 | 205.56% | 45.30 MiB | 0 |

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 7,055.55 | 7,059.44 | 7,042.96 | 1 |
| hkr04/cpp-mcp | stateful | 704.65 | 704.60 | 704.95 | 1 |
| FastMCPP | stateful | 1,308.38 | 1,308.34 | 1,311.33 | 1 |
| cxxmcp | stateful | 4,995.90 | 5,001.74 | 5,056.97 | 2 |
| Neumann-Labs/mcp-cpp | stateful | 700.95 | 700.35 | 700.70 | 3 |
| This SDK, threads | sessionless | 10,985.22 | 11,034.45 | 10,810.49 | 1 |
| cxxmcp | sessionless | 6,407.69 | 6,340.65 | 6,399.56 | 3 |

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
| This SDK, threads | stateful | default | 2024-11-05 | 7,055.55 | 0.12% | 1.70 | 3.28 | 39.83 | 0.286 | 205.89% | 35.66 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 10,985.22 | 1.08% | 1.84 | 51.57 | 59.60 | 0.183 | 205.51% | 34.43 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 704.65 | 0.03% | 40.78 | 41.83 | 41.88 | 0.284 | 21.02% | 29.97 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | 6,754.20 | 0.10% | 0.93 | 3.45 | 60.28 | 0.299 | 206.63% | 30.11 MiB | 0 |
| FastMCPP | stateful | default | 2024-11-05 | 1,308.38 | 0.13% | 0.36 | 41.79 | 42.02 | 0.329 | 45.13% | 30.38 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | 2024-11-05 | 5,813.34 | 0.29% | 0.41 | 1.49 | 46.03 | 0.349 | 207.87% | 28.82 MiB | 0 |
| cxxmcp | stateful | default | 2024-11-05 | 5,001.74 | 0.67% | 1.19 | 60.49 | 69.81 | 0.404 | 206.51% | 35.92 MiB | 0 |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | 5,073.69 | 0.25% | 1.17 | 57.55 | 69.51 | 0.397 | 206.15% | 36.14 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 700.70 | 0.04% | 40.80 | 41.85 | 41.95 | 0.513 | 37.45% | 44.96 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | 2,579.86 | 0.10% | 1.45 | 72.87 | 76.41 | 0.779 | 206.54% | 52.57 MiB | 0 |
| cxxmcp | sessionless | default | 2026-07-28 | 6,399.56 | 0.57% | 2.04 | 73.23 | 76.13 | 0.315 | 205.56% | 45.30 MiB | 0 |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | 6,419.91 | 0.12% | 2.04 | 73.20 | 76.06 | 0.312 | 205.25% | 42.50 MiB | 0 |

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 7,055.55 | 7,059.44 | 7,042.96 | 1 |
| This SDK, threads | sessionless | default | 10,985.22 | 11,034.45 | 10,810.49 | 1 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 6,754.20 | 6,745.47 | 6,758.34 | 1 |
| FastMCPP | stateful | TCP_NODELAY | 5,832.61 | 5,813.34 | 5,799.27 | 2 |
| cxxmcp | stateful | TCP_NODELAY | 5,058.24 | 5,083.73 | 5,073.69 | 3 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2,583.98 | 2,579.39 | 2,579.86 | 3 |
| cxxmcp | sessionless | TCP_NODELAY | 6,419.91 | 6,409.57 | 6,424.26 | 1 |

## Audit evidence

Both runs completed with `publishable_candidate` set and no failed cells. Each
run passed:

- 144/144 protocol gates (preflight and postflight for 72 measured rounds) under
  the universal contract, with the required `adapter-exact-v1` checks passing
  in all 96 gates of the 16 C++ targets;
- 72/72 warmups and 72/72 measurements with zero MCP, HTTP, or check failures;
- 360/360 collector audits and 72/72 per-round resource/headroom reports.

Both runs used the `baseline-cohort-retire-v1` host network policy and measured
a clean working tree.
