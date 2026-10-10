#!/usr/bin/env python3
"""Capture reproducible host, source, dependency, and profile provenance."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import platform
import subprocess
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Sequence


EXCLUDED_BENCHMARK_PARTS = frozenset(
    {"__pycache__", "alternative-sdks", "benchmark-mcp-servers-v2", "results"}
)


class CaptureError(RuntimeError):
    """Raised when required benchmark provenance cannot be captured."""


def run_command(command: Sequence[str], cwd: Path) -> dict[str, Any]:
    """Run a command without a shell and retain audit-friendly output."""
    try:
        completed = subprocess.run(
            list(command), cwd=cwd, capture_output=True, text=True, check=False
        )
    except OSError as error:
        return {
            "command": list(command),
            "exit_code": None,
            "stdout": "",
            "stderr": str(error),
        }
    return {
        "command": list(command),
        "exit_code": completed.returncode,
        "stdout": completed.stdout.strip(),
        "stderr": completed.stderr.strip(),
    }


def required_stdout(result: dict[str, Any], description: str) -> str:
    if result["exit_code"] != 0 or not result["stdout"]:
        detail = result["stderr"] or "command produced no output"
        raise CaptureError(f"could not capture {description}: {detail}")
    return str(result["stdout"])


def read_optional(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def parse_os_release(path: Path = Path("/etc/os-release")) -> dict[str, str]:
    values: dict[str, str] = {}
    content = read_optional(path)
    if content is None:
        return values
    for line in content.splitlines():
        if "=" not in line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values


def parse_cpuinfo(path: Path = Path("/proc/cpuinfo")) -> dict[str, Any]:
    content = read_optional(path)
    if content is None:
        return {
            "model": None,
            "physical_package_count": None,
            "physical_core_count": None,
        }

    records: list[dict[str, str]] = []
    current: dict[str, str] = {}
    for line in content.splitlines():
        if not line.strip():
            if current:
                records.append(current)
                current = {}
            continue
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        current[key.strip()] = value.strip()
    if current:
        records.append(current)

    model = next(
        (
            record.get("model name")
            or record.get("Hardware")
            or record.get("Processor")
            for record in records
            if record.get("model name")
            or record.get("Hardware")
            or record.get("Processor")
        ),
        None,
    )
    package_ids = {
        record["physical id"] for record in records if "physical id" in record
    }
    core_ids = {
        (record["physical id"], record["core id"])
        for record in records
        if "physical id" in record and "core id" in record
    }
    return {
        "model": model,
        "physical_package_count": len(package_ids) if package_ids else None,
        "physical_core_count": len(core_ids) if core_ids else None,
    }


def parse_memory_total_bytes(path: Path = Path("/proc/meminfo")) -> int | None:
    content = read_optional(path)
    if content is None:
        return None
    for line in content.splitlines():
        if not line.startswith("MemTotal:"):
            continue
        fields = line.split()
        if len(fields) >= 2 and fields[1].isdigit():
            return int(fields[1]) * 1024
    return None


def compress_cpu_list(cpus: Iterable[int]) -> str:
    values = sorted(set(cpus))
    if not values:
        return ""
    ranges: list[str] = []
    start = previous = values[0]
    for value in values[1:]:
        if value == previous + 1:
            previous = value
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = value
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(ranges)


def cpu_affinity() -> list[int] | None:
    try:
        return sorted(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return None


def cpu_governors(cpus: Iterable[int] | None) -> dict[str, Any]:
    cpu_values = list(cpus) if cpus is not None else list(range(os.cpu_count() or 0))
    by_cpu: dict[str, str] = {}
    for cpu in cpu_values:
        value = read_optional(
            Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor")
        )
        if value:
            by_cpu[str(cpu)] = value
    return {"values": sorted(set(by_cpu.values())), "by_cpu": by_cpu}


def benchmark_source_files(project_dir: Path) -> Iterable[Path]:
    """Yield core sources and all local harness inputs, excluding fetched/runtime data."""
    for relative in ("CMakeLists.txt", "VERSION", "LICENSE"):
        path = project_dir / relative
        if path.is_file():
            yield path

    for relative in ("cmake", "include", "src"):
        root = project_dir / relative
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                yield path

    benchmark_root = project_dir / "benchmark"
    if not benchmark_root.is_dir():
        return
    for path in sorted(benchmark_root.rglob("*")):
        if not path.is_file() or path.suffix == ".pyc":
            continue
        relative_parts = path.relative_to(benchmark_root).parts
        if any(part in EXCLUDED_BENCHMARK_PARTS for part in relative_parts):
            continue
        yield path


def tree_digest(project_dir: Path) -> tuple[str, list[str]]:
    digest = hashlib.sha256()
    relative_paths: list[str] = []
    for path in benchmark_source_files(project_dir):
        relative = path.relative_to(project_dir).as_posix()
        relative_bytes = relative.encode("utf-8")
        content = path.read_bytes()
        digest.update(len(relative_bytes).to_bytes(8, "big"))
        digest.update(relative_bytes)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
        relative_paths.append(relative)
    return digest.hexdigest(), relative_paths


def create_source_snapshot(
    project_dir: Path, relative_paths: Sequence[str], output: Path
) -> str:
    """Write a deterministic archive of every local source input used by the run."""
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w", format=tarfile.PAX_FORMAT) as archive:
        for relative in relative_paths:
            path = project_dir / relative
            content = path.read_bytes()
            info = tarfile.TarInfo(relative)
            info.size = len(content)
            info.mode = path.stat().st_mode & 0o777
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            archive.addfile(info, io.BytesIO(content))
    return hashlib.sha256(output.read_bytes()).hexdigest()


def verify_source_digest(environment_path: Path, project_dir: Path) -> int:
    try:
        environment = json.loads(environment_path.read_text(encoding="utf-8"))
        expected = environment["source"]["tree_sha256"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        print(f"capture_environment.py: invalid environment record: {error}", file=sys.stderr)
        return 2
    actual, _ = tree_digest(project_dir.resolve())
    if actual != expected:
        print(
            "capture_environment.py: local benchmark sources changed during the run",
            file=sys.stderr,
        )
        return 1
    return 0


def git_repository(
    path: Path, expected_commit: str | None = None
) -> dict[str, Any]:
    if not path.is_dir():
        return {
            "path": str(path),
            "available": False,
            "expected_commit": expected_commit,
            "error": "directory does not exist",
        }

    head_result = run_command(["git", "rev-parse", "HEAD"], path)
    status_result = run_command(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], path
    )
    origin_result = run_command(["git", "remote", "get-url", "origin"], path)
    head = head_result["stdout"] if head_result["exit_code"] == 0 else None
    status = status_result["stdout"] if status_result["exit_code"] == 0 else None
    return {
        "path": str(path.resolve()),
        "available": head is not None,
        "origin_url": origin_result["stdout"] if origin_result["exit_code"] == 0 else None,
        "head": head,
        "expected_commit": expected_commit,
        "commit_matches": expected_commit is None or head == expected_commit,
        "clean": status == "" if status is not None else None,
        "status_porcelain": status,
        "errors": {
            "head": head_result["stderr"] if head_result["exit_code"] != 0 else None,
            "status": status_result["stderr"] if status_result["exit_code"] != 0 else None,
            "origin": origin_result["stderr"] if origin_result["exit_code"] != 0 else None,
        },
    }


def alternative_repositories(manifest: Path, root: Path) -> list[dict[str, Any]]:
    try:
        manifest_file = manifest.open(encoding="utf-8", newline="")
    except OSError as error:
        raise CaptureError(f"could not open alternative manifest {manifest}: {error}") from error

    with manifest_file:
        reader = csv.DictReader(manifest_file, delimiter="\t")
        if reader.fieldnames:
            reader.fieldnames = [field.removeprefix("#").strip() for field in reader.fieldnames]
        required_fields = {"id", "repository", "commit", "benchmark_status"}
        if not reader.fieldnames or not required_fields.issubset(reader.fieldnames):
            missing = sorted(required_fields - set(reader.fieldnames or []))
            raise CaptureError(f"alternative manifest {manifest} is missing fields: {missing}")

        entries: list[dict[str, Any]] = []
        for row in reader:
            identifier = row["id"].strip()
            repository = git_repository(root / identifier, row["commit"].strip())
            repository.update(
                {
                    "id": identifier,
                    "manifest_url": row["repository"].strip(),
                    "origin_matches_manifest": repository.get("origin_url")
                    == row["repository"].strip(),
                    "benchmark_status": row["benchmark_status"].strip(),
                }
            )
            entries.append(repository)
    return entries


def inspect_k6_image(reference: str, cwd: Path) -> dict[str, Any]:
    result = run_command(["docker", "image", "inspect", reference], cwd)
    stdout = required_stdout(result, f"k6 image {reference!r}")
    try:
        inspected = json.loads(stdout)
    except json.JSONDecodeError as error:
        raise CaptureError(f"docker returned invalid JSON for k6 image {reference!r}") from error
    if not isinstance(inspected, list) or len(inspected) != 1:
        raise CaptureError(f"expected one image inspect record for k6 image {reference!r}")
    image = inspected[0]
    return {
        "reference": reference,
        "id": image.get("Id"),
        "repo_tags": image.get("RepoTags") or [],
        "repo_digests": image.get("RepoDigests") or [],
        "inspect": image,
    }


def positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture benchmark profile, host, source, and dependency provenance."
    )
    parser.add_argument("output", type=Path, help="environment JSON output path")
    parser.add_argument("--project-dir", type=Path, required=True)
    parser.add_argument("--upstream-dir", type=Path, required=True)
    parser.add_argument("--upstream-url")
    parser.add_argument("--upstream-commit", required=True)
    parser.add_argument("--alternative-root", type=Path, required=True)
    parser.add_argument("--sources-manifest", type=Path)
    parser.add_argument("--protocol-version", required=True)
    parser.add_argument("--eligibility-contract", required=True)
    parser.add_argument("--k6-image", required=True)
    parser.add_argument("--order-seed", required=True, type=int)
    parser.add_argument("--runs", required=True, type=positive_integer)
    parser.add_argument("--servers", nargs="+", required=True)
    parser.add_argument("--vus", required=True, type=positive_integer)
    parser.add_argument(
        "--measurement-duration",
        "--measure-duration",
        dest="measurement_duration",
        required=True,
    )
    parser.add_argument("--warmup-duration", required=True)
    parser.add_argument("--ramp-duration", required=True)
    return parser.parse_args(argv)


def build_metadata(args: argparse.Namespace) -> dict[str, Any]:
    project_dir = args.project_dir.resolve()
    upstream_dir = args.upstream_dir.resolve()
    alternative_root = args.alternative_root.resolve()
    manifest = (
        args.sources_manifest.resolve()
        if args.sources_manifest
        else project_dir / "benchmark" / "alternatives" / "sources.tsv"
    )

    source_sha256, source_paths = tree_digest(project_dir)
    uname = platform.uname()
    affinity = cpu_affinity()
    cpu = parse_cpuinfo()

    upstream = git_repository(upstream_dir, args.upstream_commit)
    upstream["expected_url"] = args.upstream_url
    upstream["origin_matches_expected"] = (
        args.upstream_url is None or upstream.get("origin_url") == args.upstream_url
    )
    alternatives = alternative_repositories(manifest, alternative_root)

    docker_version = run_command(
        ["docker", "version", "--format", "{{json .}}"], project_dir
    )
    if docker_version["exit_code"] == 0 and docker_version["stdout"]:
        try:
            docker_version["parsed"] = json.loads(docker_version["stdout"])
        except json.JSONDecodeError:
            docker_version["parsed"] = None

    return {
        "schema_version": 2,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "profile": {
            "servers": args.servers,
            "runs": args.runs,
            "order_seed": args.order_seed,
            "requested_protocol_version": args.protocol_version,
            "eligibility_contract": args.eligibility_contract,
            "virtual_users": args.vus,
            "ramp_duration": args.ramp_duration,
            "warmup_duration": args.warmup_duration,
            "measurement_duration": args.measurement_duration,
            "k6_image_reference": args.k6_image,
        },
        "source": {
            "tree_sha256": source_sha256,
            "file_count": len(source_paths),
            "files": source_paths,
            "project_repository": git_repository(project_dir),
        },
        "dependencies": {
            "upstream": upstream,
            "alternative_manifest": str(manifest),
            "alternatives": alternatives,
        },
        "host": {
            "os": {
                "system": uname.system,
                "release": uname.release,
                "version": uname.version,
                "distribution": parse_os_release(),
            },
            "kernel": uname.release,
            "architecture": uname.machine,
            "cpu": {
                **cpu,
                "logical_cpu_count": os.cpu_count(),
                "affinity": affinity,
                "affinity_list": compress_cpu_list(affinity or []),
                "governors": cpu_governors(affinity),
                "lscpu": run_command(["lscpu", "--json"], project_dir),
            },
            "memory_total_bytes": parse_memory_total_bytes(),
        },
        "tools": {
            "docker": docker_version,
            "docker_compose": run_command(["docker", "compose", "version"], project_dir),
            "python": {
                "version": platform.python_version(),
                "executable": sys.executable,
                "command": run_command([sys.executable, "--version"], project_dir),
            },
            "git": run_command(["git", "--version"], project_dir),
        },
        "k6_image": inspect_k6_image(args.k6_image, project_dir),
    }


def main(argv: Sequence[str] | None = None) -> int:
    effective_argv = list(argv) if argv is not None else sys.argv[1:]
    if effective_argv and effective_argv[0] == "--verify-source":
        verifier = argparse.ArgumentParser()
        verifier.add_argument("--verify-source", type=Path, required=True)
        verifier.add_argument("--project-dir", type=Path, required=True)
        verify_args = verifier.parse_args(effective_argv)
        return verify_source_digest(verify_args.verify_source, verify_args.project_dir)

    args = parse_args(effective_argv)
    try:
        metadata = build_metadata(args)
    except CaptureError as error:
        print(f"capture_environment.py: {error}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    snapshot_path = args.output.parent / "source_snapshot.tar"
    snapshot_sha256 = create_source_snapshot(
        args.project_dir.resolve(), metadata["source"]["files"], snapshot_path
    )
    metadata["source"]["snapshot"] = {
        "path": snapshot_path.name,
        "sha256": snapshot_sha256,
        "format": "deterministic POSIX tar",
    }
    args.output.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
