# Benchmark results

## Publication status

The five-C++ production run `20260720_012851_production` completed every gate
and passed an independent artifact audit. Its manifest is `complete` with
`publishable_candidate: true`.

The evidence bundle is retained locally at
`benchmark/results/20260720_012851_production/`. Generated result directories
are intentionally ignored by Git; an external citation should include an
unchanged copy of that bundle. The SHA-256 of its sorted per-file checksum
stream is
`e0d1807913cba76053dc9ba1596ac5474b29ac33fa653e4f39a9eb9e3ba72316`.

## Production result

The run used three counterbalanced rounds (order seed 417), 50 VUs, a separate
15-second ramp plus 60-second warmup, and an exact five-minute constant-load
measurement for every SDK/round pair. Each target had the same 2-CPU, 2-GiB,
and 50-request admission limits. Every row used the shared
`upstream-v2-strict-mcp-v1` measured contract; required exact C++ adapter checks
ran only in pre/postflight.

| C++ SDK | Protocol | Median ops/s | Sample CV | p50 ms | p95 ms | p99 ms | Target p95 CPU | Target p95 memory | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| This SDK | 2024-11-05 | 3,523.41 | 0.12% | 0.49 | 1.76 | 2.56 | 174.54% | 43.27 MiB | 0 |
| hkr04/cpp-mcp | 2025-03-26 | 678.33 | 0.04% | 40.77 | 41.83 | 41.93 | 22.12% | 32.65 MiB | 0 |
| FastMCPP | 2024-11-05 | 1,123.07 | 0.03% | 0.38 | 41.77 | 42.07 | 42.94% | 26.33 MiB | 0 |
| cxxmcp | 2024-11-05 | 2,987.82 | 0.56% | 1.64 | 6.12 | 7.67 | 72.46% | 30.14 MiB | 0 |
| Neumann-Labs/mcp-cpp | 2025-11-25 | 676.07 | 0.05% | 40.80 | 41.85 | 42.09 | 41.62% | 56.34 MiB | 0 |

CPU follows Docker's convention: 100% is one fully used core, and each target
had a two-core limit. Resource values are from the same run selected for that
SDK's median throughput. Target saturation is reported, while headroom gates
apply to Redis, the API service, k6, and the host; every shared-resource gate
passed.

The measured operation rates for all rounds were:

| C++ SDK | Round 1 | Round 2 | Round 3 | Selected round |
|---|---:|---:|---:|---:|
| This SDK | 3,516.55 | 3,524.21 | 3,523.41 | 3 |
| hkr04/cpp-mcp | 678.49 | 678.33 | 677.99 | 2 |
| FastMCPP | 1,122.93 | 1,123.66 | 1,123.07 | 3 |
| cxxmcp | 2,987.82 | 2,959.55 | 2,988.71 | 1 |
| Neumann-Labs/mcp-cpp | 675.50 | 676.07 | 676.15 | 2 |

These numbers describe this pinned MCP/Redis/HTTP workload and environment;
they are not a general-purpose application-performance claim.

## Audit evidence

The completed bundle records and passed:

- 30/30 protocol gates (preflight and postflight for 15 measured rounds),
  including the universal contract, required supplemental adapter validation,
  stable negotiation, and direct Redis counter/history/popularity changes;
- 15/15 warmups and 15/15 measurements with zero MCP, HTTP, check, or session
  failures;
- 75/75 collector audits and every per-round resource/headroom report;
- the `baseline-cohort-retire-v1` host network policy, with retirement and
  ignored-new interface sets preserved and coverage labeled per selected run;
  and
- final source, dependency, image, Compose, and immutable harness checks.

The deterministic source tree hash is
`b8f31b64191f848271f8a12a4a42a352838f596c686de7d5493c61fecb967a6a`.
`source_snapshot.tar` hashes to
`7c87baf6498724fe47444c98c1b38be0871b3538734937af165ee22147a304dd`.
The archived k6 and Compose inputs hash to
`20393aa7219fac9473ff3ef601555a7619548d06ae59af258fe78ec144599b4e`
and `d100fc6d99d76d00a88eb0a7a0db2d1d43d4e589289d89a68f25e2af7add9292`.

## Language diagnostic rerun

The separate `20260720_030804_baseline-diagnostic` run also completed all nine
Rust, Go, and Python rounds with zero measured errors, valid universal
pre/postflight checks, direct Redis side effects, complete collectors, and
valid resource reports. It is intentionally not included in the production
table: its manifest is diagnostic-only and never an equal-work C++ ranking.

The advisory layer preserves known upstream differences without changing
eligibility: Python emits a permissive array-item schema, and Rust returns a
mislabeled checkout count while independently producing the correct Redis
mutations. Neither difference was part of the inherited measured contract.
