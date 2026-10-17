# Benchmark results

## Publication status

The `production-full` run `20261017_161956_production-full` completed every gate.
Its manifest is `complete` with `publishable_candidate: true`. The publishable
comparison is the five-C++-SDK table below. The TCP_NODELAY builds and the
official language SDKs ran in the same run, but their tables are diagnostic.

The evidence bundle is retained locally at
`benchmark/results/20261017_161956_production-full/`. Generated result
directories are intentionally ignored by Git; an external citation should
include an unchanged copy of that bundle. The SHA-256 of its sorted per-file
checksum stream is
`d96fc52912348cbb01bbb94e4968ddec3820101dc9ec520cf32fb9219b6cc708`.

## Run configuration

The run measured all twelve targets in three counterbalanced rounds (order seed
1792221596). Each round used 50 VUs, a separate 15-second ramp plus 60-second
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
| This SDK | 2024-11-05 | 3,506.24 | 0.58% | 0.48 | 1.87 | 2.73 | 162.71% | 44.13 MiB | 0 |
| hkr04/cpp-mcp | 2025-03-26 | 677.63 | 0.16% | 40.74 | 41.83 | 42.13 | 23.82% | 30.87 MiB | 0 |
| FastMCPP | 2024-11-05 | 1,121.86 | 0.08% | 0.39 | 41.78 | 42.07 | 42.29% | 34.64 MiB | 0 |
| cxxmcp | 2024-11-05 | 2,926.80 | 1.42% | 1.75 | 6.56 | 8.21 | 76.52% | 28.20 MiB | 0 |
| Neumann-Labs/mcp-cpp | 2025-11-25 | 674.06 | 0.49% | 40.82 | 41.89 | 42.82 | 44.11% | 56.85 MiB | 0 |

The measured operation rates for all rounds were:

| C++ SDK | Round 1 | Round 2 | Round 3 | Selected round |
|---|---:|---:|---:|---:|
| This SDK | 3,506.24 | 3,495.57 | 3,535.27 | 1 |
| hkr04/cpp-mcp | 676.28 | 677.63 | 678.48 | 2 |
| FastMCPP | 1,123.37 | 1,121.86 | 1,121.72 | 2 |
| cxxmcp | 2,996.54 | 2,921.10 | 2,926.80 | 3 |
| Neumann-Labs/mcp-cpp | 669.30 | 674.06 | 675.70 | 2 |

These numbers describe this pinned MCP/Redis/HTTP workload and environment;
they are not a general-purpose application-performance claim.

## TCP_NODELAY builds (diagnostic)

Each `-nodelay` target is the same adapter as the default build, compiled with
`CPPHTTPLIB_TCP_NODELAY=true` (see the
[README](README.md#tcp_nodelay-builds)). Each pair below comes from the same
run:

| C++ SDK | Build | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| hkr04/cpp-mcp | default | 677.63 | 0.16% | 40.74 | 41.83 | 42.13 | 23.82% | 30.87 MiB | 0 |
| hkr04/cpp-mcp | TCP_NODELAY | 3,566.60 | 1.65% | 0.35 | 1.79 | 3.11 | 111.78% | 25.75 MiB | 0 |
| FastMCPP | default | 1,121.86 | 0.08% | 0.39 | 41.78 | 42.07 | 42.29% | 34.64 MiB | 0 |
| FastMCPP | TCP_NODELAY | 3,503.43 | 2.53% | 0.40 | 1.62 | 2.46 | 120.73% | 34.21 MiB | 0 |
| cxxmcp | default | 2,926.80 | 1.42% | 1.75 | 6.56 | 8.21 | 76.52% | 28.20 MiB | 0 |
| cxxmcp | TCP_NODELAY | 2,909.02 | 2.89% | 1.78 | 6.72 | 8.42 | 76.40% | 31.75 MiB | 0 |
| Neumann-Labs/mcp-cpp | default | 674.06 | 0.49% | 40.82 | 41.89 | 42.82 | 44.11% | 56.85 MiB | 0 |
| Neumann-Labs/mcp-cpp | TCP_NODELAY | 3,477.41 | 0.98% | 0.43 | 1.92 | 3.19 | 204.71% | 53.41 MiB | 0 |

With `TCP_NODELAY` on, the p50 of hkr04/cpp-mcp fell from 40.74 ms to 0.35 ms,
and that of Neumann-Labs/mcp-cpp from 40.82 ms to 0.43 ms. FastMCPP's p95 fell
from 41.78 ms to 1.62 ms. Median throughput rose from 677.63 to 3,566.60 ops/s
for hkr04/cpp-mcp, from 1,121.86 to 3,503.43 for FastMCPP, and from 674.06 to
3,477.41 for Neumann-Labs/mcp-cpp. cxxmcp's median throughput changed by
-0.61%.

| C++ SDK | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| hkr04/cpp-mcp | TCP_NODELAY | 3,566.60 | 3,506.59 | 3,624.21 | 1 |
| FastMCPP | TCP_NODELAY | 3,549.17 | 3,379.30 | 3,503.43 | 3 |
| cxxmcp | TCP_NODELAY | 3,001.78 | 2,833.38 | 2,909.02 | 3 |
| Neumann-Labs/mcp-cpp | TCP_NODELAY | 3,530.55 | 3,466.45 | 3,477.41 | 3 |

## Official language SDKs (diagnostic)

These are standalone server applications written against each language's
official MCP SDK, not adapters over the shared C++ workload (see
[Language baselines](README.md#language-baselines)). The SDK versions below
were read from the measured images.

| SDK | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Python SDK (`mcp` 2.3.0) | 2024-11-05 | 1,003.23 | 3.24% | 16.63 | 68.83 | 94.52 | 204.77% | 295.40 MiB | 0 |
| Go SDK (`go-sdk` v1.2.0) | 2024-11-05 | 2,438.29 | 0.60% | 1.38 | 28.76 | 33.90 | 206.83% | 22.80 MiB | 0 |
| Rust SDK (`rmcp` 0.17.0) | 2024-11-05 | 3,661.40 | 0.66% | 0.43 | 1.76 | 2.60 | 58.25% | 19.48 MiB | 0 |

| SDK | Round 1 | Round 2 | Round 3 | Selected round |
|---|---:|---:|---:|---:|
| Python SDK | 986.51 | 1,003.23 | 1,049.93 | 2 |
| Go SDK | 2,438.29 | 2,432.06 | 2,459.95 | 1 |
| Rust SDK | 3,661.40 | 3,626.40 | 3,672.20 | 1 |

All three passed every eligibility gate, including the direct Redis side-effect
checks. The `adapter-exact-v1` checks are advisory for them. Go passed them.
Python and Rust failed one check each in all six of their gates: Python on the
checkout tool schema, and Rust on the checkout `rate_limit_count` fixture. In
those gates Rust reported 295 while Redis recorded the expected counter change
from 0 to 1.

## Audit evidence

The completed bundle records and passed:

- 72/72 protocol gates (preflight and postflight for 36 measured rounds) under
  the universal contract, with the required `adapter-exact-v1` checks passing
  in all 54 gates of the nine C++ targets;
- 36/36 warmups and 36/36 measurements with zero MCP, HTTP, check, or session
  failures;
- 180/180 collector audits and 36/36 per-round resource/headroom reports;
- the `baseline-cohort-retire-v1` host network policy; and
- source tree digest checks after the last round, and immutable k6 and Compose
  input checks throughout the run.

The deterministic source tree hash is
`1e24d5dbc131a86aa152b15bd4e64937f445a69dce72ddd96f47872ab563b439`.
`source_snapshot.tar` hashes to
`fefcce4341fa75f2a27430e879199bc1401630660176f97e88482a068ab345bf`.
The archived k6 and Compose inputs hash to
`0fbba8de4ea920ef9fede8a3208246d2339216b70ffea817befc6826bbb86e48`
and `a9d6c1e7501e67092207703482f5e065b19a1f57ff302be04618878dd590c7b3`.
