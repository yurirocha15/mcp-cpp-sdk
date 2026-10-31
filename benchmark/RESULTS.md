# Benchmark results

These results come from the `production-full` run
`20261018_020318_production-full`, which put the same MCP/Redis/HTTP workload on
every target. The [C++ SDKs](#c-sdks) table is the comparison; the other tables
are diagnostic. [Evidence bundle](#evidence-bundle) at the end says how to check
the run.

## Run configuration

The run measured all twelve targets in three counterbalanced rounds (order seed
1792256598). Each round used 50 VUs, a separate 15-second ramp plus 60-second
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
| This SDK | 2024-11-05 | 3,511.17 | 0.29% | 0.49 | 1.96 | 2.84 | 156.08% | 45.38 MiB | 0 |
| hkr04/cpp-mcp | 2025-03-26 | 678.33 | 0.02% | 40.76 | 41.83 | 41.93 | 21.09% | 24.64 MiB | 0 |
| FastMCPP | 2024-11-05 | 1,123.51 | 0.03% | 0.35 | 41.80 | 42.09 | 37.98% | 34.47 MiB | 0 |
| cxxmcp | 2024-11-05 | 2,935.91 | 0.53% | 1.74 | 6.44 | 8.07 | 78.24% | 37.28 MiB | 0 |
| Neumann-Labs/mcp-cpp | 2025-11-25 | 675.82 | 0.03% | 40.80 | 41.86 | 42.01 | 35.96% | 55.69 MiB | 0 |

hkr04/cpp-mcp, FastMCPP, cxxmcp, and Neumann-Labs/mcp-cpp leave `TCP_NODELAY`
off. The same adapters built with it on are measured in
[TCP_NODELAY builds](#tcp_nodelay-builds-diagnostic).

The measured operation rates for all rounds were:

| C++ SDK | Round 1 | Round 2 | Round 3 | Selected round |
|---|---:|---:|---:|---:|
| This SDK | 3,511.17 | 3,513.23 | 3,494.97 | 1 |
| hkr04/cpp-mcp | 678.33 | 678.25 | 678.47 | 1 |
| FastMCPP | 1,123.96 | 1,123.51 | 1,123.28 | 2 |
| cxxmcp | 2,935.91 | 2,939.83 | 2,910.99 | 1 |
| Neumann-Labs/mcp-cpp | 675.81 | 675.82 | 676.14 | 2 |

These numbers describe this pinned MCP/Redis/HTTP workload and environment;
they are not a general-purpose application-performance claim.

## TCP_NODELAY builds (diagnostic)

Each `-nodelay` target is the same adapter as the default build, compiled with
`CPPHTTPLIB_TCP_NODELAY=true` (see the
[README](README.md#tcp_nodelay-builds)). Each pair below comes from the same
run:

| C++ SDK | Build | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| hkr04/cpp-mcp | default | 678.33 | 0.02% | 40.76 | 41.83 | 41.93 | 21.09% | 24.64 MiB | 0 |
| hkr04/cpp-mcp | TCP_NODELAY | 3,592.47 | 0.04% | 0.36 | 1.66 | 2.54 | 97.69% | 31.95 MiB | 0 |
| FastMCPP | default | 1,123.51 | 0.03% | 0.35 | 41.80 | 42.09 | 37.98% | 34.47 MiB | 0 |
| FastMCPP | TCP_NODELAY | 3,501.11 | 0.08% | 0.40 | 1.62 | 2.47 | 119.59% | 34.33 MiB | 0 |
| cxxmcp | default | 2,935.91 | 0.53% | 1.74 | 6.44 | 8.07 | 78.24% | 37.28 MiB | 0 |
| cxxmcp | TCP_NODELAY | 2,932.93 | 0.16% | 1.73 | 6.49 | 8.12 | 76.77% | 32.36 MiB | 0 |
| Neumann-Labs/mcp-cpp | default | 675.82 | 0.03% | 40.80 | 41.86 | 42.01 | 35.96% | 55.69 MiB | 0 |
| Neumann-Labs/mcp-cpp | TCP_NODELAY | 3,482.83 | 0.09% | 0.43 | 1.90 | 3.19 | 204.10% | 46.76 MiB | 0 |

With `TCP_NODELAY` on, the p50 of hkr04/cpp-mcp fell from 40.76 ms to 0.36 ms,
and that of Neumann-Labs/mcp-cpp from 40.80 ms to 0.43 ms. FastMCPP's p95 fell
from 41.80 ms to 1.62 ms. Median throughput rose from 678.33 to 3,592.47 ops/s
for hkr04/cpp-mcp, from 1,123.51 to 3,501.11 for FastMCPP, and from 675.82 to
3,482.83 for Neumann-Labs/mcp-cpp. cxxmcp's median throughput changed by
-0.10%.

| C++ SDK | Build | Round 1 | Round 2 | Round 3 | Selected round |
|---|---|---:|---:|---:|---:|
| hkr04/cpp-mcp | TCP_NODELAY | 3,590.95 | 3,592.47 | 3,593.71 | 2 |
| FastMCPP | TCP_NODELAY | 3,504.12 | 3,498.86 | 3,501.11 | 3 |
| cxxmcp | TCP_NODELAY | 2,930.06 | 2,932.93 | 2,939.13 | 2 |
| Neumann-Labs/mcp-cpp | TCP_NODELAY | 3,486.76 | 3,480.36 | 3,482.83 | 3 |

## Official language SDKs (diagnostic)

These are standalone server applications written against each language's
official MCP SDK, not adapters over the shared C++ workload (see
[Language baselines](README.md#language-baselines)). The SDK versions below
were read from the measured images.

| SDK | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Python SDK (`mcp` 2.3.0) | 2024-11-05 | 992.79 | 0.64% | 17.01 | 68.63 | 94.53 | 204.00% | 275.40 MiB | 0 |
| Go SDK (`go-sdk` v1.2.0) | 2024-11-05 | 2,388.24 | 0.84% | 1.47 | 29.25 | 34.16 | 207.25% | 28.97 MiB | 0 |
| Rust SDK (`rmcp` 0.17.0) | 2024-11-05 | 3,640.86 | 0.05% | 0.46 | 1.93 | 2.80 | 56.51% | 16.79 MiB | 0 |

| SDK | Round 1 | Round 2 | Round 3 | Selected round |
|---|---:|---:|---:|---:|
| Python SDK | 1,001.70 | 989.30 | 992.79 | 3 |
| Go SDK | 2,380.27 | 2,388.24 | 2,418.24 | 2 |
| Rust SDK | 3,638.93 | 3,640.86 | 3,642.27 | 2 |

All three passed every eligibility gate, including the direct Redis side-effect
checks. The `adapter-exact-v1` checks are advisory for them. Go passed them.
Python and Rust failed one check each in all six of their gates: Python on the
checkout tool schema, and Rust on the checkout `rate_limit_count` fixture. In
those gates Rust reported 295 while Redis recorded the expected counter change
from 0 to 1.

## Evidence bundle

The run completed every gate. Its manifest is `complete` with `publishable_candidate: true`. The publishable
comparison is the five-C++-SDK table above. The TCP_NODELAY builds and the
official language SDKs ran in the same run, but their tables are diagnostic.

The evidence bundle is retained locally at
`benchmark/results/20261018_020318_production-full/`. Generated result
directories are intentionally ignored by Git; an external citation should
include an unchanged copy of that bundle. The SHA-256 of its sorted per-file
checksum stream is
`2923ba1ffcf102c637e4666f4c8133a31e44891e5ee9f75a5ba34f51a365b4cd`.
To recompute it, run this from inside the bundle directory:

```bash
find . -type f | LC_ALL=C sort | sed 's|^\./||' | xargs -d '\n' sha256sum | sha256sum
```

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

The run measured commit `a68b77f3f0cfb0812cf4dbf1e76cd87c495c0f17` with a clean
working tree. Its deterministic source tree hash covers that commit's `benchmark/`
directory, including the previous version of this file, and is
`a741bbfdcc87165b19b4087079d5b0af58196826fff15e2be3b5a9c35b2eef67`.
`source_snapshot.tar` hashes to
`32897301e7fec5b91c2d48d19c416b8b0a880b9e5d92d818eeb144330a7d283d`.
The archived k6 and Compose inputs hash to
`0fbba8de4ea920ef9fede8a3208246d2339216b70ffea817befc6826bbb86e48`
and `a9d6c1e7501e67092207703482f5e065b19a1f57ff302be04618878dd590c7b3`.
