#!/usr/bin/env python3
"""Validate resource-sampling integrity and shared-resource headroom."""

from __future__ import annotations

import argparse
import json
import math
import re
from datetime import datetime
from pathlib import Path
from typing import Any


SAMPLE_FIELDS = {
    "timestamp",
    "cpu_percent",
    "mem_usage_bytes",
    "mem_limit_bytes",
    "net_io_rx",
    "net_io_tx",
}


def percentile(values: list[float], fraction: float) -> float:
    """Return a nearest-rank percentile without interpolating observations."""
    if not values:
        raise ValueError("at least one sample is required")
    rank = max(1, math.ceil(len(values) * fraction))
    return sorted(values)[rank - 1]


def parse_duration(value: str) -> float:
    match = re.fullmatch(r"\s*([0-9]*\.?[0-9]+)\s*(ms|s|m|h)?\s*", value)
    if not match:
        raise argparse.ArgumentTypeError(
            "duration must be seconds or use an ms, s, m, or h suffix"
        )
    units = {None: 1.0, "ms": 0.001, "s": 1.0, "m": 60.0, "h": 3600.0}
    seconds = float(match.group(1)) * units[match.group(2)]
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("duration must be greater than zero")
    return seconds


def audit_path_for(sample_path: Path) -> Path:
    return sample_path.with_name(f"{sample_path.stem}.audit.json")


def parse_timestamp(value: Any) -> datetime:
    if not isinstance(value, str):
        raise ValueError("timestamp is not a string")
    try:
        timestamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError(f"invalid ISO-8601 timestamp {value!r}") from error
    if timestamp.tzinfo is None or timestamp.utcoffset() is None:
        raise ValueError(f"timestamp lacks a UTC offset: {value!r}")
    return timestamp


def exact_nonnegative_integer(value: Any, description: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(float(value))
        or float(value) != int(value)
    ):
        raise ValueError(f"{description} must be an exact integer")
    parsed = int(value)
    if parsed < 0:
        raise ValueError(f"{description} must be non-negative")
    return parsed


def sampling_integrity(
    samples: list[dict[str, Any]],
    audit: dict[str, Any],
    memory_limit: int,
    expected_duration: float,
) -> dict[str, Any]:
    """Check audit status, schema, declared limits, and temporal coverage."""
    reasons: list[str] = []
    try:
        interval = float(audit["interval_seconds"])
    except (KeyError, TypeError, ValueError):
        interval = math.nan
        reasons.append("audit metadata has no valid interval_seconds")
    if not math.isfinite(interval) or interval <= 0:
        if not reasons:
            reasons.append("audit interval_seconds must be finite and positive")
        interval = math.nan

    if audit.get("schema_version") != 1:
        reasons.append("unsupported or missing audit schema_version")
    if audit.get("status") != "complete":
        reasons.append(f"collector status is {audit.get('status')!r}, not 'complete'")
    failure_count = audit.get("failure_count")
    if isinstance(failure_count, bool) or failure_count != 0:
        reasons.append(f"collector recorded {failure_count!r} failed attempts")
    recorded_failures = audit.get("failures")
    if not isinstance(recorded_failures, list):
        reasons.append("collector audit has no failures list")
    elif isinstance(failure_count, int) and not isinstance(failure_count, bool) \
            and len(recorded_failures) != failure_count:
        reasons.append("collector failure_count does not match its failures list")
    if audit.get("fatal_error") is not None:
        reasons.append(f"collector recorded fatal error: {audit.get('fatal_error')}")
    termination = audit.get("termination")
    if not isinstance(termination, dict) or termination.get("reason") != "signal":
        reasons.append("collector did not record a clean signal-driven stop")
    if audit.get("sample_count") != len(samples):
        reasons.append(
            "audit sample_count does not match the sample file "
            f"({audit.get('sample_count')!r} != {len(samples)})"
        )
    if audit.get("attempt_count") != len(samples):
        reasons.append(
            "collector attempts do not exactly match successful samples "
            f"({audit.get('attempt_count')!r} != {len(samples)})"
        )

    timestamps: list[datetime] = []
    cpu: list[float] = []
    memory: list[int] = []
    network: list[tuple[int, int, int]] = []
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            reasons.append(f"sample {index} is not an object")
            continue
        missing = SAMPLE_FIELDS - sample.keys()
        if missing:
            reasons.append(f"sample {index} is missing fields: {sorted(missing)}")
            continue
        try:
            timestamps.append(parse_timestamp(sample["timestamp"]))
            if (
                isinstance(sample["cpu_percent"], bool)
                or not isinstance(sample["cpu_percent"], (int, float))
            ):
                raise ValueError("cpu_percent must be numeric and not boolean")
            cpu_value = float(sample["cpu_percent"])
            memory_value = exact_nonnegative_integer(
                sample["mem_usage_bytes"], "mem_usage_bytes"
            )
            sample_memory_limit = exact_nonnegative_integer(
                sample["mem_limit_bytes"], "mem_limit_bytes"
            )
            net_rx = exact_nonnegative_integer(sample["net_io_rx"], "net_io_rx")
            net_tx = exact_nonnegative_integer(sample["net_io_tx"], "net_io_tx")
            if not math.isfinite(cpu_value) or cpu_value < 0:
                raise ValueError("cpu_percent must be finite and non-negative")
            if memory_value > memory_limit:
                raise ValueError("mem_usage_bytes exceeds the declared memory limit")
            cpu.append(cpu_value)
            memory.append(memory_value)
            network.append((index, net_rx, net_tx))
            if (
                sample_memory_limit != memory_limit
            ):
                reasons.append(
                    f"sample {index} reports mem_limit_bytes={sample_memory_limit}, "
                    f"expected {memory_limit}"
                )
        except (TypeError, ValueError) as error:
            reasons.append(f"sample {index} has invalid values: {error}")

    for earlier, later in zip(network, network[1:]):
        earlier_index, earlier_rx, earlier_tx = earlier
        later_index, later_rx, later_tx = later
        if later_rx < earlier_rx or later_tx < earlier_tx:
            reasons.append(
                "network counters decreased between samples "
                f"{earlier_index} and {later_index}"
            )

    minimum_samples = 3
    if math.isfinite(interval):
        minimum_samples = max(3, math.floor(expected_duration / interval) - 1)
    if len(samples) < minimum_samples:
        reasons.append(
            f"only {len(samples)} samples were collected; expected at least "
            f"{minimum_samples} for {expected_duration:g}s"
        )

    span_seconds = 0.0
    maximum_gap_seconds = 0.0
    if len(timestamps) >= 2:
        gaps = [
            (later - earlier).total_seconds()
            for earlier, later in zip(timestamps, timestamps[1:])
        ]
        if any(gap <= 0 for gap in gaps):
            reasons.append("sample timestamps are not strictly increasing")
        span_seconds = (timestamps[-1] - timestamps[0]).total_seconds()
        maximum_gap_seconds = max(gaps)
        if math.isfinite(interval):
            minimum_span = max(0.0, expected_duration - 2.0 * interval)
            if span_seconds < minimum_span:
                reasons.append(
                    f"sample timestamps cover only {span_seconds:.3f}s; "
                    f"expected at least {minimum_span:.3f}s"
                )
            maximum_allowed_gap = interval * 1.75
            if maximum_gap_seconds > maximum_allowed_gap:
                reasons.append(
                    f"maximum sample gap is {maximum_gap_seconds:.3f}s; "
                    f"allowed at most {maximum_allowed_gap:.3f}s"
                )
    elif samples:
        reasons.append("fewer than two valid timestamps were collected")

    elapsed = audit.get("elapsed_seconds")
    try:
        elapsed_seconds = float(elapsed)
    except (TypeError, ValueError):
        elapsed_seconds = math.nan
        reasons.append("audit metadata has no valid elapsed_seconds")
    if not math.isfinite(elapsed_seconds) or elapsed_seconds < 0:
        if not any("elapsed_seconds" in reason for reason in reasons):
            reasons.append("audit elapsed_seconds must be finite and non-negative")
        elapsed_seconds = math.nan
    if math.isfinite(elapsed_seconds) and math.isfinite(interval):
        minimum_elapsed = max(0.0, expected_duration - interval)
        if elapsed_seconds < minimum_elapsed:
            reasons.append(
                f"collector ran for only {elapsed_seconds:.3f}s; "
                f"expected at least {minimum_elapsed:.3f}s"
            )

    return {
        "valid": not reasons,
        "reason": "; ".join(reasons) if reasons else None,
        "sample_count": len(samples),
        "minimum_sample_count": minimum_samples,
        "expected_duration_seconds": expected_duration,
        "interval_seconds": interval if math.isfinite(interval) else None,
        "timestamp_span_seconds": span_seconds,
        "maximum_timestamp_gap_seconds": maximum_gap_seconds,
        "audit_elapsed_seconds": elapsed_seconds if math.isfinite(elapsed_seconds) else None,
        "cpu_values": cpu,
        "memory_values": memory,
    }


def evaluate(
    name: str,
    samples: list[dict[str, Any]],
    audit: dict[str, Any],
    cpu_limit: float,
    memory_limit: int,
    threshold: float,
    expected_duration: float,
    enforce_headroom: bool,
) -> dict[str, Any]:
    integrity = sampling_integrity(samples, audit, memory_limit, expected_duration)
    cpu = integrity.pop("cpu_values")
    memory = integrity.pop("memory_values")
    result: dict[str, Any] = {
        "name": name,
        "role": "shared" if enforce_headroom else "observed_target",
        "headroom_enforced": enforce_headroom,
        "collection": integrity,
    }

    if cpu and memory:
        cpu_capacity = cpu_limit * 100.0
        cpu_p95 = percentile(cpu, 0.95)
        memory_p95 = percentile([float(value) for value in memory], 0.95)
        cpu_ratio = cpu_p95 / cpu_capacity
        memory_ratio = memory_p95 / memory_limit
        headroom_reasons = []
        if cpu_ratio >= threshold:
            headroom_reasons.append(f"p95 CPU used {cpu_ratio:.1%} of its limit")
        if memory_ratio >= threshold:
            headroom_reasons.append(f"p95 memory used {memory_ratio:.1%} of its limit")
        result.update(
            {
                "cpu": {
                    "limit_cores": cpu_limit,
                    "p95_percent": cpu_p95,
                    "p95_limit_ratio": cpu_ratio,
                },
                "memory": {
                    "limit_bytes": memory_limit,
                    "p95_bytes": int(memory_p95),
                    "p95_limit_ratio": memory_ratio,
                },
                "headroom_exceeded": bool(headroom_reasons),
                "headroom_reason": "; ".join(headroom_reasons) or None,
            }
        )
    else:
        result["headroom_exceeded"] = None
        result["headroom_reason"] = "no valid samples available for p95 calculation"

    headroom_valid = not result["headroom_exceeded"] if enforce_headroom else True
    result["valid"] = bool(integrity["valid"] and headroom_valid)
    reasons = [integrity["reason"]] if integrity["reason"] else []
    if enforce_headroom and result["headroom_reason"]:
        reasons.append(result["headroom_reason"])
    result["reason"] = "; ".join(reasons) or None
    return result


def parse_resource(value: str) -> tuple[str, Path, float, int]:
    try:
        name, path, cpu_limit, memory_limit = value.split(":", 3)
        parsed_cpu = float(cpu_limit)
        parsed_memory = int(memory_limit)
        if (
            not name
            or not math.isfinite(parsed_cpu)
            or parsed_cpu <= 0
            or parsed_memory <= 0
        ):
            raise ValueError
        return name, Path(path), parsed_cpu, parsed_memory
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError(
            "resource must be NAME:PATH:CPU_CORES:MEMORY_BYTES with positive limits"
        ) from error


def load_resource(
    name: str,
    path: Path,
    cpu_limit: float,
    memory_limit: int,
    threshold: float,
    expected_duration: float,
    enforce_headroom: bool,
) -> dict[str, Any]:
    audit_path = audit_path_for(path)
    try:
        with path.open(encoding="utf-8") as stream:
            samples = json.load(stream)
        if not isinstance(samples, list):
            raise ValueError("sample JSON must contain a top-level list")
        with audit_path.open(encoding="utf-8") as stream:
            audit = json.load(stream)
        if not isinstance(audit, dict):
            raise ValueError("audit JSON must contain a top-level object")
    except (OSError, ValueError, json.JSONDecodeError) as error:
        return {
            "name": name,
            "role": "shared" if enforce_headroom else "observed_target",
            "headroom_enforced": enforce_headroom,
            "valid": False,
            "reason": f"unable to load samples and audit metadata: {error}",
            "sample_path": str(path),
            "audit_path": str(audit_path),
        }

    result = evaluate(
        name,
        samples,
        audit,
        cpu_limit,
        memory_limit,
        threshold,
        expected_duration,
        enforce_headroom,
    )
    result["sample_path"] = str(path)
    result["audit_path"] = str(audit_path)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--resource",
        action="append",
        type=parse_resource,
        default=[],
        help="shared NAME:PATH:CPU_CORES:MEMORY_BYTES (p95 headroom enforced)",
    )
    parser.add_argument(
        "--observed-resource",
        action="append",
        type=parse_resource,
        default=[],
        help="target NAME:PATH:CPU_CORES:MEMORY_BYTES (collection integrity only)",
    )
    parser.add_argument(
        "--expected-duration",
        "--expected-duration-seconds",
        dest="expected_duration",
        type=parse_duration,
        required=True,
        help="expected collection duration, for example 300, 300s, or 5m",
    )
    parser.add_argument("--threshold", type=float, default=0.90)
    args = parser.parse_args()
    if not 0 < args.threshold < 1:
        parser.error("--threshold must be between 0 and 1")
    if not args.resource and not args.observed_resource:
        parser.error("at least one --resource or --observed-resource is required")

    definitions = [
        (*resource, True) for resource in args.resource
    ] + [
        (*resource, False) for resource in args.observed_resource
    ]
    names = [definition[0] for definition in definitions]
    if len(names) != len(set(names)):
        parser.error("resource names must be unique")

    results = [
        load_resource(
            name,
            path,
            cpu_limit,
            memory_limit,
            args.threshold,
            args.expected_duration,
            enforce_headroom,
        )
        for name, path, cpu_limit, memory_limit, enforce_headroom in definitions
    ]
    report = {
        "valid": all(result["valid"] for result in results),
        "threshold": args.threshold,
        "expected_duration_seconds": args.expected_duration,
        "resources": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not report["valid"]:
        for result in results:
            if not result["valid"]:
                print(f"{result['name']}: {result['reason']}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
