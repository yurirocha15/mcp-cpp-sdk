#!/usr/bin/env python3
"""Capture and validate per-run Docker resource and image provenance."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


GIBIBYTE = 1_073_741_824


class CaptureError(RuntimeError):
    """Raised when Docker metadata is missing or the resource contract is violated."""


def run_command(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(command), capture_output=True, text=True, check=False
        )
    except OSError as error:
        raise CaptureError(f"could not execute {command[0]!r}: {error}") from error


def required_command(command: Sequence[str], description: str) -> str:
    completed = run_command(command)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "unknown error"
        raise CaptureError(f"could not capture {description}: {detail}")
    if not completed.stdout.strip():
        raise CaptureError(f"could not capture {description}: command produced no output")
    return completed.stdout


def inspect_one(
    command: Sequence[str], description: str
) -> tuple[str, dict[str, Any]]:
    raw = required_command(command, description)
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as error:
        raise CaptureError(f"{description} was not valid JSON") from error
    if (
        not isinstance(value, list)
        or len(value) != 1
        or not isinstance(value[0], dict)
    ):
        raise CaptureError(f"expected exactly one {description} record")
    return raw, value[0]


def container_file(container: str, path: str) -> dict[str, Any]:
    completed = run_command(["docker", "exec", container, "cat", path])
    return {
        "path": path,
        "available": completed.returncode == 0,
        "value": completed.stdout.strip() if completed.returncode == 0 else None,
        "error": completed.stderr.strip() if completed.returncode != 0 else None,
    }


def first_available(container: str, paths: Sequence[str]) -> dict[str, Any]:
    attempts = [container_file(container, path) for path in paths]
    selected = next((attempt for attempt in attempts if attempt["available"]), None)
    return {"selected": selected, "attempts": attempts}


def detect_cgroup(container: str) -> dict[str, Any]:
    version_probe = container_file(container, "/sys/fs/cgroup/cgroup.controllers")
    if version_probe["available"]:
        return {
            "version": 2,
            "cpu": first_available(container, ["/sys/fs/cgroup/cpu.max"]),
            "memory": first_available(container, ["/sys/fs/cgroup/memory.max"]),
            "cpuset_effective": first_available(
                container, ["/sys/fs/cgroup/cpuset.cpus.effective"]
            ),
            "controllers": version_probe,
        }

    return {
        "version": 1,
        "cpu_quota": first_available(
            container,
            [
                "/sys/fs/cgroup/cpu/cpu.cfs_quota_us",
                "/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us",
            ],
        ),
        "cpu_period": first_available(
            container,
            [
                "/sys/fs/cgroup/cpu/cpu.cfs_period_us",
                "/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us",
            ],
        ),
        "memory": first_available(
            container,
            [
                "/sys/fs/cgroup/memory/memory.limit_in_bytes",
                "/sys/fs/cgroup/memory.limit_in_bytes",
            ],
        ),
        "cpuset_effective": first_available(
            container,
            [
                "/sys/fs/cgroup/cpuset/cpuset.effective_cpus",
                "/sys/fs/cgroup/cpuset/cpuset.cpus",
                "/sys/fs/cgroup/cpuset.cpus.effective",
                "/sys/fs/cgroup/cpuset.cpus",
            ],
        ),
        "controllers": version_probe,
    }


def selected_value(record: dict[str, Any]) -> str | None:
    selected = record.get("selected")
    if not selected:
        return None
    value = selected.get("value")
    return str(value) if value is not None else None


def parse_cpu_set(value: str | None) -> set[int] | None:
    if value is None or not value.strip():
        return None
    cpus: set[int] = set()
    try:
        for field in value.split(","):
            bounds = field.strip().split("-", 1)
            start = int(bounds[0])
            end = int(bounds[1]) if len(bounds) == 2 else start
            if start < 0 or end < start:
                return None
            cpus.update(range(start, end + 1))
    except ValueError:
        return None
    return cpus


def validate_resources(
    container_inspect: dict[str, Any],
    cgroup: dict[str, Any],
    expected_cpus: float,
    expected_memory_bytes: int,
    expected_cpuset: str | None = None,
) -> dict[str, Any]:
    host_config = container_inspect.get("HostConfig") or {}
    expected_nano_cpus = round(expected_cpus * 1_000_000_000)
    checks: dict[str, dict[str, Any]] = {
        "host_nano_cpus": {
            "expected": expected_nano_cpus,
            "actual": host_config.get("NanoCpus"),
        },
        "host_memory_bytes": {
            "expected": expected_memory_bytes,
            "actual": host_config.get("Memory"),
        },
    }
    checks["host_nano_cpus"]["ok"] = (
        checks["host_nano_cpus"]["actual"] == expected_nano_cpus
    )
    checks["host_memory_bytes"]["ok"] = (
        checks["host_memory_bytes"]["actual"] == expected_memory_bytes
    )

    if expected_cpuset is not None:
        expected_cpu_set = parse_cpu_set(expected_cpuset)
        host_cpu_set = parse_cpu_set(host_config.get("CpusetCpus"))
        cgroup_cpu_set = parse_cpu_set(
            selected_value(cgroup["cpuset_effective"])
        )
        checks["host_cpuset"] = {
            "expected": expected_cpuset,
            "actual": host_config.get("CpusetCpus"),
            "ok": expected_cpu_set is not None and host_cpu_set == expected_cpu_set,
        }
        checks["cgroup_cpuset_effective"] = {
            "expected": expected_cpuset,
            "actual": selected_value(cgroup["cpuset_effective"]),
            "ok": expected_cpu_set is not None and cgroup_cpu_set == expected_cpu_set,
        }

    if cgroup["version"] == 2:
        cpu_value = selected_value(cgroup["cpu"])
        cpu_fields = cpu_value.split() if cpu_value else []
        quota = (
            int(cpu_fields[0])
            if len(cpu_fields) == 2 and cpu_fields[0].isdigit()
            else None
        )
        period = (
            int(cpu_fields[1])
            if len(cpu_fields) == 2 and cpu_fields[1].isdigit()
            else None
        )
    else:
        quota_value = selected_value(cgroup["cpu_quota"])
        period_value = selected_value(cgroup["cpu_period"])
        quota = (
            int(quota_value)
            if quota_value and quota_value.lstrip("-").isdigit()
            else None
        )
        period = int(period_value) if period_value and period_value.isdigit() else None

    memory_value = selected_value(cgroup["memory"])
    memory_limit = int(memory_value) if memory_value and memory_value.isdigit() else None
    checks["cgroup_cpu_quota"] = {
        "expected_cpus": expected_cpus,
        "quota": quota,
        "period": period,
        "ok": (
            quota is not None
            and period is not None
            and period > 0
            and quota == round(expected_cpus * period)
        ),
    }
    checks["cgroup_memory_bytes"] = {
        "expected": expected_memory_bytes,
        "actual": memory_limit,
        "ok": memory_limit == expected_memory_bytes,
    }
    errors = [name for name, check in checks.items() if not check["ok"]]
    return {"ok": not errors, "checks": checks, "failed_checks": errors}


def configured_executable(container_inspect: dict[str, Any]) -> str | None:
    config = container_inspect.get("Config") or {}
    entrypoint = config.get("Entrypoint") or []
    command = config.get("Cmd") or []
    if isinstance(entrypoint, str):
        entrypoint = [entrypoint]
    if isinstance(command, str):
        command = [command]
    argv = [*entrypoint, *command]
    return str(argv[0]) if argv else None


def executable_provenance(container: str, configured: str | None) -> dict[str, Any]:
    record: dict[str, Any] = {
        "configured": configured,
        "resolved_path": None,
        "sha256": None,
    }
    if not configured:
        record["error"] = "container has no configured command"
        return record

    if configured.startswith("/"):
        resolved = configured
    else:
        resolver = run_command(
            [
                "docker",
                "exec",
                container,
                "sh",
                "-c",
                'candidate=$(command -v "$1") || exit 1; case "$candidate" in /*) printf "%s\\n" "$candidate" ;; *) printf "%s/%s\\n" "$PWD" "$candidate" ;; esac',
                "resolve",
                configured,
            ]
        )
        if resolver.returncode != 0 or not resolver.stdout.strip():
            record["error"] = (
                resolver.stderr.strip()
                or "configured executable was not resolvable"
            )
            return record
        resolved = resolver.stdout.strip().splitlines()[0]
    record["resolved_path"] = resolved

    with tempfile.TemporaryDirectory(prefix="mcp-benchmark-executable-") as temp_dir:
        destination = Path(temp_dir) / "executable"
        copier = run_command(
            ["docker", "cp", "-L", f"{container}:{resolved}", str(destination)]
        )
        if copier.returncode != 0 or not destination.is_file():
            record["error"] = copier.stderr.strip() or "docker cp produced no file"
            return record
        record["sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
    return record


def positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def positive_number(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Write container_inspect_runN.json, image_inspect_runN.json, and "
            "cgroup_runN.json, then enforce the benchmark resource contract."
        )
    )
    parser.add_argument(
        "--container", required=True, help="running container name or ID"
    )
    parser.add_argument("--run", required=True, type=positive_integer, dest="run_number")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--expected-cpus", type=positive_number, default=2.0)
    parser.add_argument(
        "--expected-memory-bytes", type=positive_integer, default=2 * GIBIBYTE
    )
    parser.add_argument(
        "--expected-cpuset",
        help=(
            "CPU list/ranges that Docker and the effective cgroup must exactly match"
        ),
    )
    parser.add_argument(
        "--skip-executable",
        action="store_true",
        help="skip docker cp hashing when a pinned image digest is sufficient",
    )
    return parser.parse_args(argv)


def capture(args: argparse.Namespace) -> dict[str, Any]:
    raw_container, container_inspect = inspect_one(
        ["docker", "inspect", args.container],
        f"container inspect for {args.container!r}",
    )
    image_reference = container_inspect.get("Image")
    if not image_reference:
        raise CaptureError("container inspect did not include an image ID")
    raw_image, image_inspect = inspect_one(
        ["docker", "image", "inspect", str(image_reference)],
        f"image inspect for {image_reference!r}",
    )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"run{args.run_number}"
    (args.output_dir / f"container_inspect_{suffix}.json").write_text(
        raw_container.rstrip() + "\n", encoding="utf-8"
    )
    (args.output_dir / f"image_inspect_{suffix}.json").write_text(
        raw_image.rstrip() + "\n", encoding="utf-8"
    )

    cgroup = detect_cgroup(args.container)
    validation = validate_resources(
        container_inspect,
        cgroup,
        args.expected_cpus,
        args.expected_memory_bytes,
        args.expected_cpuset,
    )
    executable = (
        {"skipped": True, "reason": "pinned image provenance is sufficient"}
        if args.skip_executable
        else executable_provenance(
            args.container, configured_executable(container_inspect)
        )
    )
    if not args.skip_executable and not executable.get("sha256"):
        raise CaptureError(
            "could not capture configured executable: "
            f"{executable.get('error', 'missing SHA-256')}"
        )

    summary = {
        "schema_version": 1,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "container": {
            "requested": args.container,
            "id": container_inspect.get("Id"),
            "name": container_inspect.get("Name"),
            "state": (container_inspect.get("State") or {}).get("Status"),
            "host_config": {
                "nano_cpus": (container_inspect.get("HostConfig") or {}).get(
                    "NanoCpus"
                ),
                "memory_bytes": (container_inspect.get("HostConfig") or {}).get(
                    "Memory"
                ),
                "cpuset_cpus": (container_inspect.get("HostConfig") or {}).get(
                    "CpusetCpus"
                ),
            },
        },
        "image": {
            "id": image_inspect.get("Id"),
            "repo_tags": image_inspect.get("RepoTags") or [],
            "repo_digests": image_inspect.get("RepoDigests") or [],
        },
        "executable": executable,
        "cgroup": cgroup,
        "cpuset_effective": selected_value(cgroup["cpuset_effective"]),
        "validation": validation,
    }
    (args.output_dir / f"cgroup_{suffix}.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    return summary


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        summary = capture(args)
    except CaptureError as error:
        print(f"capture_container.py: {error}", file=sys.stderr)
        return 2
    if not summary["validation"]["ok"]:
        failed = ", ".join(summary["validation"]["failed_checks"])
        print(
            f"capture_container.py: resource validation failed: {failed}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
