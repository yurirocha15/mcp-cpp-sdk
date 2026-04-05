#!/usr/bin/env python3

import json
import re
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


RUNNING = True
SAMPLES = []
OUTPUT_PATH: Path | None = None


SIZE_UNITS = {
    "b": 1,
    "kb": 1000,
    "mb": 1000**2,
    "gb": 1000**3,
    "tb": 1000**4,
    "kib": 1024,
    "mib": 1024**2,
    "gib": 1024**3,
    "tib": 1024**4,
}


def parse_size_to_bytes(value: str) -> int:
    text = value.strip().lower()
    if text in {"", "0", "0b", "--"}:
        return 0

    match = re.match(r"^([0-9]*\.?[0-9]+)\s*([a-z]+)$", text)
    if not match:
        raise ValueError(f"Unable to parse size value: {value!r}")

    number = float(match.group(1))
    unit = match.group(2)
    if unit not in SIZE_UNITS:
        raise ValueError(f"Unknown size unit in value: {value!r}")

    return int(number * SIZE_UNITS[unit])


def parse_cpu_percent(value: str) -> float:
    text = value.strip().replace("%", "")
    return float(text) if text else 0.0


def parse_mem_usage(value: str) -> tuple[int, int]:
    parts = [p.strip() for p in value.split("/")]
    if len(parts) != 2:
        raise ValueError(f"Unexpected memory usage format: {value!r}")
    return parse_size_to_bytes(parts[0]), parse_size_to_bytes(parts[1])


def parse_net_io(value: str) -> tuple[int, int]:
    parts = [p.strip() for p in value.split("/")]
    if len(parts) != 2:
        raise ValueError(f"Unexpected net I/O format: {value!r}")
    return parse_size_to_bytes(parts[0]), parse_size_to_bytes(parts[1])


def write_samples() -> None:
    global OUTPUT_PATH
    if OUTPUT_PATH is None:
        return

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT_PATH.open("w", encoding="utf-8") as f:
        json.dump(SAMPLES, f, indent=2)


def handle_signal(_signum: int, _frame) -> None:
    global RUNNING
    RUNNING = False


def collect_once(container_name: str) -> dict:
    format_str = "{{.CPUPerc}}|{{.MemUsage}}|{{.NetIO}}"
    cmd = [
        "docker",
        "stats",
        "--no-stream",
        "--format",
        format_str,
        container_name,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)

    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or proc.stdout.strip() or "docker stats failed")

    line = proc.stdout.strip()
    if not line:
        raise RuntimeError("docker stats returned empty output")

    parts = line.split("|")
    if len(parts) != 3:
        raise RuntimeError(f"Unexpected docker stats format: {line!r}")

    cpu_percent = parse_cpu_percent(parts[0])
    mem_usage_bytes, mem_limit_bytes = parse_mem_usage(parts[1])
    net_io_rx, net_io_tx = parse_net_io(parts[2])

    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "cpu_percent": cpu_percent,
        "mem_usage_bytes": mem_usage_bytes,
        "mem_limit_bytes": mem_limit_bytes,
        "net_io_rx": net_io_rx,
        "net_io_tx": net_io_tx,
    }


def main() -> int:
    global OUTPUT_PATH

    if len(sys.argv) != 4:
        print(
            "Usage: collect_stats.py <container_name> <output_path> <interval_seconds>",
            file=sys.stderr,
        )
        return 1

    container_name = sys.argv[1]
    OUTPUT_PATH = Path(sys.argv[2])
    try:
        interval_seconds = float(sys.argv[3])
        if interval_seconds <= 0:
            raise ValueError("interval_seconds must be > 0")
    except ValueError as exc:
        print(f"Invalid interval_seconds: {exc}", file=sys.stderr)
        return 1

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    exit_code = 0
    try:
        while RUNNING:
            try:
                sample = collect_once(container_name)
                SAMPLES.append(sample)
            except Exception as exc:
                print(f"Warning: failed to collect stats: {exc}", file=sys.stderr)

            slept = 0.0
            while RUNNING and slept < interval_seconds:
                chunk = min(0.2, interval_seconds - slept)
                time.sleep(chunk)
                slept += chunk
    except Exception as exc:
        print(f"Fatal error in collector loop: {exc}", file=sys.stderr)
        exit_code = 1
    finally:
        try:
            write_samples()
        except Exception as exc:
            print(f"Failed to write output JSON: {exc}", file=sys.stderr)
            return 1

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
