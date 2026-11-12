# Benchmark results

Two runs put the same MCP/Redis/HTTP workload on every target, on one machine:

- The **paced run** (`production-full`, 2026-11-08) gives every target the same
  offered load. Each of the 50 virtual users pauses 50 ms after every iteration
  of four operations, so the rate cannot exceed 4,000 operations per second. A
  target that keeps up shows how much latency, CPU and memory that load costs
  it.
- The **capacity run** (`capacity-full`, 2026-11-11) removes the pause. Each
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
| Order seed | 1794121354 | 1794406810 |
| Commit | `27eaf0a7d48b9bc7b146b8a881dfdecfd52f3167` | `438a1839142d95bf7b73f0b22bf0acb05b8e6603` |

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
| This SDK, coroutines | stateless | 2024-11-05 | 3,686.26 | 0.06% | 0.38 | 1.75 | 2.49 | 0.144 | 55.64% | 8.38 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 3,678.45 | 0.03% | 0.36 | 1.81 | 2.61 | 0.203 | 77.82% | 32.93 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 3,675.38 | 0.02% | 0.44 | 1.73 | 2.49 | 0.154 | 59.32% | 16.64 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,078.73 | 0.44% | 1.65 | 4.91 | 7.13 | 0.574 | 184.51% | 14.23 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,057.94 | 0.15% | 15.52 | 62.74 | 74.44 | 1.896 | 203.52% | 270.30 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 3,544.86 | 0.81% | 0.52 | 2.09 | 3.02 | 0.240 | 90.65% | 10.00 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 3,541.10 | 0.88% | 0.49 | 1.79 | 2.66 | 0.306 | 113.27% | 33.27 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 3,602.76 | 0.05% | 0.47 | 1.69 | 2.43 | 0.199 | 75.06% | 15.35 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 3,225.14 | 0.29% | 1.09 | 3.38 | 4.84 | 0.514 | 173.39% | 14.16 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 3,743.03 | 0.11% | 0.54 | 2.37 | 3.50 | 0.118 | 47.76% | 8.73 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 3,743.45 | 0.03% | 0.50 | 2.42 | 3.51 | 0.188 | 75.17% | 32.92 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 3,743.06 | 0.03% | 0.59 | 2.19 | 3.17 | 0.124 | 50.52% | 15.92 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 3,442.11 | 0.03% | 1.48 | 5.60 | 8.17 | 0.431 | 154.99% | 14.44 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 1,950.65 | 0.08% | 14.35 | 35.98 | 63.19 | 1.025 | 203.46% | 273.40 MiB | 0 |

In every mode, both builds of this SDK and Rust reached rates within 2% of each
other without reaching their CPU limit. The coroutines build used 0.144 ms of
CPU per operation stateless (Rust: 0.154), 0.118 ms sessionless (Rust: 0.124)
and 0.240 ms stateful (Rust: 0.199). Its target p95 memory was 8.38 to
10.00 MiB, against 15.35 to 16.64 MiB for Rust.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 3,687.44 | 3,686.26 | 3,683.41 | 2 |
| This SDK, threads | stateless | 3,678.82 | 3,678.45 | 3,676.70 | 2 |
| Rust SDK | stateless | 3,675.93 | 3,674.53 | 3,675.38 | 3 |
| Go SDK | stateless | 3,061.23 | 3,087.85 | 3,078.73 | 3 |
| Python SDK | stateless | 1,057.94 | 1,057.33 | 1,060.29 | 1 |
| This SDK, coroutines | stateful | 3,590.46 | 3,544.86 | 3,537.54 | 2 |
| This SDK, threads | stateful | 3,539.76 | 3,594.85 | 3,541.10 | 3 |
| Rust SDK | stateful | 3,603.39 | 3,602.76 | 3,599.79 | 2 |
| Go SDK | stateful | 3,229.22 | 3,225.14 | 3,211.32 | 2 |
| This SDK, coroutines | sessionless | 3,749.56 | 3,743.03 | 3,741.93 | 2 |
| This SDK, threads | sessionless | 3,744.96 | 3,743.45 | 3,743.01 | 2 |
| Rust SDK | sessionless | 3,743.06 | 3,742.71 | 3,745.07 | 1 |
| Go SDK | sessionless | 3,442.11 | 3,441.96 | 3,443.86 | 1 |
| Python SDK | sessionless | 1,950.65 | 1,953.45 | 1,950.62 | 1 |

### Capacity run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 2024-11-05 | 13,766.96 | 0.65% | 2.44 | 4.53 | 5.15 | 0.143 | 201.87% | 8.85 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 9,129.60 | 0.58% | 1.09 | 3.23 | 50.44 | 0.221 | 206.32% | 34.46 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 13,373.66 | 0.87% | 2.47 | 4.17 | 4.86 | 0.146 | 200.77% | 16.45 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,272.05 | 0.58% | 7.90 | 17.44 | 23.70 | 0.595 | 197.79% | 15.98 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,033.56 | 0.01% | 16.06 | 69.41 | 97.75 | 1.933 | 202.62% | 271.00 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 8,620.64 | 0.17% | 3.13 | 5.54 | 6.22 | 0.230 | 202.10% | 9.92 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 6,889.48 | 0.36% | 1.76 | 3.32 | 38.86 | 0.293 | 205.82% | 34.46 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 9,765.41 | 0.12% | 2.54 | 4.28 | 5.12 | 0.200 | 200.03% | 21.11 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 3,637.77 | 0.16% | 6.04 | 12.40 | 16.82 | 0.538 | 198.74% | 16.35 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 18,541.36 | 0.88% | 2.90 | 5.12 | 5.71 | 0.106 | 200.40% | 8.99 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 10,950.57 | 0.27% | 1.84 | 52.46 | 59.63 | 0.184 | 205.55% | 34.18 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 18,099.72 | 0.37% | 3.01 | 5.07 | 5.99 | 0.109 | 200.60% | 16.19 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 4,429.50 | 0.24% | 12.81 | 21.31 | 25.75 | 0.443 | 198.44% | 17.55 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 1,844.82 | 0.37% | 17.94 | 82.78 | 103.59 | 1.086 | 202.50% | 270.40 MiB | 0 |

Every target in this group used its full 2-CPU limit. The coroutines build
reached 13,766.96 operations per second stateless (Rust: 13,373.66), 18,541.36
sessionless (Rust: 18,099.72) and 8,620.64 stateful (Rust: 9,765.41). Its p99
stayed between 5.15 and 6.22 ms, while the threads build's p99 was 38.86 to
59.63 ms.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 13,824.42 | 13,766.96 | 13,648.08 | 2 |
| This SDK, threads | stateless | 9,045.13 | 9,129.60 | 9,143.08 | 2 |
| Rust SDK | stateless | 13,179.28 | 13,386.10 | 13,373.66 | 3 |
| Go SDK | stateless | 3,280.69 | 3,272.05 | 3,244.26 | 2 |
| Python SDK | stateless | 1,033.56 | 1,033.66 | 1,033.52 | 1 |
| This SDK, coroutines | stateful | 8,600.68 | 8,628.60 | 8,620.64 | 3 |
| This SDK, threads | stateful | 6,923.16 | 6,889.48 | 6,873.98 | 2 |
| Rust SDK | stateful | 9,765.41 | 9,760.97 | 9,782.35 | 1 |
| Go SDK | stateful | 3,637.77 | 3,630.75 | 3,642.36 | 1 |
| This SDK, coroutines | sessionless | 18,315.82 | 18,632.04 | 18,541.36 | 3 |
| This SDK, threads | sessionless | 10,924.46 | 10,982.95 | 10,950.57 | 3 |
| Rust SDK | sessionless | 18,018.11 | 18,099.72 | 18,152.27 | 2 |
| Go SDK | sessionless | 4,441.35 | 4,429.50 | 4,419.89 | 2 |
| Python SDK | sessionless | 1,844.82 | 1,837.01 | 1,850.79 | 1 |

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
| This SDK, threads | stateful | 2024-11-05 | 3,541.10 | 0.88% | 0.49 | 1.79 | 2.66 | 0.306 | 113.27% | 33.27 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 678.75 | 0.01% | 40.77 | 41.82 | 41.92 | 0.282 | 20.62% | 31.15 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,122.63 | 0.03% | 0.39 | 41.79 | 42.04 | 0.333 | 39.73% | 32.46 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 3,628.28 | 0.03% | 0.37 | 1.63 | 2.43 | 0.306 | 115.88% | 29.97 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 675.82 | 0.04% | 40.80 | 41.85 | 42.03 | 0.513 | 36.89% | 44.11 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 3,743.45 | 0.03% | 0.50 | 2.42 | 3.51 | 0.188 | 75.17% | 32.92 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 3,760.46 | 0.09% | 0.46 | 2.07 | 3.02 | 0.254 | 104.48% | 43.76 MiB | 0 |

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 3,539.76 | 3,594.85 | 3,541.10 | 3 |
| hkr04/cpp-mcp | stateful | 678.75 | 678.71 | 678.83 | 1 |
| FastMCPP | stateful | 1,122.63 | 1,122.30 | 1,122.94 | 1 |
| cxxmcp | stateful | 3,628.77 | 3,626.87 | 3,628.28 | 3 |
| Neumann-Labs/mcp-cpp | stateful | 676.14 | 675.68 | 675.82 | 3 |
| This SDK, threads | sessionless | 3,744.96 | 3,743.45 | 3,743.01 | 2 |
| cxxmcp | sessionless | 3,760.46 | 3,760.72 | 3,755.03 | 1 |

### Capacity run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | 2024-11-05 | 6,889.48 | 0.36% | 1.76 | 3.32 | 38.86 | 0.293 | 205.82% | 34.46 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 704.44 | 0.08% | 40.78 | 41.83 | 41.89 | 0.285 | 21.10% | 31.45 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,312.39 | 0.06% | 0.36 | 41.80 | 42.06 | 0.322 | 45.41% | 29.29 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 5,021.84 | 0.70% | 1.18 | 60.03 | 69.50 | 0.402 | 205.95% | 37.25 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 700.78 | 0.04% | 40.80 | 41.85 | 41.97 | 0.514 | 38.25% | 45.21 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 10,950.57 | 0.27% | 1.84 | 52.46 | 59.63 | 0.184 | 205.55% | 34.18 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 6,471.50 | 0.14% | 2.02 | 73.23 | 76.08 | 0.311 | 205.38% | 42.16 MiB | 0 |

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 6,923.16 | 6,889.48 | 6,873.98 | 2 |
| hkr04/cpp-mcp | stateful | 703.73 | 704.83 | 704.44 | 3 |
| FastMCPP | stateful | 1,311.23 | 1,312.39 | 1,312.60 | 2 |
| cxxmcp | stateful | 5,076.84 | 5,011.79 | 5,021.84 | 3 |
| Neumann-Labs/mcp-cpp | stateful | 700.78 | 701.15 | 700.60 | 1 |
| This SDK, threads | sessionless | 10,924.46 | 10,982.95 | 10,950.57 | 3 |
| cxxmcp | sessionless | 6,471.87 | 6,471.50 | 6,455.84 | 2 |

## TCP_NODELAY builds

Each `-nodelay` target is the same adapter as the default build, compiled with
`CPPHTTPLIB_TCP_NODELAY=true` (see the [README](README.md#tcp_nodelay-builds)).
Each pair below comes from the same run. Each table starts with this SDK's
threads build from that run, over the same shared workload; it has no separate
TCP_NODELAY build.

### Paced run

| SDK | Mode | Build | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 2024-11-05 | 3,541.10 | 0.88% | 0.49 | 1.79 | 2.66 | 0.306 | 113.27% | 33.27 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 3,743.45 | 0.03% | 0.50 | 2.42 | 3.51 | 0.188 | 75.17% | 32.92 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 678.75 | 0.01% | 40.77 | 41.82 | 41.92 | 0.282 | 20.62% | 31.15 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | 3,625.09 | 0.17% | 0.33 | 1.52 | 2.31 | 0.233 | 87.51% | 26.83 MiB | 0 |
| FastMCPP | stateful | default | 2024-11-05 | 1,122.63 | 0.03% | 0.39 | 41.79 | 42.04 | 0.333 | 39.73% | 32.46 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | 2024-11-05 | 3,539.60 | 0.08% | 0.37 | 1.48 | 2.26 | 0.295 | 109.32% | 25.39 MiB | 0 |
| cxxmcp | stateful | default | 2024-11-05 | 3,628.28 | 0.03% | 0.37 | 1.63 | 2.43 | 0.306 | 115.88% | 29.97 MiB | 0 |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | 3,628.17 | 0.06% | 0.37 | 1.61 | 2.41 | 0.306 | 115.78% | 30.08 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 675.82 | 0.04% | 40.80 | 41.85 | 42.03 | 0.513 | 36.89% | 44.11 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | 3,549.77 | 0.23% | 0.38 | 1.64 | 2.52 | 0.506 | 191.44% | 44.97 MiB | 0 |
| cxxmcp | sessionless | default | 2026-07-28 | 3,760.46 | 0.09% | 0.46 | 2.07 | 3.02 | 0.254 | 104.48% | 43.76 MiB | 0 |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | 3,759.60 | 0.12% | 0.47 | 2.06 | 2.98 | 0.256 | 104.88% | 33.14 MiB | 0 |

With `TCP_NODELAY` on, the p50 of hkr04/cpp-mcp fell from 40.77 ms to 0.33 ms,
and that of Neumann-Labs/mcp-cpp from 40.80 ms to 0.38 ms. FastMCPP's p95 fell
from 41.79 ms to 1.48 ms. cxxmcp's median throughput changed by less than 0.1%.

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 3,539.76 | 3,594.85 | 3,541.10 | 3 |
| This SDK, threads | sessionless | default | 3,744.96 | 3,743.45 | 3,743.01 | 2 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 3,625.09 | 3,632.54 | 3,620.68 | 1 |
| FastMCPP | stateful | TCP_NODELAY | 3,536.15 | 3,539.60 | 3,541.93 | 2 |
| cxxmcp | stateful | TCP_NODELAY | 3,627.68 | 3,628.17 | 3,631.97 | 2 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 3,538.14 | 3,554.28 | 3,549.77 | 3 |
| cxxmcp | sessionless | TCP_NODELAY | 3,759.60 | 3,763.95 | 3,755.27 | 1 |

### Capacity run

| SDK | Mode | Build | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 2024-11-05 | 6,889.48 | 0.36% | 1.76 | 3.32 | 38.86 | 0.293 | 205.82% | 34.46 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 10,950.57 | 0.27% | 1.84 | 52.46 | 59.63 | 0.184 | 205.55% | 34.18 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 704.44 | 0.08% | 40.78 | 41.83 | 41.89 | 0.285 | 21.10% | 31.45 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | 6,754.67 | 0.30% | 0.93 | 3.41 | 60.26 | 0.297 | 205.96% | 29.71 MiB | 0 |
| FastMCPP | stateful | default | 2024-11-05 | 1,312.39 | 0.06% | 0.36 | 41.80 | 42.06 | 0.322 | 45.41% | 29.29 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | - | failed | - | - | - | - | - | - | - | - |
| cxxmcp | stateful | default | 2024-11-05 | 5,021.84 | 0.70% | 1.18 | 60.03 | 69.50 | 0.402 | 205.95% | 37.25 MiB | 0 |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | 5,059.39 | 0.49% | 1.17 | 60.56 | 69.74 | 0.398 | 206.34% | 40.12 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 700.78 | 0.04% | 40.80 | 41.85 | 41.97 | 0.514 | 38.25% | 45.21 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | 2,620.90 | 0.32% | 1.42 | 72.76 | 76.51 | 0.767 | 206.06% | 52.71 MiB | 0 |
| cxxmcp | sessionless | default | 2026-07-28 | 6,471.50 | 0.14% | 2.02 | 73.23 | 76.08 | 0.311 | 205.38% | 42.16 MiB | 0 |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | 6,471.18 | 0.04% | 2.02 | 73.17 | 76.14 | 0.311 | 205.58% | 41.45 MiB | 0 |

FastMCPP's TCP_NODELAY build measured 5,841.71 and 5,811.69 operations per
second in rounds 1 and 2. In the round 3 warmup one request timed out, which
crossed a k6 threshold. The run recorded the cell as failed, as a capacity run
does, and skipped its round 3 measurement, so it has no median.

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 6,923.16 | 6,889.48 | 6,873.98 | 2 |
| This SDK, threads | sessionless | default | 10,924.46 | 10,982.95 | 10,950.57 | 3 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 6,765.51 | 6,726.39 | 6,754.67 | 3 |
| FastMCPP | stateful | TCP_NODELAY | 5,841.71 | 5,811.69 | failed | - |
| cxxmcp | stateful | TCP_NODELAY | 5,059.39 | 5,023.85 | 5,071.47 | 1 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2,634.89 | 2,620.90 | 2,620.06 | 2 |
| cxxmcp | sessionless | TCP_NODELAY | 6,471.18 | 6,470.62 | 6,475.63 | 1 |

## Audit evidence

Both runs completed with `publishable_candidate` set. The paced run passed:

- 144/144 protocol gates (preflight and postflight for 72 measured rounds) under
  the universal contract, with the required `adapter-exact-v1` checks passing
  in all 96 gates of the 16 C++ targets;
- 72/72 warmups and 72/72 measurements with zero MCP, HTTP, or check failures;
- 360/360 collector audits and 72/72 per-round resource/headroom reports.

In the capacity run:

- 143/143 protocol gates passed under the universal contract, with the required
  `adapter-exact-v1` checks passing in all 95 gates of the 16 C++ targets;
- 71 of 72 warmups and 71/71 measurements had zero MCP, HTTP, or check
  failures; the other warmup is the failed cell described under
  [TCP_NODELAY builds](#tcp_nodelay-builds);
- 355/355 collector audits and 71/71 per-round resource/headroom reports passed.

Both runs used the `baseline-cohort-retire-v1` host network policy and measured
a clean working tree.
