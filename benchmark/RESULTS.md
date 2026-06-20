# Benchmark Results

This file keeps the benchmark records by run date:

- `20260509_122305`: full Rust, C++, Go, and Python comparison
- `20260620_220910`: C++ three-run verification

## Test Profile

| | |
|---|---|
| **Workload** | TM Dev Lab v2 MCP benchmark: Redis + HTTP I/O-bound tools |
| **Load** | 50 VUs, 15s ramp-up, 5m sustained load, 10s ramp-down |
| **Warmup** | 60s warmup excluded from metrics |
| **Repetition** | `run.sh` executes 3 k6 runs and selects the median by RPS |
| **Infrastructure** | Same upstream API service and Redis seeder, Docker Compose |
| **Host** | AMD Ryzen 9 9900X, 32 GB RAM, Ubuntu kernel 6.17.0-20-generic |

## 20260509_122305

Results from `benchmark/results/20260509_122305/`.

| Server | RPS | p50 (ms) | p99 (ms) | Error Rate | Memory |
|---|---:|---:|---:|---:|---:|
| Rust | 7,344 | 0.16 | 2.17 | 0% | 12.6 MB |
| C++ | 5,665 | 0.48 | 5.90 | 0% | 3.4 MB |
| Go | 5,349 | 0.34 | 31.45 | 0% | 12.2 MB |
| Python | 867 | 16.65 | 109.14 | 0% | 250.7 MB |

## 20260620_220910

C++-only verification from `benchmark/results/20260620_220910/`.

| Run | RPS |
|---:|---:|
| 1 | 6,995.45 |
| 2 | 7,025.13 |
| 3 | 7,031.65 |

| Metric | Value |
|---|---:|
| Median run | 2 |
| Median RPS | 7,025.13 |
| Mean RPS | 7,017.41 |
| CV | 0.22% |
| Requests | 2,283,376 |
| p50 | 0.68 ms |
| p95 | 1.92 ms |
| p99 | 2.69 ms |
| Error rate | 0% |

Compared with the `20260509_122305` C++ result, the `20260620_220910` median C++ run improved from **5,665 RPS** to **7,025.13 RPS** under the same benchmark profile.
