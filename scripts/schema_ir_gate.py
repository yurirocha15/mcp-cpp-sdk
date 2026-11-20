#!/usr/bin/env python3
"""Compare the instructions schema validation takes in two builds of the SDK.

Runs the mcp-schema-workload executable of each build (test/perf/schema_workload.cpp) under
Callgrind with two iteration counts. The difference, divided by the difference in iterations, is
the instruction count of one iteration without the start-up, and does not depend on the host's
load. The base build is measured twice; when the two measurements disagree by more than 0.5% the
run is invalid. Then each group's head/base ratio is held to its limit:

  tools, native   at most 103%: schemas the native evaluation already implemented
  applicators     at most 100%: schemas jsoncons evaluated on main
  stops           at most 100%: validations the work budget stops
  compile         at most 120%: compiling the schemas of the other groups

Both workloads must print the same fingerprint for a group, so that only the same schemas and
values are compared.

Usage:
  python3 scripts/schema_ir_gate.py --head BUILD/mcp-schema-workload --base MAIN/mcp-schema-workload
      [--head-label SHA] [--base-label SHA] [--groups tools,native,...] [--output result.json]

Exits 1 when a limit is exceeded, a fingerprint differs or the base build does not repeat.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

LIMITS = {
    "tools": 1.03,
    "native": 1.03,
    "applicators": 1.00,
    "stops": 1.00,
    "compile": 1.20,
}
FEW = 1
MANY = 101
REPEAT_TOLERANCE = 0.005

_TOTALS = re.compile(r"^(?:totals|summary):\s+(\d+)", re.MULTILINE)
_FINGERPRINT = re.compile(r"^fingerprint ([0-9a-f]{16}) valid (\d+)$", re.MULTILINE)


class GateError(Exception):
    """The measurement could not be made."""


def parse_callgrind_total(text: str) -> int:
    """The instruction count in a Callgrind output file."""
    found = _TOTALS.findall(text)
    if not found:
        raise GateError("no totals or summary line in the Callgrind output")
    return int(found[-1])


def parse_fingerprint(stdout: str) -> str:
    found = _FINGERPRINT.search(stdout)
    if found is None:
        raise GateError(f"the workload printed no fingerprint: {stdout!r}")
    return found.group(1)


@dataclass(frozen=True)
class Run:
    instructions: int
    fingerprint: str


def callgrind_run(executable: Path, group: str, iterations: int) -> Run:
    """Runs @p executable under Callgrind and returns its instruction count and fingerprint."""
    with tempfile.TemporaryDirectory() as scratch:
        output = Path(scratch) / "callgrind.out"
        result = subprocess.run(
            [
                "valgrind",
                "--tool=callgrind",
                f"--callgrind-out-file={output}",
                str(executable),
                group,
                str(iterations),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise GateError(f"{executable} {group} {iterations} failed: {result.stderr[-2000:]}")
        return Run(parse_callgrind_total(output.read_text()), parse_fingerprint(result.stdout))


Runner = Callable[[Path, str, int], Run]


def per_iteration(runner: Runner, executable: Path, group: str) -> tuple[float, str]:
    few = runner(executable, group, FEW)
    many = runner(executable, group, MANY)
    if few.fingerprint != many.fingerprint:
        raise GateError(f"{executable} {group}: the fingerprint changed between runs")
    return (many.instructions - few.instructions) / (MANY - FEW), few.fingerprint


def measure(runner: Runner, head: Path, base: Path, groups: list[str]) -> dict:
    """Measures every group and decides whether the head build passes."""
    result: dict = {"groups": {}, "passed": True, "problems": []}
    for group in groups:
        head_ir, head_print = per_iteration(runner, head, group)
        base_ir, base_print = per_iteration(runner, base, group)
        repeat_ir, _ = per_iteration(runner, base, group)
        entry = {
            "head": head_ir,
            "base": base_ir,
            "base_repeat": repeat_ir,
            "limit": LIMITS[group],
            "ratio": head_ir / base_ir if base_ir > 0 else None,
        }
        result["groups"][group] = entry
        if head_print != base_print:
            result["problems"].append(f"{group}: the two workloads differ ({head_print} vs {base_print})")
            continue
        if base_ir <= 0 or abs(repeat_ir - base_ir) > REPEAT_TOLERANCE * base_ir:
            result["problems"].append(
                f"{group}: the base build does not repeat ({base_ir:.0f} vs {repeat_ir:.0f})"
            )
            continue
        if entry["ratio"] > LIMITS[group]:
            result["problems"].append(
                f"{group}: head takes {entry['ratio']:.1%} of base's instructions, "
                f"limit {LIMITS[group]:.0%}"
            )
    result["passed"] = not result["problems"]
    return result


def main(argv: list[str] | None = None, runner: Runner = callgrind_run) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--head", type=Path, required=True, help="mcp-schema-workload of the change")
    parser.add_argument("--base", type=Path, required=True, help="mcp-schema-workload of main")
    parser.add_argument("--head-label", default="head", help="commit of the head build")
    parser.add_argument("--base-label", default="base", help="commit of the base build")
    parser.add_argument("--groups", default=",".join(LIMITS), help="comma-separated groups")
    parser.add_argument("--output", type=Path, help="write the result as JSON here")
    args = parser.parse_args(argv)

    groups = [group for group in args.groups.split(",") if group]
    unknown = [group for group in groups if group not in LIMITS]
    if unknown:
        parser.error(f"unknown groups: {', '.join(unknown)}")
    try:
        result = measure(runner, args.head, args.base, groups)
    except GateError as error:
        print(f"schema instruction gate: {error}", file=sys.stderr)
        return 1
    result["head_label"] = args.head_label
    result["base_label"] = args.base_label
    for group, entry in result["groups"].items():
        ratio = "n/a" if entry["ratio"] is None else f"{entry['ratio']:.1%}"
        print(
            f"{group:12} head {entry['head']:>14.0f}  base {entry['base']:>14.0f}  "
            f"ratio {ratio:>7}  limit {entry['limit']:.0%}"
        )
    for problem in result["problems"]:
        print(f"FAIL {problem}")
    if args.output:
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print("schema instruction gate: " + ("passed" if result["passed"] else "failed"))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
