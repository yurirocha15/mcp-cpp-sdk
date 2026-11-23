#!/usr/bin/env python3
"""Split the harness's CPU set into the server under test's CPUs and the shared rest.

The server under test gets CPUs of its own, one per physical core. The other hardware threads of
those cores are left idle, and Redis, the API service and k6 share the remaining CPUs.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

SYSFS_CPU = Path("/sys/devices/system/cpu")


def parse_cpu_list(value: str) -> set[int]:
    """Parse a kernel CPU list such as "0-3,8,10-11"."""
    cpus: set[int] = set()
    for field in value.strip().split(","):
        if not field:
            continue
        bounds = field.split("-", 1)
        start = int(bounds[0])
        end = int(bounds[1]) if len(bounds) == 2 else start
        if start < 0 or end < start:
            raise ValueError(f"invalid CPU range: {field!r}")
        cpus.update(range(start, end + 1))
    return cpus


def format_cpu_list(cpus: set[int] | list[int]) -> str:
    return ",".join(str(cpu) for cpu in sorted(cpus))


def read_siblings(cpus: set[int], sysfs: Path = SYSFS_CPU) -> dict[int, frozenset[int]]:
    """Each CPU's hardware threads on the same physical core, itself included."""
    siblings: dict[int, frozenset[int]] = {}
    for cpu in cpus:
        path = sysfs / f"cpu{cpu}" / "topology" / "thread_siblings_list"
        try:
            siblings[cpu] = frozenset(parse_cpu_list(path.read_text()) | {cpu})
        except FileNotFoundError:
            siblings[cpu] = frozenset({cpu})
    return siblings


def partition(
    cpus: set[int], siblings: dict[int, frozenset[int]], target_count: int
) -> dict[str, list[int]]:
    """Choose target_count CPUs on distinct physical cores, the highest-numbered cores first.

    The other hardware threads of the chosen cores are reserved; every other CPU is shared.
    """
    if target_count < 1:
        raise ValueError("target_count must be at least 1")
    cores: dict[frozenset[int], set[int]] = {}
    for cpu in cpus:
        cores.setdefault(siblings.get(cpu, frozenset({cpu})), set()).add(cpu)
    ranked = sorted(cores.values(), key=max, reverse=True)
    if len(ranked) <= target_count:
        raise ValueError(
            f"{len(ranked)} physical cores cannot give the server {target_count} "
            "and leave any for the shared containers"
        )
    target = {min(core) for core in ranked[:target_count]}
    reserved = set().union(*ranked[:target_count]) - target
    shared = cpus - target - reserved
    return {"target": sorted(target), "reserved": sorted(reserved), "shared": sorted(shared)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-cpus", type=int, default=2)
    parser.add_argument("--cpus", help="CPU list to split; defaults to this process's affinity")
    args = parser.parse_args()

    cpus = parse_cpu_list(args.cpus) if args.cpus else set(os.sched_getaffinity(0))
    try:
        result = partition(cpus, read_siblings(cpus), args.target_cpus)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps({key: format_cpu_list(value) for key, value in result.items()}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
