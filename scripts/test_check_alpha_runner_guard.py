#!/usr/bin/env python3
"""Tests for check_alpha_runner_guard: the runner pin comes from package.json."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import check_alpha_runner_guard as guard

READING_RUN_SH = 'expected="$(python3 -c "..." "$SCRIPT_DIR/runner/package.json")"\n'


class CheckPinTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.package_json = self.root / "package.json"
        self.run_sh = self.root / "run.sh"
        self.run_sh.write_text(READING_RUN_SH, encoding="utf-8")

    def pin(self, version: str) -> None:
        self.package_json.write_text(
            json.dumps({"dependencies": {guard.RUNNER_PACKAGE: version}}), encoding="utf-8"
        )

    def test_exact_release_passes(self) -> None:
        self.pin("0.1.16")
        self.assertEqual(guard.check_pin(self.run_sh, self.package_json), "0.1.16")

    def test_prerelease_pin_fails(self) -> None:
        for version in ("0.2.0-alpha.12", "0.2.0-rc.1", "0.2.0-beta.3"):
            with self.subTest(version=version):
                self.pin(version)
                with self.assertRaises(guard.GuardError):
                    guard.check_pin(self.run_sh, self.package_json)

    def test_range_pin_fails(self) -> None:
        for version in ("^0.1.16", "~0.1.16", ">=0.1.16", "latest", "0.1"):
            with self.subTest(version=version):
                self.pin(version)
                with self.assertRaises(guard.GuardError):
                    guard.check_pin(self.run_sh, self.package_json)

    def test_missing_dependency_fails(self) -> None:
        self.package_json.write_text(json.dumps({"dependencies": {}}), encoding="utf-8")
        with self.assertRaises(guard.GuardError):
            guard.check_pin(self.run_sh, self.package_json)

    def test_run_sh_with_its_own_version_fails(self) -> None:
        self.pin("0.1.16")
        self.run_sh.write_text(READING_RUN_SH + 'expected="0.1.16"\n', encoding="utf-8")
        with self.assertRaises(guard.GuardError):
            guard.check_pin(self.run_sh, self.package_json)

    def test_run_sh_not_reading_package_json_fails(self) -> None:
        self.pin("0.1.16")
        self.run_sh.write_text("true\n", encoding="utf-8")
        with self.assertRaises(guard.GuardError):
            guard.check_pin(self.run_sh, self.package_json)

    def test_address_in_run_sh_is_not_a_version(self) -> None:
        self.pin("0.1.16")
        self.run_sh.write_text(READING_RUN_SH + 'curl http://127.0.0.1:3001\n', encoding="utf-8")
        self.assertEqual(guard.check_pin(self.run_sh, self.package_json), "0.1.16")

    def test_repository_passes(self) -> None:
        guard.check_pin()
        guard.check_no_gating_reference()


if __name__ == "__main__":
    unittest.main()
