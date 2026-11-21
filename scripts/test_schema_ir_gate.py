#!/usr/bin/env python3
"""Tests for the schema instruction gate, with a runner standing in for Callgrind."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import schema_ir_gate  # noqa: E402

HEAD = Path("head/mcp-schema-workload")
BASE = Path("base/mcp-schema-workload")


def fake_runner(per_iteration: dict[tuple[Path, str], float], fingerprints=None, repeat_drift=0.0):
    """A runner whose instruction counts are start-up plus per_iteration times the iterations."""
    calls: dict[tuple[Path, str], int] = {}

    def run(executable: Path, group: str, iterations: int) -> schema_ir_gate.Run:
        key = (executable, group)
        calls[key] = calls.get(key, 0) + 1
        cost = per_iteration[key]
        # The base build is measured twice, two runs each; the second measurement drifts.
        if executable == BASE and calls[key] > 2:
            cost *= 1 + repeat_drift
        fingerprint = (fingerprints or {}).get(key, "00000000000000aa")
        return schema_ir_gate.Run(int(50_000 + cost * iterations), fingerprint)

    return run


class ParsingTest(unittest.TestCase):
    def test_reads_the_last_totals_line(self) -> None:
        text = "events: Ir\nsummary: 120\nfn=main\ntotals: 1234567\n"
        self.assertEqual(schema_ir_gate.parse_callgrind_total(text), 1234567)

    def test_reads_a_summary_line_without_totals(self) -> None:
        self.assertEqual(schema_ir_gate.parse_callgrind_total("events: Ir\nsummary: 99\n"), 99)

    def test_output_without_a_count_is_an_error(self) -> None:
        with self.assertRaises(schema_ir_gate.GateError):
            schema_ir_gate.parse_callgrind_total("events: Ir\n")

    def test_reads_the_fingerprint(self) -> None:
        self.assertEqual(
            schema_ir_gate.parse_fingerprint("fingerprint 0123456789abcdef valid 12\n"), "0123456789abcdef"
        )
        with self.assertRaises(schema_ir_gate.GateError):
            schema_ir_gate.parse_fingerprint("nothing\n")


class MeasureTest(unittest.TestCase):
    def costs(self, group: str, head: float, base: float) -> dict[tuple[Path, str], float]:
        return {(HEAD, group): head, (BASE, group): base}

    def test_start_up_cost_does_not_count(self) -> None:
        result = schema_ir_gate.measure(fake_runner(self.costs("native", 1000, 1000)), HEAD, BASE, ["native"])
        self.assertEqual(result["groups"]["native"]["head"], 1000)
        self.assertTrue(result["passed"])

    def test_each_group_is_held_to_its_limit(self) -> None:
        for group, limit in schema_ir_gate.LIMITS.items():
            with self.subTest(group=group):
                at_limit = round(1000 * limit)
                runner = fake_runner(self.costs(group, at_limit - 1, 1000))
                self.assertTrue(schema_ir_gate.measure(runner, HEAD, BASE, [group])["passed"])
                runner = fake_runner(self.costs(group, at_limit + 1, 1000))
                self.assertFalse(schema_ir_gate.measure(runner, HEAD, BASE, [group])["passed"])

    def test_different_workloads_are_not_compared(self) -> None:
        runner = fake_runner(
            self.costs("native", 500, 1000), fingerprints={(HEAD, "native"): "00000000000000bb"}
        )
        result = schema_ir_gate.measure(runner, HEAD, BASE, ["native"])
        self.assertFalse(result["passed"])
        self.assertIn("workloads differ", result["problems"][0])

    def test_a_base_build_that_does_not_repeat_invalidates_the_run(self) -> None:
        runner = fake_runner(self.costs("native", 1000, 1000), repeat_drift=0.01)
        result = schema_ir_gate.measure(runner, HEAD, BASE, ["native"])
        self.assertFalse(result["passed"])
        self.assertIn("does not repeat", result["problems"][0])
        runner = fake_runner(self.costs("native", 1000, 1000), repeat_drift=0.004)
        self.assertTrue(schema_ir_gate.measure(runner, HEAD, BASE, ["native"])["passed"])


class MainTest(unittest.TestCase):
    def test_writes_the_result_with_both_labels(self) -> None:
        costs = {(executable, group): 1000.0 for executable in (HEAD, BASE) for group in schema_ir_gate.LIMITS}
        with tempfile.TemporaryDirectory() as scratch:
            output = Path(scratch) / "result.json"
            status = schema_ir_gate.main(
                ["--head", str(HEAD), "--base", str(BASE), "--head-label", "abc", "--base-label", "def",
                 "--output", str(output)],
                runner=fake_runner(costs),
            )
            self.assertEqual(status, 0)
            result = json.loads(output.read_text())
        self.assertEqual((result["head_label"], result["base_label"]), ("abc", "def"))
        self.assertEqual(set(result["groups"]), set(schema_ir_gate.LIMITS))

    def test_fails_when_a_limit_is_exceeded(self) -> None:
        costs = {(HEAD, "stops"): 1100.0, (BASE, "stops"): 1000.0}
        status = schema_ir_gate.main(
            ["--head", str(HEAD), "--base", str(BASE), "--groups", "stops"], runner=fake_runner(costs)
        )
        self.assertEqual(status, 1)


if __name__ == "__main__":
    unittest.main()
