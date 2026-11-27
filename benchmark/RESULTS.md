# Benchmark results

Two runs put the same MCP/Redis/HTTP workload on every target, on one machine:

- The **paced run** (`production-full`, 2026-11-26) gives every target the same
  offered load. Each of the 50 virtual users pauses 50 ms after every iteration
  of four operations, so the rate cannot exceed 4,000 operations per second. A
  target that keeps up shows how much latency, CPU and memory that load costs
  it.
- The **capacity run** (`capacity-full`, 2026-11-26) removes the pause. Each
  target then runs as fast as its 2-CPU limit allows, which shows its maximum
  throughput and its latency at that point.

The two runs used different load models and are never combined in one table.
They measured the same commit, targets and CPU placement (see [Run
configuration](#run-configuration)). Each section below shows the paced table
first and the capacity table second. [Audit evidence](#audit-evidence) at the
end lists what each run passed.

## Run configuration

Both runs measured the same 27 targets in three counterbalanced rounds. Each
round used 50 VUs, a separate 15-second ramp plus 60-second warmup, and a
five-minute constant-load measurement for every target. Every target container
had the same 2-CPU and 2-GiB limits and ran on two CPUs of its own, on separate
physical cores, with the other hardware threads of those cores left idle (see
[Concurrency and resource
controls](README.md#concurrency-and-resource-controls)). Every row used the
shared `upstream-v2-strict-mcp-v1` measured contract, and every C++ target also
had to pass the `adapter-exact-v1` adapter checks in preflight and postflight.
Both runs built the C++ targets on Ubuntu 26.04 (see [Comparable C++
scope](README.md#comparable-c-scope)).

| | Paced run | Capacity run |
|---|---|---|
| Profile | `production-full` | `capacity-full` |
| Targets | 27 | 27 |
| Target CPUs | CPUs 10 and 11; 22 and 23 idle | CPUs 10 and 11; 22 and 23 idle |
| Pause per iteration | 50 ms | none |
| k6 limits | 4 CPUs, 2 GiB | 8 CPUs, 4 GiB |
| API service CPU limit | 2 CPUs | 8 CPUs |
| Redis CPU limit | 0.5 CPU | 1 CPU |
| Order seed | 1795682642 | 1795650601 |
| C++ target image base | Ubuntu 26.04 | Ubuntu 26.04 |
| Commit | `6683966b9417517fd02b75cc120c8cce396b80de` | `6683966b9417517fd02b75cc120c8cce396b80de` |

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

This SDK appears in three builds. The coroutines build runs its tool handlers
as C++20 coroutines over asynchronous Redis and HTTP clients, without a handler
thread pool. The threads build runs the shared, synchronous C++ workload that
the [C++ SDKs](#c-sdks) section uses. See [Coroutine
workload](README.md#coroutine-workload). The sync-mode build runs the same
shared workload as a sync mode program. See [Sync mode
workload](README.md#sync-mode-workload).

Stateless and stateful rows use protocol `2024-11-05`. Sessionless rows use
`2026-07-28`, which has no sessions. Python has no stateful row (see [Session
modes](README.md#session-modes)).

### Paced run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 2024-11-05 | 3,721.14 | 0.07% | 0.27 | 1.51 | 2.32 | 0.095 | 36.89% | 8.77 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 3,694.76 | 0.11% | 0.36 | 1.70 | 2.61 | 0.114 | 44.71% | 29.61 MiB | 0 |
| This SDK, sync mode | stateless | 2024-11-05 | 3,686.16 | 0.15% | 0.35 | 1.76 | 2.74 | 0.099 | 38.22% | 27.96 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 3,704.71 | 0.12% | 0.33 | 1.56 | 2.38 | 0.114 | 44.14% | 13.12 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,173.34 | 0.21% | 1.29 | 4.93 | 7.87 | 0.508 | 169.96% | 13.69 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,098.73 | 0.16% | 20.58 | 53.23 | 78.15 | 1.817 | 202.59% | 269.90 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 3,657.95 | 0.03% | 0.30 | 1.47 | 2.28 | 0.143 | 54.72% | 9.59 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 3,621.20 | 0.07% | 0.41 | 1.68 | 2.63 | 0.165 | 63.95% | 30.54 MiB | 0 |
| This SDK, sync mode | stateful | 2024-11-05 | 3,622.58 | 0.17% | 0.39 | 1.66 | 2.64 | 0.129 | 49.09% | 26.89 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 3,652.01 | 0.10% | 0.34 | 1.41 | 2.21 | 0.153 | 58.11% | 16.92 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 3,347.68 | 0.26% | 0.84 | 2.87 | 4.58 | 0.440 | 153.55% | 13.36 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 3,766.24 | 0.11% | 0.43 | 2.14 | 3.32 | 0.090 | 36.11% | 8.96 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 3,740.49 | 0.03% | 0.54 | 2.38 | 3.67 | 0.104 | 41.72% | 30.98 MiB | 0 |
| This SDK, sync mode | sessionless | 2026-07-28 | 3,738.03 | 0.20% | 0.53 | 2.49 | 3.87 | 0.093 | 36.97% | 28.75 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 3,754.35 | 0.01% | 0.51 | 2.13 | 3.20 | 0.105 | 42.25% | 14.42 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 3,564.97 | 0.19% | 1.11 | 4.13 | 6.77 | 0.373 | 137.02% | 13.68 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 2,149.75 | 2.41% | 10.83 | 35.24 | 58.05 | 0.929 | 202.01% | 268.80 MiB | 0 |

In every mode, the three builds of this SDK and Rust reached rates within 1.1%
of each other without reaching their CPU limit. The coroutines build used
0.095 ms of CPU per operation stateless (Rust: 0.114), 0.090 ms sessionless
(Rust: 0.105) and 0.143 ms stateful (Rust: 0.153). Its target p95 memory was
8.77 to 9.59 MiB, against 13.12 to 16.92 MiB for Rust. The sync-mode build used
0.093 to 0.129 ms per operation, and the threads build 0.104 to 0.165 ms.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 3,717.66 | 3,722.38 | 3,721.14 | 3 |
| This SDK, threads | stateless | 3,697.05 | 3,694.76 | 3,689.08 | 2 |
| This SDK, sync mode | stateless | 3,694.34 | 3,683.64 | 3,686.16 | 3 |
| Rust SDK | stateless | 3,707.68 | 3,704.71 | 3,698.96 | 2 |
| Go SDK | stateless | 3,168.28 | 3,173.34 | 3,181.40 | 2 |
| Python SDK | stateless | 1,096.32 | 1,098.73 | 1,099.84 | 2 |
| This SDK, coroutines | stateful | 3,659.86 | 3,657.95 | 3,657.50 | 2 |
| This SDK, threads | stateful | 3,622.35 | 3,617.47 | 3,621.20 | 3 |
| This SDK, sync mode | stateful | 3,630.76 | 3,618.89 | 3,622.58 | 3 |
| Rust SDK | stateful | 3,656.63 | 3,652.01 | 3,649.53 | 2 |
| Go SDK | stateful | 3,347.68 | 3,339.39 | 3,356.82 | 1 |
| This SDK, coroutines | sessionless | 3,772.92 | 3,766.24 | 3,765.18 | 2 |
| This SDK, threads | sessionless | 3,738.59 | 3,740.49 | 3,741.07 | 2 |
| This SDK, sync mode | sessionless | 3,746.52 | 3,738.03 | 3,731.60 | 2 |
| Rust SDK | sessionless | 3,754.67 | 3,754.35 | 3,754.35 | 3 |
| Go SDK | sessionless | 3,564.97 | 3,571.07 | 3,557.25 | 1 |
| Python SDK | sessionless | 2,149.75 | 2,068.43 | 2,162.86 | 1 |

### Capacity run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 2024-11-05 | 19,855.16 | 0.08% | 1.34 | 3.05 | 3.86 | 0.087 | 183.53% | 9.26 MiB | 0 |
| This SDK, threads | stateless | 2024-11-05 | 18,525.37 | 1.43% | 1.10 | 3.20 | 4.45 | 0.100 | 193.38% | 31.79 MiB | 0 |
| This SDK, sync mode | stateless | 2024-11-05 | 17,412.52 | 2.38% | 1.17 | 3.42 | 4.72 | 0.095 | 171.56% | 30.25 MiB | 0 |
| Rust SDK | stateless | 2024-11-05 | 16,902.69 | 0.84% | 1.79 | 3.39 | 5.01 | 0.108 | 192.92% | 15.43 MiB | 0 |
| Go SDK | stateless | 2024-11-05 | 3,699.48 | 0.35% | 7.11 | 17.16 | 22.79 | 0.509 | 191.42% | 16.79 MiB | 0 |
| Python SDK | stateless | 2024-11-05 | 1,094.17 | 0.13% | 25.11 | 60.98 | 90.97 | 1.830 | 202.44% | 270.70 MiB | 0 |
| This SDK, coroutines | stateful | 2024-11-05 | 14,416.67 | 1.10% | 2.04 | 3.34 | 4.38 | 0.133 | 200.63% | 9.90 MiB | 0 |
| This SDK, threads | stateful | 2024-11-05 | 12,907.63 | 0.93% | 1.62 | 3.35 | 4.69 | 0.151 | 201.55% | 33.88 MiB | 0 |
| This SDK, sync mode | stateful | 2024-11-05 | 12,989.05 | 0.58% | 1.64 | 3.52 | 4.89 | 0.122 | 163.99% | 29.64 MiB | 0 |
| Rust SDK | stateful | 2024-11-05 | 12,851.14 | 0.64% | 1.91 | 3.54 | 5.19 | 0.145 | 196.86% | 23.79 MiB | 0 |
| Go SDK | stateful | 2024-11-05 | 4,287.81 | 0.04% | 4.99 | 11.27 | 15.05 | 0.442 | 193.23% | 17.07 MiB | 0 |
| This SDK, coroutines | sessionless | 2026-07-28 | 26,347.89 | 0.23% | 1.96 | 3.63 | 4.60 | 0.070 | 193.16% | 9.31 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 22,943.24 | 3.34% | 2.07 | 4.32 | 5.62 | 0.081 | 190.77% | 32.24 MiB | 0 |
| This SDK, sync mode | sessionless | 2026-07-28 | 22,701.97 | 2.63% | 2.03 | 4.40 | 5.71 | 0.074 | 173.42% | 30.11 MiB | 0 |
| Rust SDK | sessionless | 2026-07-28 | 22,620.47 | 0.31% | 2.31 | 4.09 | 5.95 | 0.081 | 195.79% | 15.27 MiB | 0 |
| Go SDK | sessionless | 2026-07-28 | 5,076.64 | 0.21% | 10.65 | 21.62 | 27.98 | 0.372 | 192.66% | 17.98 MiB | 0 |
| Python SDK | sessionless | 2026-07-28 | 1,960.39 | 1.28% | 25.25 | 77.76 | 124.95 | 1.020 | 202.20% | 273.80 MiB | 0 |

The coroutines build reached 19,855.16 operations per second stateless (Rust:
16,902.69), 26,347.89 sessionless (Rust: 22,620.47) and 14,416.67 stateful
(Rust: 12,851.14). Its p99 stayed between 3.86 and 4.60 ms (Rust: 5.01 to
5.95 ms), and its target p95 memory between 9.26 and 9.90 MiB (Rust: 15.27 to
23.79 MiB). The threads build reached 100.4% to 109.6% of Rust's rate in each
mode, and the sync-mode build 100.4% to 103.0%. The sync-mode build's target
p95 CPU was 163.99% to 173.42%; every other target in this group reached
183.53% or more.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, coroutines | stateless | 19,855.16 | 19,880.92 | 19,854.50 | 1 |
| This SDK, threads | stateless | 18,111.15 | 18,525.37 | 18,601.96 | 2 |
| This SDK, sync mode | stateless | 17,866.01 | 17,412.52 | 17,038.81 | 2 |
| Rust SDK | stateless | 16,833.10 | 16,902.69 | 17,107.00 | 2 |
| Go SDK | stateless | 3,699.48 | 3,687.38 | 3,713.30 | 1 |
| Python SDK | stateless | 1,095.55 | 1,092.60 | 1,094.17 | 3 |
| This SDK, coroutines | stateful | 14,315.71 | 14,626.88 | 14,416.67 | 3 |
| This SDK, threads | stateful | 12,907.63 | 13,073.26 | 12,840.39 | 1 |
| This SDK, sync mode | stateful | 12,956.50 | 13,101.58 | 12,989.05 | 3 |
| Rust SDK | stateful | 12,927.30 | 12,851.14 | 12,762.97 | 2 |
| Go SDK | stateful | 4,286.63 | 4,287.81 | 4,289.99 | 2 |
| This SDK, coroutines | sessionless | 26,347.89 | 26,309.52 | 26,428.43 | 1 |
| This SDK, threads | sessionless | 23,270.87 | 22,943.24 | 21,826.58 | 2 |
| This SDK, sync mode | sessionless | 21,789.99 | 22,701.97 | 22,895.22 | 2 |
| Rust SDK | sessionless | 22,657.15 | 22,620.47 | 22,523.37 | 2 |
| Go SDK | sessionless | 5,074.54 | 5,076.64 | 5,093.61 | 2 |
| Python SDK | sessionless | 1,960.39 | 1,948.09 | 1,996.59 | 1 |


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
| This SDK, threads | stateful | 2024-11-05 | 3,621.20 | 0.07% | 0.41 | 1.68 | 2.63 | 0.165 | 63.95% | 30.54 MiB | 0 |
| This SDK, coroutines (async workload) | stateful | 2024-11-05 | 3,657.95 | 0.03% | 0.30 | 1.47 | 2.28 | 0.143 | 54.72% | 9.59 MiB | 0 |
| This SDK, sync mode | stateful | 2024-11-05 | 3,622.58 | 0.17% | 0.39 | 1.66 | 2.64 | 0.129 | 49.09% | 26.89 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 680.24 | 0.01% | 40.77 | 41.84 | 41.92 | 0.164 | 12.12% | 22.91 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,128.28 | 0.10% | 0.31 | 41.23 | 42.00 | 0.209 | 25.16% | 24.16 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 3,660.56 | 0.07% | 0.42 | 1.46 | 2.29 | 0.170 | 64.00% | 31.33 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 679.86 | 0.05% | 40.80 | 41.85 | 41.92 | 0.311 | 22.45% | 43.08 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 3,740.49 | 0.03% | 0.54 | 2.38 | 3.67 | 0.104 | 41.72% | 30.98 MiB | 0 |
| This SDK, coroutines (async workload) | sessionless | 2026-07-28 | 3,766.24 | 0.11% | 0.43 | 2.14 | 3.32 | 0.090 | 36.11% | 8.96 MiB | 0 |
| This SDK, sync mode | sessionless | 2026-07-28 | 3,738.03 | 0.20% | 0.53 | 2.49 | 3.87 | 0.093 | 36.97% | 28.75 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 3,762.47 | 0.07% | 0.52 | 1.79 | 2.67 | 0.126 | 52.05% | 31.34 MiB | 0 |

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 3,622.35 | 3,617.47 | 3,621.20 | 3 |
| This SDK, coroutines (async workload) | stateful | 3,659.86 | 3,657.95 | 3,657.50 | 2 |
| This SDK, sync mode | stateful | 3,630.76 | 3,618.89 | 3,622.58 | 3 |
| hkr04/cpp-mcp | stateful | 680.25 | 680.24 | 680.16 | 2 |
| FastMCPP | stateful | 1,129.99 | 1,127.83 | 1,128.28 | 3 |
| cxxmcp | stateful | 3,660.56 | 3,658.02 | 3,663.09 | 1 |
| Neumann-Labs/mcp-cpp | stateful | 679.76 | 679.86 | 680.42 | 2 |
| This SDK, threads | sessionless | 3,738.59 | 3,740.49 | 3,741.07 | 2 |
| This SDK, coroutines (async workload) | sessionless | 3,772.92 | 3,766.24 | 3,765.18 | 2 |
| This SDK, sync mode | sessionless | 3,746.52 | 3,738.03 | 3,731.60 | 2 |
| cxxmcp | sessionless | 3,766.35 | 3,761.49 | 3,762.47 | 3 |

### Capacity run

| SDK | Mode | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | 2024-11-05 | 12,907.63 | 0.93% | 1.62 | 3.35 | 4.69 | 0.151 | 201.55% | 33.88 MiB | 0 |
| This SDK, coroutines (async workload) | stateful | 2024-11-05 | 14,416.67 | 1.10% | 2.04 | 3.34 | 4.38 | 0.133 | 200.63% | 9.90 MiB | 0 |
| This SDK, sync mode | stateful | 2024-11-05 | 12,989.05 | 0.58% | 1.64 | 3.52 | 4.89 | 0.122 | 163.99% | 29.64 MiB | 0 |
| hkr04/cpp-mcp | stateful | 2025-03-26 | 706.49 | 0.04% | 40.78 | 41.84 | 41.89 | 0.171 | 12.68% | 23.04 MiB | 0 |
| FastMCPP | stateful | 2024-11-05 | 1,323.78 | 0.02% | 0.30 | 41.21 | 42.03 | 0.208 | 29.16% | 30.41 MiB | 0 |
| cxxmcp | stateful | 2024-11-05 | 11,218.93 | 1.19% | 2.32 | 3.88 | 4.92 | 0.171 | 196.78% | 44.89 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | 2025-11-25 | 706.94 | 0.04% | 40.80 | 41.84 | 41.90 | 0.313 | 23.18% | 44.49 MiB | 0 |
| This SDK, threads | sessionless | 2026-07-28 | 22,943.24 | 3.34% | 2.07 | 4.32 | 5.62 | 0.081 | 190.77% | 32.24 MiB | 0 |
| This SDK, coroutines (async workload) | sessionless | 2026-07-28 | 26,347.89 | 0.23% | 1.96 | 3.63 | 4.60 | 0.070 | 193.16% | 9.31 MiB | 0 |
| This SDK, sync mode | sessionless | 2026-07-28 | 22,701.97 | 2.63% | 2.03 | 4.40 | 5.71 | 0.074 | 173.42% | 30.11 MiB | 0 |
| cxxmcp | sessionless | 2026-07-28 | 15,865.76 | 0.83% | 3.01 | 5.72 | 7.51 | 0.121 | 195.55% | 42.70 MiB | 0 |

This SDK's threads build reached 12,907.63 operations per second stateful and
22,943.24 sessionless, its coroutines build 14,416.67 and 26,347.89, and its
sync-mode build 12,989.05 and 22,701.97. cxxmcp, the fastest of the other SDKs
in their default builds, reached 11,218.93 and 15,865.76. The default builds
of hkr04/cpp-mcp, FastMCPP and Neumann-Labs/mcp-cpp reached 706.49 to
1,323.78; [TCP_NODELAY builds](#tcp_nodelay-builds) has their builds with
`TCP_NODELAY` on.

The measured operation rates for all rounds were:

| SDK | Mode | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | 12,907.63 | 13,073.26 | 12,840.39 | 1 |
| This SDK, coroutines (async workload) | stateful | 14,315.71 | 14,626.88 | 14,416.67 | 3 |
| This SDK, sync mode | stateful | 12,956.50 | 13,101.58 | 12,989.05 | 3 |
| hkr04/cpp-mcp | stateful | 706.48 | 706.96 | 706.49 | 3 |
| FastMCPP | stateful | 1,323.78 | 1,323.58 | 1,324.15 | 1 |
| cxxmcp | stateful | 11,218.93 | 11,262.29 | 11,013.85 | 1 |
| Neumann-Labs/mcp-cpp | stateful | 707.15 | 706.94 | 706.63 | 2 |
| This SDK, threads | sessionless | 23,270.87 | 22,943.24 | 21,826.58 | 2 |
| This SDK, coroutines (async workload) | sessionless | 26,347.89 | 26,309.52 | 26,428.43 | 1 |
| This SDK, sync mode | sessionless | 21,789.99 | 22,701.97 | 22,895.22 | 2 |
| cxxmcp | sessionless | 15,901.53 | 15,659.18 | 15,865.76 | 3 |


## TCP_NODELAY builds

Each `-nodelay` target is the same adapter as the default build, compiled with
`CPPHTTPLIB_TCP_NODELAY=true` (see the [README](README.md#tcp_nodelay-builds)).
Each pair below comes from the same run. Each table starts with this SDK's
threads build from that run, over the same shared workload; it has no separate
TCP_NODELAY build.

### Paced run

| SDK | Mode | Build | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 2024-11-05 | 3,621.20 | 0.07% | 0.41 | 1.68 | 2.63 | 0.165 | 63.95% | 30.54 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 3,740.49 | 0.03% | 0.54 | 2.38 | 3.67 | 0.104 | 41.72% | 30.98 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 680.24 | 0.01% | 40.77 | 41.84 | 41.92 | 0.164 | 12.12% | 22.91 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | 3,667.68 | 0.07% | 0.29 | 1.36 | 2.18 | 0.135 | 51.08% | 24.39 MiB | 0 |
| FastMCPP | stateful | default | 2024-11-05 | 1,128.28 | 0.10% | 0.31 | 41.23 | 42.00 | 0.209 | 25.16% | 24.16 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | 2024-11-05 | 3,606.97 | 0.29% | 0.34 | 1.25 | 2.02 | 0.171 | 64.22% | 24.80 MiB | 0 |
| cxxmcp | stateful | default | 2024-11-05 | 3,660.56 | 0.07% | 0.42 | 1.46 | 2.29 | 0.170 | 64.00% | 31.33 MiB | 0 |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | 3,659.63 | 0.07% | 0.42 | 1.48 | 2.32 | 0.171 | 64.44% | 32.25 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 679.86 | 0.05% | 40.80 | 41.85 | 41.92 | 0.311 | 22.45% | 43.08 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | 3,583.95 | 0.25% | 0.42 | 1.45 | 2.17 | 0.252 | 93.39% | 40.39 MiB | 0 |
| cxxmcp | sessionless | default | 2026-07-28 | 3,762.47 | 0.07% | 0.52 | 1.79 | 2.67 | 0.126 | 52.05% | 31.34 MiB | 0 |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | 3,765.96 | 0.08% | 0.51 | 1.79 | 2.69 | 0.125 | 50.17% | 32.32 MiB | 0 |

With `TCP_NODELAY` on, the p50 of hkr04/cpp-mcp fell from 40.77 ms to 0.29 ms,
and that of Neumann-Labs/mcp-cpp from 40.80 ms to 0.42 ms. FastMCPP's p95 fell
from 41.23 ms to 1.25 ms. cxxmcp's median throughput changed by less than 0.1%.

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 3,622.35 | 3,617.47 | 3,621.20 | 3 |
| This SDK, threads | sessionless | default | 3,738.59 | 3,740.49 | 3,741.07 | 2 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 3,667.68 | 3,671.62 | 3,667.24 | 1 |
| FastMCPP | stateful | TCP_NODELAY | 3,606.97 | 3,598.50 | 3,619.34 | 1 |
| cxxmcp | stateful | TCP_NODELAY | 3,656.80 | 3,659.63 | 3,661.57 | 2 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 3,598.85 | 3,583.95 | 3,582.25 | 2 |
| cxxmcp | sessionless | TCP_NODELAY | 3,765.96 | 3,760.96 | 3,766.59 | 1 |

### Capacity run

| SDK | Mode | Build | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | CPU ms/op | Target p95 CPU | Target p95 memory | Errors |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 2024-11-05 | 12,907.63 | 0.93% | 1.62 | 3.35 | 4.69 | 0.151 | 201.55% | 33.88 MiB | 0 |
| This SDK, threads | sessionless | default | 2026-07-28 | 22,943.24 | 3.34% | 2.07 | 4.32 | 5.62 | 0.081 | 190.77% | 32.24 MiB | 0 |
| hkr04/cpp-mcp | stateful | default | 2025-03-26 | 706.49 | 0.04% | 40.78 | 41.84 | 41.89 | 0.171 | 12.68% | 23.04 MiB | 0 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 2025-03-26 | 13,824.12 | 2.40% | 1.38 | 2.72 | 3.54 | 0.142 | 204.00% | 26.23 MiB | 0 |
| FastMCPP | stateful | default | 2024-11-05 | 1,323.78 | 0.02% | 0.30 | 41.21 | 42.03 | 0.208 | 29.16% | 30.41 MiB | 0 |
| FastMCPP | stateful | TCP_NODELAY | 2024-11-05 | 11,282.45 | 0.08% | 0.70 | 1.52 | 2.11 | 0.171 | 199.45% | 24.78 MiB | 0 |
| cxxmcp | stateful | default | 2024-11-05 | 11,218.93 | 1.19% | 2.32 | 3.88 | 4.92 | 0.171 | 196.78% | 44.89 MiB | 0 |
| cxxmcp | stateful | TCP_NODELAY | 2024-11-05 | 11,214.42 | 1.29% | 2.32 | 3.89 | 4.93 | 0.171 | 196.93% | 37.29 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | default | 2025-11-25 | 706.94 | 0.04% | 40.80 | 41.84 | 41.90 | 0.313 | 23.18% | 44.49 MiB | 0 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 2025-11-25 | 7,191.88 | 0.63% | 1.44 | 5.12 | 7.35 | 0.267 | 196.36% | 60.61 MiB | 0 |
| cxxmcp | sessionless | default | 2026-07-28 | 15,865.76 | 0.83% | 3.01 | 5.72 | 7.51 | 0.121 | 195.55% | 42.70 MiB | 0 |
| cxxmcp | sessionless | TCP_NODELAY | 2026-07-28 | 15,888.60 | 0.47% | 3.01 | 5.70 | 7.51 | 0.121 | 196.36% | 43.31 MiB | 0 |

With `TCP_NODELAY` on, hkr04/cpp-mcp reached 13,824.12 operations per second,
FastMCPP 11,282.45 and Neumann-Labs/mcp-cpp 7,191.88, against 706.49 to
1,323.78 for their default builds. cxxmcp's median throughput changed by less
than 0.2%.

The measured operation rates for all rounds were:

| SDK | Mode | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---|---:|---:|---:|---:|
| This SDK, threads | stateful | default | 12,907.63 | 13,073.26 | 12,840.39 | 1 |
| This SDK, threads | sessionless | default | 23,270.87 | 22,943.24 | 21,826.58 | 2 |
| hkr04/cpp-mcp | stateful | TCP_NODELAY | 13,774.14 | 13,824.12 | 14,379.54 | 2 |
| FastMCPP | stateful | TCP_NODELAY | 11,289.28 | 11,282.45 | 11,271.10 | 2 |
| cxxmcp | stateful | TCP_NODELAY | 11,229.66 | 11,214.42 | 10,973.84 | 2 |
| Neumann-Labs/mcp-cpp | stateful | TCP_NODELAY | 7,191.88 | 7,174.71 | 7,260.41 | 1 |
| cxxmcp | sessionless | TCP_NODELAY | 15,888.60 | 15,925.75 | 15,783.48 | 1 |


## Audit evidence

Both runs completed with `publishable_candidate` set and no failed cells. Each
passed:

- 162/162 protocol gates (preflight and postflight for 81 measured rounds)
  under the universal contract, with the required `adapter-exact-v1` checks
  passing in all 114 gates of the 19 C++ targets;
- 81/81 warmups and 81/81 measurements with zero MCP, HTTP, or check failures;
- 405/405 collector audits and 81/81 per-round resource/headroom reports.

Both runs used the `baseline-cohort-retire-v1` host network policy and measured
a clean working tree.
