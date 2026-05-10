#!/usr/bin/env python3
"""
Select median k6 benchmark run by RPS and generate multi-run statistics.

Usage:
  select_median_run.py <results_dir> <num_runs>

Reads k6_summary_run{1..N}.json files, selects the median by RPS, copies it
to k6_summary.json, and writes k6_multi_run_stats.json with CV% and per-run stats.
"""

import json
import sys
import os
import math
import shutil


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <results_dir> <num_runs>", file=sys.stderr)
        sys.exit(1)

    results_dir = sys.argv[1]
    num_runs = int(sys.argv[2])

    # Load all runs and extract RPS
    runs = []
    for i in range(1, num_runs + 1):
        path = os.path.join(results_dir, f'k6_summary_run{i}.json')
        with open(path) as f:
            data = json.load(f)
        rps = data.get('http', {}).get('rps', 0)
        runs.append((rps, i, path))

    # Sort by RPS and select median
    runs.sort(key=lambda x: x[0])
    median_idx = len(runs) // 2
    median_rps, median_run, median_path = runs[median_idx]

    # Compute coefficient of variation (CV%)
    rps_values = [r[0] for r in runs]
    mean_rps = sum(rps_values) / len(rps_values)
    if mean_rps > 0:
        variance = sum((v - mean_rps) ** 2 for v in rps_values) / len(rps_values)
        std_dev = math.sqrt(variance)
        cv_pct = (std_dev / mean_rps) * 100
    else:
        cv_pct = 0.0

    # Copy median run as canonical summary
    canonical = os.path.join(results_dir, 'k6_summary.json')
    shutil.copy2(median_path, canonical)

    # Write per-run stats and CV%
    stats = {
        'runs': [{'run': r[1], 'rps': r[0]} for r in sorted(runs, key=lambda x: x[1])],
        'median_run': median_run,
        'median_rps': median_rps,
        'mean_rps': mean_rps,
        'cv_pct': round(cv_pct, 2),
    }
    with open(os.path.join(results_dir, 'k6_multi_run_stats.json'), 'w') as f:
        json.dump(stats, f, indent=2)

    print(f'Median run: {median_run} (RPS={median_rps:.2f}), CV%={cv_pct:.2f}%')


if __name__ == '__main__':
    main()
