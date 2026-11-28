#!/usr/bin/env python3
"""Tests for check_pending_sse: which server-sse-polling results the pending gate accepts."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import check_pending_sse as gate

SCENARIO_DIR = "server-server-sse-polling-2026-11-27T01-19-55-697Z"


def entry(check_id: str, status: str) -> dict[str, str]:
    return {"id": check_id, "status": status}


def expected_checks() -> list[dict[str, str]]:
    return [
        entry("outgoing-request", "INFO"),
        entry("incoming-sse-event", "INFO"),
        entry(gate.PRIMING_CHECK, "SUCCESS"),
        entry(gate.RETRY_CHECK, "WARNING"),
        entry("server-sse-disconnect-resume", "INFO"),
    ]


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.results = Path(self.directory.name)

    def write(self, checks: list[dict[str, str]], scenario_dir: str = SCENARIO_DIR,
              results: Path | None = None) -> None:
        folder = (results or self.results) / scenario_dir
        folder.mkdir()
        (folder / "checks.json").write_text(json.dumps(checks), encoding="utf-8")

    def test_priming_with_only_the_retry_warning_passes(self) -> None:
        self.write(expected_checks())
        gate.check(self.results)

    def test_missing_priming_fails(self) -> None:
        checks = expected_checks()
        checks[2] = entry(gate.PRIMING_CHECK, "WARNING")
        self.write(checks)
        with self.assertRaisesRegex(gate.CheckError, gate.PRIMING_CHECK):
            gate.check(self.results)

    def test_absent_priming_check_fails(self) -> None:
        self.write([check for check in expected_checks() if check["id"] != gate.PRIMING_CHECK])
        with self.assertRaisesRegex(gate.CheckError, gate.PRIMING_CHECK):
            gate.check(self.results)

    def test_another_warning_or_failure_fails(self) -> None:
        for status in ("WARNING", "FAILURE"):
            with self.subTest(status=status):
                results = self.results / status
                results.mkdir()
                checks = expected_checks() + [entry("server-sse-post-request", status)]
                self.write(checks, results=results)
                with self.assertRaisesRegex(gate.CheckError, "server-sse-post-request"):
                    gate.check(results)

    def test_passing_retry_check_fails(self) -> None:
        checks = expected_checks()
        checks[3] = entry(gate.RETRY_CHECK, "SUCCESS")
        self.write(checks)
        with self.assertRaisesRegex(gate.CheckError, gate.RETRY_CHECK):
            gate.check(self.results)

    def test_missing_result_fails(self) -> None:
        with self.assertRaisesRegex(gate.CheckError, "found 0"):
            gate.check(self.results)

    def test_two_results_fail(self) -> None:
        self.write(expected_checks())
        self.write(expected_checks(), "server-server-sse-polling-2026-11-27T01-20-00-000Z")
        with self.assertRaisesRegex(gate.CheckError, "found 2"):
            gate.check(self.results)


if __name__ == "__main__":
    unittest.main()
