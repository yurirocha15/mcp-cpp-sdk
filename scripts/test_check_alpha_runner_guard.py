#!/usr/bin/env python3
"""Tests for check_alpha_runner_guard: the runner pin and where the alpha runner may run."""

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


GATE_JOB = """  gate:
    runs-on: ubuntu-24.04
    steps:
      - run: bash conformance/run.sh
"""

ALPHA_JOB_TEXT = """  conformance-2026-07-28-alpha:
    runs-on: ubuntu-24.04
    continue-on-error: true
    steps:
      - run: bash conformance/run_alpha.sh build/release build/conformance-results-alpha
"""


def workflow(*jobs: str) -> str:
    return "name: test\non: push\njobs:\n" + "".join(jobs)


class AlphaJobTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)
        self.workflows = root / "workflows"
        self.scripts = root / "scripts"
        self.workflows.mkdir()
        self.scripts.mkdir()

    def write(self, name: str, text: str, directory: Path | None = None) -> None:
        (directory or self.workflows).joinpath(name).write_text(text, encoding="utf-8")

    def check(self) -> None:
        guard.check_no_gating_reference(self.workflows, self.scripts)

    def test_column_zero_comment_between_jobs_does_not_hide_a_dependent_job(self) -> None:
        text = (
            "name: x\njobs:\n  conformance-2026-07-28-alpha:\n    continue-on-error: true\n"
            "    steps:\n      - run: echo ok\n# note\n\n  gate:\n"
            "    needs: [conformance-2026-07-28-alpha]\n    steps:\n"
            "      - run: bash conformance/run_alpha.sh\n"
        )
        self.write("conformance.yml", text)
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_no_alpha_job_passes(self) -> None:
        self.write("conformance.yml", workflow(GATE_JOB))
        self.check()

    def test_non_gating_alpha_job_passes(self) -> None:
        self.write("conformance.yml", workflow(GATE_JOB, ALPHA_JOB_TEXT))
        self.check()

    def test_alpha_job_first_in_the_file_passes(self) -> None:
        self.write("conformance.yml", workflow(ALPHA_JOB_TEXT, GATE_JOB))
        self.check()

    def test_alpha_job_without_continue_on_error_fails(self) -> None:
        job = ALPHA_JOB_TEXT.replace("    continue-on-error: true\n", "")
        self.write("conformance.yml", workflow(GATE_JOB, job))
        with self.assertRaisesRegex(guard.GuardError, "continue-on-error"):
            self.check()

    def test_continue_on_error_on_a_step_does_not_count(self) -> None:
        job = ALPHA_JOB_TEXT.replace("    continue-on-error: true\n", "").replace(
            "      - run:", "      - continue-on-error: true\n        run:"
        )
        self.write("conformance.yml", workflow(GATE_JOB, job))
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_alpha_runner_in_another_job_fails(self) -> None:
        gate = GATE_JOB + "      - run: bash conformance/run_alpha.sh\n"
        self.write("conformance.yml", workflow(gate, ALPHA_JOB_TEXT))
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_job_needing_the_alpha_job_fails(self) -> None:
        gate = GATE_JOB + "    needs: conformance-2026-07-28-alpha\n"
        self.write("conformance.yml", workflow(gate, ALPHA_JOB_TEXT))
        with self.assertRaisesRegex(guard.GuardError, "outside it"):
            self.check()

    def test_alpha_runner_in_another_workflow_fails(self) -> None:
        self.write("conformance.yml", workflow(GATE_JOB, ALPHA_JOB_TEXT))
        self.write("ci.yml", workflow(ALPHA_JOB_TEXT))
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_alpha_results_in_a_script_fails(self) -> None:
        self.write("conformance.yml", workflow(GATE_JOB))
        self.write("phase_exit.py", "RESULTS = 'build/conformance-results-alpha'\n", self.scripts)
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_job_blocks_span_to_the_next_job(self) -> None:
        text = workflow(GATE_JOB, ALPHA_JOB_TEXT)
        blocks = guard.job_blocks(text)
        self.assertEqual(set(blocks), {"gate", "conformance-2026-07-28-alpha"})
        first, last = blocks["gate"]
        self.assertEqual(blocks["conformance-2026-07-28-alpha"][0], last + 1)
        self.assertLess(first, last)


if __name__ == "__main__":
    unittest.main()
