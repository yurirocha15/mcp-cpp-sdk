# Benchmark results

These results come from a `production-full` run on 2026-10-18, which put the
same MCP/Redis/HTTP workload on every target. The [C++ SDKs](#c-sdks) table is
the comparison; the other tables are diagnostic. [Audit evidence](#audit-evidence)
at the end lists what the run passed.

## Run configuration

The run measured all twelve targets in three counterbalanced rounds (order seed
1792335571). Each round used 50 VUs, a separate 15-second ramp plus 60-second
warmup, and a five-minute constant-load measurement for every target. Every
target container had the same 2-CPU and 2-GiB limits. Every row used the shared
`upstream-v2-strict-mcp-v1` measured contract. The nine C++ targets, default
and TCP_NODELAY builds alike, also had to pass the `adapter-exact-v1` adapter
checks in preflight and postflight.

CPU follows Docker's convention: 100% is one fully used core. Resource values
are from the round selected for that target's median throughput. Target
saturation is reported but not gated. Headroom gates apply to Redis, the API
service, k6, and the host, and every one of them passed.

## C++ SDKs

| C++ SDK | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK | 2024-11-05 | 3,564.90 | 0.46% | 0.43 | 1.71 | 2.46 | 148.47% | 44.74 MiB | 0 |
| hkr04/cpp-mcp | 2025-03-26 | 678.67 | 0.03% | 40.77 | 41.82 | 41.91 | 21.12% | 31.35 MiB | 0 |
| FastMCPP | 2024-11-05 | 1,123.39 | 0.04% | 0.38 | 41.78 | 42.07 | 40.98% | 35.61 MiB | 0 |
| cxxmcp | 2024-11-05 | 3,043.31 | 0.89% | 1.48 | 5.71 | 7.13 | 71.34% | 30.72 MiB | 0 |
| Neumann-Labs/mcp-cpp | 2025-11-25 | 675.95 | 0.04% | 40.80 | 41.85 | 42.02 | 38.44% | 54.26 MiB | 0 |

hkr04/cpp-mcp, FastMCPP, cxxmcp, and Neumann-Labs/mcp-cpp leave `TCP_NODELAY`
off. The same adapters built with it on are measured in
[TCP_NODELAY builds](#tcp_nodelay-builds-diagnostic).

The measured operation rates for all rounds were:

| C++ SDK | Round 1 | Round 2 | Round 3 | Selected round |
|---|---:|---:|---:|---:|
| This SDK | 3,537.98 | 3,567.21 | 3,564.90 | 3 |
| hkr04/cpp-mcp | 678.68 | 678.30 | 678.67 | 3 |
| FastMCPP | 1,123.39 | 1,123.44 | 1,122.72 | 1 |
| cxxmcp | 2,997.48 | 3,043.31 | 3,044.94 | 2 |
| Neumann-Labs/mcp-cpp | 675.95 | 675.66 | 676.21 | 1 |

These numbers describe this pinned MCP/Redis/HTTP workload and environment;
they are not a general-purpose application-performance claim.

## TCP_NODELAY builds (diagnostic)

Each `-nodelay` target is the same adapter as the default build, compiled with
`CPPHTTPLIB_TCP_NODELAY=true` (see the
[README](README.md#tcp_nodelay-builds)). Each pair below comes from the same
run:

| C++ SDK | Build | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| hkr04/cpp-mcp | default | 678.67 | 0.03% | 40.77 | 41.82 | 41.91 | 21.12% | 31.35 MiB | 0 |
| hkr04/cpp-mcp | TCP_NODELAY | 3,639.57 | 0.30% | 0.32 | 1.47 | 2.24 | 90.38% | 33.79 MiB | 0 |
| FastMCPP | default | 1,123.39 | 0.04% | 0.38 | 41.78 | 42.07 | 40.98% | 35.61 MiB | 0 |
| FastMCPP | TCP_NODELAY | 3,565.90 | 0.68% | 0.36 | 1.43 | 2.17 | 111.25% | 38.08 MiB | 0 |
| cxxmcp | default | 3,043.31 | 0.89% | 1.48 | 5.71 | 7.13 | 71.34% | 30.72 MiB | 0 |
| cxxmcp | TCP_NODELAY | 3,036.82 | 0.77% | 1.49 | 5.78 | 7.25 | 70.73% | 33.50 MiB | 0 |
| Neumann-Labs/mcp-cpp | default | 675.95 | 0.04% | 40.80 | 41.85 | 42.02 | 38.44% | 54.26 MiB | 0 |
| Neumann-Labs/mcp-cpp | TCP_NODELAY | 3,555.02 | 0.57% | 0.38 | 1.65 | 2.50 | 191.82% | 43.45 MiB | 0 |

With `TCP_NODELAY` on, the p50 of hkr04/cpp-mcp fell from 40.77 ms to 0.32 ms,
and that of Neumann-Labs/mcp-cpp from 40.80 ms to 0.38 ms. FastMCPP's p95 fell
from 41.78 ms to 1.43 ms. Median throughput rose from 678.67 to 3,639.57 ops/s
for hkr04/cpp-mcp, from 1,123.39 to 3,565.90 for FastMCPP, and from 675.95 to
3,555.02 for Neumann-Labs/mcp-cpp. cxxmcp's median throughput changed by
-0.21%.

| C++ SDK | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| hkr04/cpp-mcp | TCP_NODELAY | 3,620.85 | 3,639.59 | 3,639.57 | 3 |
| FastMCPP | TCP_NODELAY | 3,529.34 | 3,565.90 | 3,575.04 | 2 |
| cxxmcp | TCP_NODELAY | 2,998.36 | 3,036.82 | 3,040.27 | 2 |
| Neumann-Labs/mcp-cpp | TCP_NODELAY | 3,521.83 | 3,555.02 | 3,557.98 | 2 |

## Official language SDKs (diagnostic)

These are standalone server applications written against each language's
official MCP SDK, not adapters over the shared C++ workload (see
[Language baselines](README.md#language-baselines)). The SDK versions below
were read from the measured images.

| SDK | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Python SDK (`mcp` 2.3.0) | 2024-11-05 | 1,067.43 | 1.67% | 15.05 | 66.65 | 88.80 | 203.22% | 297.60 MiB | 0 |
| Go SDK (`go-sdk` v1.2.0) | 2024-11-05 | 2,463.22 | 1.26% | 1.35 | 28.30 | 33.79 | 207.45% | 28.38 MiB | 0 |
| Rust SDK (`rmcp` 0.17.0) | 2024-11-05 | 3,687.20 | 0.06% | 0.39 | 1.66 | 2.42 | 52.85% | 30.73 MiB | 0 |

| SDK | Round 1 | Round 2 | Round 3 | Selected round |
|---|---:|---:|---:|---:|
| Python SDK | 1,037.63 | 1,067.43 | 1,068.84 | 2 |
| Go SDK | 2,463.22 | 2,510.62 | 2,451.64 | 1 |
| Rust SDK | 3,683.99 | 3,687.20 | 3,687.80 | 2 |

All three passed every eligibility gate, including the direct Redis side-effect
checks. The `adapter-exact-v1` checks are advisory for them. Go passed them.
Python and Rust failed one check each in all six of their gates: Python on the
checkout tool schema, and Rust on the checkout `rate_limit_count` fixture. In
those gates Rust reported 295 while Redis recorded the expected counter change
from 0 to 1.

## Audit evidence

The run completed and passed:

- 72/72 protocol gates (preflight and postflight for 36 measured rounds) under
  the universal contract, with the required `adapter-exact-v1` checks passing
  in all 54 gates of the nine C++ targets;
- 36/36 warmups and 36/36 measurements with zero MCP, HTTP, check, or session
  failures;
- 180/180 collector audits and 36/36 per-round resource/headroom reports;
- the `baseline-cohort-retire-v1` host network policy; and
- source tree digest checks after the last round, and immutable k6 and Compose
  input checks throughout the run.

The run measured commit `e30c35c82524473f1fd56f7423eeb6d349810b98` with a clean
working tree. Its deterministic source tree hash covers the SDK sources and the
benchmark harness at that commit, including the previous version of this file,
and is
`06f6b83e754b658e7be619aa3a0146e71beb06165bc1cc9db38ac5b4565b4486`.
The run's k6 and Compose inputs hash to
`0fbba8de4ea920ef9fede8a3208246d2339216b70ffea817befc6826bbb86e48`
and `a9d6c1e7501e67092207703482f5e065b19a1f57ff302be04618878dd590c7b3`.
