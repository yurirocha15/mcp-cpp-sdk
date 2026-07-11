# Benchmark Results

This file keeps the benchmark records by run date:

- `20260405_205033`: C++, Go, and Python comparison
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

## 20260405_205033

Results from `benchmark/results/20260405_205033/`.

| Server | RPS | p50 (ms) | p99 (ms) | Error Rate | Avg Memory | Max Memory |
|---|---:|---:|---:|---:|---:|---:|
| C++ | 12,191.61 | 0.31 | 5.55 | 0% | 11.34 MB | 13.07 MB |
| Go | 9,154.28 | 0.36 | 36.06 | 0% | 21.38 MB | 24.52 MB |
| Python | 904.33 | 18.17 | 190.20 | 0% | 58.09 MB | 61.86 MB |

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
| Avg memory | 6.92 MB |
| Max memory | 8.15 MB |

Compared with the `20260405_205033` C++ result, the `20260620_220910` median C++ run used less Docker-reported memory: average memory decreased from **11.34 MB** to **6.92 MB**, and max memory decreased from **13.07 MB** to **8.15 MB**.
