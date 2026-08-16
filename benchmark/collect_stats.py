#!/usr/bin/env python3
"""Collect auditable resource samples for a container or the benchmark host."""

from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path
from typing import Any


RUNNING = True
STOP_SIGNAL: int | None = None


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
ANSI_CONTROL = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
SIZE_VALUE = re.compile(
    r"^((?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:e[+-]?[0-9]+)?)\s*([a-z]+)$"
)
HOST_NETWORK_POLICY = "baseline-cohort-retire-v1"


class CollectionStopped(Exception):
    """Internal signal that a streaming collector was stopped intentionally."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_size_to_bytes(value: str) -> int:
    text = value.strip().lower()
    if text in {"", "0", "0b", "--"}:
        return 0

    match = SIZE_VALUE.match(text)
    if not match:
        raise ValueError(f"Unable to parse size value: {value!r}")

    number = Decimal(match.group(1))
    unit = match.group(2)
    if unit not in SIZE_UNITS:
        raise ValueError(f"Unknown size unit in value: {value!r}")

    return int(number * SIZE_UNITS[unit])


def parse_cpu_percent(value: str) -> float:
    text = value.strip().replace("%", "")
    return float(text) if text else 0.0


def parse_mem_usage(value: str) -> tuple[int, int]:
    parts = [part.strip() for part in value.split("/")]
    if len(parts) != 2:
        raise ValueError(f"Unexpected memory usage format: {value!r}")
    return parse_size_to_bytes(parts[0]), parse_size_to_bytes(parts[1])


def parse_net_io(value: str) -> tuple[int, int]:
    parts = [part.strip() for part in value.split("/")]
    if len(parts) != 2:
        raise ValueError(f"Unexpected net I/O format: {value!r}")
    return parse_size_to_bytes(parts[0]), parse_size_to_bytes(parts[1])


def audit_path_for(output_path: Path) -> Path:
    """Return the deterministic sidecar path without changing the sample format."""
    return output_path.with_name(f"{output_path.stem}.audit.json")


def readiness_path_for(output_path: Path) -> Path:
    """Return the marker written only after the first valid baseline sample."""
    return output_path.with_name(f"{output_path.stem}.ready.json")


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def write_initialization_failure(
    output_path: Path,
    target: str,
    target_type: str,
    interval_seconds: float,
    error: Exception,
) -> bool:
    """Best-effort failure artifacts for a collector that could not start."""
    timestamp = utc_now()
    audit = {
        "schema_version": 1,
        "status": "failed",
        "target": target,
        "target_type": target_type,
        "output_path": str(output_path),
        "audit_path": str(audit_path_for(output_path)),
        "readiness_path": str(readiness_path_for(output_path)),
        "interval_seconds": interval_seconds,
        "started_at": timestamp,
        "finished_at": timestamp,
        "elapsed_seconds": 0.0,
        "termination": {"reason": "initialization_failure", "signal": None},
        "attempt_count": 0,
        "sample_count": 0,
        "failure_count": 1,
        "failures": [{"timestamp": timestamp, "error": str(error)}],
        "fatal_error": str(error),
    }
    try:
        write_json(output_path, [])
        write_json(audit_path_for(output_path), audit)
    except Exception as write_error:
        print(
            f"Failed to write collector initialization artifacts: {write_error}",
            file=sys.stderr,
        )
        return False
    return True


def handle_signal(signum: int, _frame: object) -> None:
    global RUNNING, STOP_SIGNAL
    STOP_SIGNAL = signum
    RUNNING = False


def parse_container_stats_line(line: str) -> dict[str, int | float | str]:
    parts = line.split("|")
    if len(parts) != 3:
        raise RuntimeError(f"Unexpected docker stats format: {line!r}")

    cpu_percent = parse_cpu_percent(parts[0])
    mem_usage_bytes, mem_limit_bytes = parse_mem_usage(parts[1])
    net_io_rx, net_io_tx = parse_net_io(parts[2])
    return {
        "timestamp": utc_now(),
        "cpu_percent": cpu_percent,
        "mem_usage_bytes": mem_usage_bytes,
        "mem_limit_bytes": mem_limit_bytes,
        "net_io_rx": net_io_rx,
        "net_io_tx": net_io_tx,
    }


def collect_container_once(container_name: str) -> dict[str, int | float | str]:
    """Collect one container sample; retained as a deterministic test seam."""
    result = subprocess.run(
        [
            "docker",
            "stats",
            "--no-stream",
            "--format",
            "{{.CPUPerc}}|{{.MemUsage}}|{{.NetIO}}",
            container_name,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = [
        ANSI_CONTROL.sub("", line).strip()
        for line in result.stdout.splitlines()
        if ANSI_CONTROL.sub("", line).strip()
    ]
    if len(lines) != 1:
        raise RuntimeError(
            f"docker stats returned {len(lines)} rows for {container_name!r}"
        )
    return parse_container_stats_line(lines[0])


_DEFAULT_COLLECT_CONTAINER_ONCE = collect_container_once


class ContainerStatsStream:
    """Read a persistent Docker stats stream without repeated startup delays."""

    def __init__(self, container_name: str) -> None:
        self.container_name = container_name
        self.process = subprocess.Popen(
            [
                "docker",
                "stats",
                "--format",
                "{{.CPUPerc}}|{{.MemUsage}}|{{.NetIO}}",
                container_name,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        if self.process.stdout is None or self.process.stderr is None:
            self.close()
            raise RuntimeError("docker stats stream did not expose stdout and stderr")

    def collect(self) -> dict[str, int | float | str]:
        assert self.process.stdout is not None
        while RUNNING:
            raw_line = self.process.stdout.readline()
            if not RUNNING:
                raise CollectionStopped
            if raw_line == "":
                return_code = self.process.poll()
                detail = self._stderr()
                raise RuntimeError(
                    detail
                    or f"docker stats stream exited unexpectedly with code {return_code}"
                )
            line = ANSI_CONTROL.sub("", raw_line).strip()
            if not line:
                continue
            return parse_container_stats_line(line)
        raise CollectionStopped

    def _stderr(self) -> str:
        if self.process.stderr is None:
            return ""
        return self.process.stderr.read().strip()

    def close(self) -> None:
        if self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)


def read_cpu_snapshot(proc_root: Path, affinity: frozenset[int]) -> tuple[int, int]:
    """Return aggregate (total, busy) scheduler ticks for affinity-visible CPUs."""
    totals = 0
    busy = 0
    observed: set[int] = set()
    with (proc_root / "stat").open(encoding="utf-8") as stream:
        for line in stream:
            match = re.match(r"^cpu(\d+)\s+(.+)$", line.rstrip())
            if not match:
                continue
            cpu = int(match.group(1))
            if cpu not in affinity:
                continue
            values = [int(value) for value in match.group(2).split()]
            if len(values) < 5:
                raise RuntimeError(f"Malformed /proc/stat row for cpu{cpu}")
            # Linux accounts guest time inside user/nice, so only the first eight
            # counters participate in the conventional non-double-counted total.
            cpu_total = sum(values[:8])
            cpu_idle = values[3] + values[4]
            totals += cpu_total
            busy += cpu_total - cpu_idle
            observed.add(cpu)
    missing = affinity - observed
    if missing:
        raise RuntimeError(f"/proc/stat is missing affinity CPUs: {sorted(missing)}")
    return totals, busy


def read_host_memory(proc_root: Path) -> tuple[int, int]:
    fields: dict[str, int] = {}
    with (proc_root / "meminfo").open(encoding="utf-8") as stream:
        for line in stream:
            key, separator, value = line.partition(":")
            if not separator:
                continue
            parts = value.split()
            if parts:
                fields[key] = int(parts[0]) * 1024
    try:
        total = fields["MemTotal"]
        available = fields["MemAvailable"]
    except KeyError as error:
        raise RuntimeError(f"Missing {error.args[0]} in /proc/meminfo") from error
    return total - available, total


def read_host_network(proc_root: Path) -> dict[str, tuple[int, int]]:
    counters: dict[str, tuple[int, int]] = {}
    with (proc_root / "net" / "dev").open(encoding="utf-8") as stream:
        for line in stream:
            if ":" not in line:
                continue
            interface, values = line.split(":", 1)
            interface = interface.strip()
            fields = values.split()
            if len(fields) < 16:
                raise RuntimeError(f"Malformed /proc/net/dev row: {line.rstrip()!r}")
            if not interface or interface in counters:
                raise RuntimeError(
                    f"Invalid interface in /proc/net/dev row: {line.rstrip()!r}"
                )
            counters[interface] = (int(fields[0]), int(fields[8]))
    if not counters:
        raise RuntimeError("/proc/net/dev contains no network interfaces")
    return counters


class HostStatsCollector:
    """Measure the host resources available to this affinity-constrained process."""

    def __init__(
        self,
        proc_root: Path = Path("/proc"),
        affinity: frozenset[int] | None = None,
    ) -> None:
        self.proc_root = proc_root
        self.affinity = (
            frozenset(os.sched_getaffinity(0)) if affinity is None else affinity
        )
        if not self.affinity:
            raise RuntimeError("collector process has an empty CPU affinity")
        self._previous_total, self._previous_busy = read_cpu_snapshot(
            self.proc_root, self.affinity
        )
        network = read_host_network(self.proc_root)
        self.network_interfaces = frozenset(network)
        self.active_network_interfaces = set(self.network_interfaces)
        self.retired_network_interfaces: set[str] = set()
        self.ignored_new_network_interfaces: set[str] = set()
        self._previous_network = {
            interface: network[interface] for interface in self.network_interfaces
        }

    def audit_metadata(self) -> dict[str, Any]:
        """Describe the fixed host scope and any observed interface retirement."""
        return {
            "cpu_affinity": sorted(self.affinity),
            "cpu_capacity_cores": len(self.affinity),
            "network_interfaces": sorted(self.network_interfaces),
            "active_network_interfaces": sorted(self.active_network_interfaces),
            "retired_network_interfaces": sorted(self.retired_network_interfaces),
            "ignored_new_network_interfaces": sorted(
                self.ignored_new_network_interfaces
            ),
            "network_interface_policy": HOST_NETWORK_POLICY,
            "network_interface_accounting": (
                "baseline cohort; freeze last counters on retirement; "
                "ignore later additions"
            ),
        }

    def collect(self) -> dict[str, int | float | str]:
        total, busy = read_cpu_snapshot(self.proc_root, self.affinity)
        total_delta = total - self._previous_total
        busy_delta = busy - self._previous_busy
        self._previous_total, self._previous_busy = total, busy
        if total_delta <= 0 or busy_delta < 0:
            raise RuntimeError("host CPU counters did not advance monotonically")

        # Match docker-stats semantics: one fully busy core is 100%, so the
        # capacity is affinity-core-count * 100%.
        cpu_percent = busy_delta / total_delta * len(self.affinity) * 100.0
        mem_usage_bytes, mem_limit_bytes = read_host_memory(self.proc_root)
        network = read_host_network(self.proc_root)
        self.ignored_new_network_interfaces.update(
            network.keys() - self.network_interfaces
        )
        missing_interfaces = self.active_network_interfaces - network.keys()
        if missing_interfaces:
            # Docker can remove a short-lived host-side interface after the
            # collector has taken its baseline. Freeze that interface at its
            # last observed counters so the aggregate remains monotonic and the
            # bytes observed before retirement remain represented. Interfaces
            # created after the baseline are intentionally never adopted.
            self.active_network_interfaces.difference_update(missing_interfaces)
            self.retired_network_interfaces.update(missing_interfaces)
        if not self.active_network_interfaces:
            raise RuntimeError(
                "all baseline host network interfaces disappeared during collection"
            )
        for interface in self.active_network_interfaces:
            previous_rx, previous_tx = self._previous_network[interface]
            current_rx, current_tx = network[interface]
            if current_rx < previous_rx or current_tx < previous_tx:
                raise RuntimeError(
                    f"host network counters reset for interface {interface!r}"
                )
            self._previous_network[interface] = (current_rx, current_tx)
        net_io_rx = sum(
            self._previous_network[interface][0]
            for interface in self.network_interfaces
        )
        net_io_tx = sum(
            self._previous_network[interface][1]
            for interface in self.network_interfaces
        )
        return {
            "timestamp": utc_now(),
            "cpu_percent": cpu_percent,
            "mem_usage_bytes": mem_usage_bytes,
            "mem_limit_bytes": mem_limit_bytes,
            "net_io_rx": net_io_rx,
            "net_io_tx": net_io_tx,
        }


def wait_until(deadline: float) -> None:
    while RUNNING:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(0.2, remaining))


def main() -> int:
    global RUNNING, STOP_SIGNAL

    if len(sys.argv) != 4:
        print(
            "Usage: collect_stats.py <container_name|@host> "
            "<output_path> <interval_seconds>",
            file=sys.stderr,
        )
        return 1

    target = sys.argv[1]
    output_path = Path(sys.argv[2])
    try:
        interval_seconds = float(sys.argv[3])
        if interval_seconds <= 0:
            raise ValueError("interval_seconds must be > 0")
    except ValueError as exc:
        print(f"Invalid interval_seconds: {exc}", file=sys.stderr)
        return 1

    RUNNING = True
    STOP_SIGNAL = None
    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    target_type = "host" if target == "@host" else "container"
    host_collector: HostStatsCollector | None = None
    container_stream: ContainerStatsStream | None = None
    injected_single_sample = collect_container_once is not _DEFAULT_COLLECT_CONTAINER_ONCE
    try:
        if target_type == "host":
            host_collector = HostStatsCollector()
        elif not injected_single_sample:
            container_stream = ContainerStatsStream(target)
    except Exception as exc:
        print(f"Failed to initialize resource collector: {exc}", file=sys.stderr)
        write_initialization_failure(
            output_path, target, target_type, interval_seconds, exc
        )
        return 1

    started_at = utc_now()
    started_monotonic = time.monotonic()
    samples: list[dict[str, int | float | str]] = []
    failures: list[dict[str, str]] = []
    attempt_count = 0
    fatal_error: str | None = None
    readiness_written = False

    # A host CPU percentage requires a delta between two /proc/stat snapshots.
    next_sample_at = started_monotonic + interval_seconds
    try:
        while RUNNING:
            if host_collector is not None:
                wait_until(next_sample_at)
                if not RUNNING:
                    break
            attempt_count += 1
            try:
                if host_collector is not None:
                    samples.append(host_collector.collect())
                elif injected_single_sample:
                    samples.append(collect_container_once(target))
                else:
                    assert container_stream is not None
                    samples.append(container_stream.collect())
            except CollectionStopped:
                attempt_count -= 1
                break
            except Exception as exc:
                failure = {"timestamp": utc_now(), "error": str(exc)}
                failures.append(failure)
                print(
                    f"Resource collection attempt {attempt_count} failed: {exc}",
                    file=sys.stderr,
                )
                # A failed sample already makes the run ineligible, so continuing
                # can only create duplicate errors and cannot salvage the audit.
                break

            if samples and not readiness_written:
                try:
                    write_json(
                        readiness_path_for(output_path),
                        {
                            "schema_version": 1,
                            "status": "ready",
                            "target": target,
                            "target_type": target_type,
                            "ready_at": utc_now(),
                            "baseline_timestamp": samples[0]["timestamp"],
                        },
                    )
                    readiness_written = True
                except Exception as exc:
                    fatal_error = f"failed to write collector readiness marker: {exc}"
                    print(fatal_error, file=sys.stderr)
                    break

            if host_collector is not None:
                next_sample_at += interval_seconds
                if next_sample_at <= time.monotonic():
                    # Do not issue a burst of catch-up samples. The validator will
                    # reject the resulting sparse coverage instead of hiding it.
                    next_sample_at = time.monotonic() + interval_seconds
    except Exception as exc:
        fatal_error = str(exc)
        print(f"Fatal error in collector loop: {exc}", file=sys.stderr)
    finally:
        if container_stream is not None:
            container_stream.close()

    finished_monotonic = time.monotonic()
    finished_at = utc_now()
    succeeded = (
        bool(samples)
        and readiness_written
        and not failures
        and fatal_error is None
        and STOP_SIGNAL is not None
    )
    audit: dict[str, Any] = {
        "schema_version": 1,
        "status": "complete" if succeeded else "failed",
        "target": target,
        "target_type": target_type,
        "output_path": str(output_path),
        "audit_path": str(audit_path_for(output_path)),
        "readiness_path": str(readiness_path_for(output_path)),
        "interval_seconds": interval_seconds,
        "started_at": started_at,
        "finished_at": finished_at,
        "elapsed_seconds": finished_monotonic - started_monotonic,
        "termination": {
            "reason": "signal" if STOP_SIGNAL is not None else "loop_exit",
            "signal": STOP_SIGNAL,
        },
        "attempt_count": attempt_count,
        "sample_count": len(samples),
        "failure_count": len(failures),
        "failures": failures,
        "fatal_error": fatal_error,
    }
    if host_collector is not None:
        audit["host"] = host_collector.audit_metadata()
    else:
        audit["source"] = "persistent docker stats stream"

    write_failed = False
    try:
        # Keep the historical top-level list schema for downstream consumers.
        write_json(output_path, samples)
    except Exception as exc:
        write_failed = True
        print(f"Failed to write sample JSON: {exc}", file=sys.stderr)
    try:
        write_json(audit_path_for(output_path), audit)
    except Exception as exc:
        write_failed = True
        print(f"Failed to write audit JSON: {exc}", file=sys.stderr)

    return 0 if succeeded and not write_failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
