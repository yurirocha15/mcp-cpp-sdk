#!/usr/bin/env python3
"""Tests for check_alpha_runner_guard: the runner pins, the alpha install and where the alpha runner may run."""

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

ALPHA_JOB_TEXT = """  alpha-runner:
    runs-on: ubuntu-24.04
    continue-on-error: true
    steps:
      - run: bash conformance/run_alpha.sh build/release build/conformance-results-alpha
"""


def workflow(*jobs: str) -> str:
    return "name: test\non: push\njobs:\n" + "".join(jobs)


ALPHA_VERSION = "0.2.0-alpha.12"
ALPHA_RUN_SH_TEXT = """REQUIREMENT_SETS=(2026-07-28 2025-11-25)
RUNNER="$SCRIPT_DIR/runner-alpha/node_modules/.bin/conformance"
BASELINE="$SCRIPT_DIR/expected-failures-alpha.yml"
expected="$(python3 -c "..." "$SCRIPT_DIR/runner-alpha/package.json")"
"$RUNNER" server --requirements "$set" --expected-failures "$BASELINE"
"$RUNNER" client --requirements "$set" --expected-failures "$BASELINE"
curl http://127.0.0.1:3002/mcp
"""
ALPHA_INSTALL_STEP = "      - run: npm ci --ignore-scripts --prefix conformance/runner-alpha\n"


class AlphaInstallTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.package_json = self.root / "package.json"
        self.lockfile = self.root / "package-lock.json"
        self.run_sh = self.root / "run_alpha.sh"
        self.baseline = self.root / "expected-failures-alpha.yml"
        self.workflow = self.root / "conformance-alpha.yml"
        self.run_sh.write_text(ALPHA_RUN_SH_TEXT, encoding="utf-8")
        self.baseline.write_text("client: []\nserver: []\n", encoding="utf-8")
        self.workflow.write_text(workflow(ALPHA_JOB_TEXT + ALPHA_INSTALL_STEP), encoding="utf-8")
        self.pin(ALPHA_VERSION)
        self.lock(ALPHA_VERSION, ALPHA_VERSION)

    def pin(self, version: str) -> None:
        self.package_json.write_text(
            json.dumps({"dependencies": {guard.RUNNER_PACKAGE: version}}), encoding="utf-8"
        )

    def lock(self, wanted: str, locked: str) -> None:
        self.lockfile.write_text(
            json.dumps(
                {
                    "packages": {
                        "": {"dependencies": {guard.RUNNER_PACKAGE: wanted}},
                        f"node_modules/{guard.RUNNER_PACKAGE}": {"version": locked},
                    }
                }
            ),
            encoding="utf-8",
        )

    def harness(self) -> None:
        guard.check_alpha_harness(self.run_sh, self.baseline)

    def test_exact_prerelease_pin_passes(self) -> None:
        for version in ("0.2.0-alpha.12", "0.2.0", "1.0.0-rc.1"):
            with self.subTest(version=version):
                self.pin(version)
                self.assertEqual(guard.read_alpha_pin(self.package_json), version)

    def test_range_or_tag_pin_fails(self) -> None:
        for version in ("^0.2.0-alpha.12", "~0.2.0", ">=0.2.0-alpha.1", "alpha", "latest", "0.2"):
            with self.subTest(version=version):
                self.pin(version)
                with self.assertRaises(guard.GuardError):
                    guard.read_alpha_pin(self.package_json)

    def test_missing_package_json_fails(self) -> None:
        self.package_json.unlink()
        with self.assertRaises(guard.GuardError):
            guard.read_alpha_pin(self.package_json)

    def test_matching_lockfile_passes(self) -> None:
        guard.check_alpha_lockfile(ALPHA_VERSION, self.lockfile)

    def test_missing_lockfile_fails(self) -> None:
        self.lockfile.unlink()
        with self.assertRaisesRegex(guard.GuardError, "does not exist"):
            guard.check_alpha_lockfile(ALPHA_VERSION, self.lockfile)

    def test_lockfile_with_another_version_fails(self) -> None:
        self.lock(ALPHA_VERSION, "0.2.0-alpha.11")
        with self.assertRaisesRegex(guard.GuardError, "regenerate"):
            guard.check_alpha_lockfile(ALPHA_VERSION, self.lockfile)

    def test_lockfile_with_another_range_fails(self) -> None:
        self.lock("^0.2.0-alpha.12", ALPHA_VERSION)
        with self.assertRaises(guard.GuardError):
            guard.check_alpha_lockfile(ALPHA_VERSION, self.lockfile)

    def test_lockfile_without_the_runner_fails(self) -> None:
        self.lockfile.write_text(json.dumps({"packages": {"": {}}}), encoding="utf-8")
        with self.assertRaises(guard.GuardError):
            guard.check_alpha_lockfile(ALPHA_VERSION, self.lockfile)

    def test_harness_passes(self) -> None:
        self.harness()

    def test_harness_fetching_with_npx_fails(self) -> None:
        self.run_sh.write_text(ALPHA_RUN_SH_TEXT + "npx --yes pkg\n", encoding="utf-8")
        with self.assertRaisesRegex(guard.GuardError, "npx"):
            self.harness()

    def test_harness_with_its_own_version_fails(self) -> None:
        self.run_sh.write_text(ALPHA_RUN_SH_TEXT + 'VERSION="0.2.0"\n', encoding="utf-8")
        with self.assertRaisesRegex(guard.GuardError, "version of its own"):
            self.harness()

    def test_harness_missing_a_requirement_set_fails(self) -> None:
        for requirement_set in guard.ALPHA_REQUIREMENT_SETS:
            with self.subTest(requirement_set=requirement_set):
                self.run_sh.write_text(
                    ALPHA_RUN_SH_TEXT.replace(requirement_set, "1999-01-01", 1), encoding="utf-8"
                )
                with self.assertRaisesRegex(guard.GuardError, requirement_set):
                    self.harness()

    def test_harness_without_the_baseline_flag_fails(self) -> None:
        self.run_sh.write_text(
            ALPHA_RUN_SH_TEXT.replace(' --expected-failures "$BASELINE"', "", 1), encoding="utf-8"
        )
        with self.assertRaisesRegex(guard.GuardError, "expected-failures"):
            self.harness()

    def test_harness_not_reading_the_pin_fails(self) -> None:
        self.run_sh.write_text(
            ALPHA_RUN_SH_TEXT.replace("runner-alpha/package.json", "package.json"), encoding="utf-8"
        )
        with self.assertRaises(guard.GuardError):
            self.harness()

    def test_missing_baseline_fails(self) -> None:
        self.baseline.unlink()
        with self.assertRaises(guard.GuardError):
            self.harness()

    def test_workflow_with_the_locked_install_passes(self) -> None:
        guard.check_alpha_install_step(self.workflow)

    def test_workflow_without_the_locked_install_fails(self) -> None:
        for step in (
            "      - run: npm install --prefix conformance/runner-alpha\n",
            "      - run: npm ci --prefix conformance/runner-alpha\n",
            "      - run: npm ci --ignore-scripts --prefix conformance/runner\n",
            "      - run: npx --yes pkg\n",
        ):
            with self.subTest(step=step):
                self.workflow.write_text(workflow(ALPHA_JOB_TEXT + step), encoding="utf-8")
                with self.assertRaises(guard.GuardError):
                    guard.check_alpha_install_step(self.workflow)

    def test_workflow_with_the_install_and_npx_fails(self) -> None:
        text = workflow(ALPHA_JOB_TEXT + ALPHA_INSTALL_STEP + "      - run: npx --yes pkg\n")
        self.workflow.write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(guard.GuardError, "npx"):
            guard.check_alpha_install_step(self.workflow)

    def test_repository_alpha_install_passes(self) -> None:
        guard.check_alpha_lockfile(guard.read_alpha_pin())
        guard.check_alpha_harness()
        guard.check_alpha_install_step()


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
            "name: x\njobs:\n  alpha-runner:\n    continue-on-error: true\n"
            "    steps:\n      - run: echo ok\n# note\n\n  gate:\n"
            "    needs: [alpha-runner]\n    steps:\n"
            "      - run: bash conformance/run_alpha.sh\n"
        )
        self.write("conformance-alpha.yml", text)
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_no_alpha_job_passes(self) -> None:
        self.write("conformance-alpha.yml", workflow(GATE_JOB))
        self.check()

    def test_non_gating_alpha_job_passes(self) -> None:
        self.write("conformance-alpha.yml", workflow(GATE_JOB, ALPHA_JOB_TEXT))
        self.check()

    def test_alpha_job_first_in_the_file_passes(self) -> None:
        self.write("conformance-alpha.yml", workflow(ALPHA_JOB_TEXT, GATE_JOB))
        self.check()

    def test_alpha_job_without_continue_on_error_fails(self) -> None:
        job = ALPHA_JOB_TEXT.replace("    continue-on-error: true\n", "")
        self.write("conformance-alpha.yml", workflow(GATE_JOB, job))
        with self.assertRaisesRegex(guard.GuardError, "continue-on-error"):
            self.check()

    def test_continue_on_error_on_a_step_does_not_count(self) -> None:
        job = ALPHA_JOB_TEXT.replace("    continue-on-error: true\n", "").replace(
            "      - run:", "      - continue-on-error: true\n        run:"
        )
        self.write("conformance-alpha.yml", workflow(GATE_JOB, job))
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_alpha_runner_in_another_job_fails(self) -> None:
        gate = GATE_JOB + "      - run: bash conformance/run_alpha.sh\n"
        self.write("conformance-alpha.yml", workflow(gate, ALPHA_JOB_TEXT))
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_job_needing_the_alpha_job_fails(self) -> None:
        gate = GATE_JOB + "    needs: alpha-runner\n"
        self.write("conformance-alpha.yml", workflow(gate, ALPHA_JOB_TEXT))
        with self.assertRaisesRegex(guard.GuardError, "outside it"):
            self.check()

    def test_alpha_runner_in_another_workflow_fails(self) -> None:
        self.write("conformance-alpha.yml", workflow(GATE_JOB, ALPHA_JOB_TEXT))
        self.write("ci.yml", workflow(ALPHA_JOB_TEXT))
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_alpha_runner_in_the_old_workflow_fails(self) -> None:
        self.write("conformance.yml", workflow(GATE_JOB, ALPHA_JOB_TEXT))
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_ci_result_needing_the_alpha_job_fails(self) -> None:
        self.write("conformance-alpha.yml", workflow(GATE_JOB, ALPHA_JOB_TEXT))
        result = "  result:\n    if: always()\n    needs: [changes, alpha-runner]\n    steps:\n      - run: true\n"
        self.write("ci.yml", workflow(GATE_JOB, result))
        with self.assertRaisesRegex(guard.GuardError, "outside it"):
            self.check()

    def test_workflow_calling_the_alpha_workflow_fails(self) -> None:
        self.write("conformance-alpha.yml", workflow(GATE_JOB, ALPHA_JOB_TEXT))
        caller = "  alpha:\n    uses: ./.github/workflows/conformance-alpha.yml\n"
        self.write("ci.yml", workflow(GATE_JOB, caller))
        with self.assertRaisesRegex(guard.GuardError, "only it may do"):
            self.check()

    def test_alpha_workflow_naming_itself_in_its_triggers_passes(self) -> None:
        text = workflow(GATE_JOB, ALPHA_JOB_TEXT).replace(
            "on: push\n", "on:\n  push:\n    paths:\n      - '.github/workflows/conformance-alpha.yml'\n"
        )
        self.write("conformance-alpha.yml", text)
        self.check()

    def test_alpha_results_in_a_script_fails(self) -> None:
        self.write("conformance-alpha.yml", workflow(GATE_JOB))
        self.write("phase_exit.py", "RESULTS = 'build/conformance-results-alpha'\n", self.scripts)
        with self.assertRaises(guard.GuardError):
            self.check()

    def test_job_blocks_span_to_the_next_job(self) -> None:
        text = workflow(GATE_JOB, ALPHA_JOB_TEXT)
        blocks = guard.job_blocks(text)
        self.assertEqual(set(blocks), {"gate", "alpha-runner"})
        first, last = blocks["gate"]
        self.assertEqual(blocks["alpha-runner"][0], last + 1)
        self.assertLess(first, last)


if __name__ == "__main__":
    unittest.main()
