from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from release.workflow_gate import (
    WorkflowGateError,
    conan_publication_result,
    main,
    require_aur_validation,
    require_preparation,
    require_publishing_enabled,
    require_package_validation,
    require_validation_completion,
)


class WorkflowGateTests(unittest.TestCase):
    def test_publication_kill_switch_is_exact(self) -> None:
        require_publishing_enabled("true")
        for value in ("", "TRUE", "false", " true"):
            with self.subTest(value=value), self.assertRaises(WorkflowGateError):
                require_publishing_enabled(value)

    def test_conan_recipe_states_map_to_exact_ledger_results(self) -> None:
        self.assertEqual(
            conan_publication_result("prepared"), "DISPATCHED_PENDING_REVIEW"
        )
        self.assertEqual(
            conan_publication_result("identical"), "SKIPPED_ALREADY_IDENTICAL"
        )
        with self.assertRaises(WorkflowGateError):
            conan_publication_result("failed")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "github-output"
            self.assertEqual(
                main(
                    [
                        "conan-result",
                        "--state",
                        "prepared",
                        "--github-output",
                        str(output),
                    ]
                ),
                0,
            )
            self.assertEqual(
                output.read_text(encoding="utf-8"),
                "result=DISPATCHED_PENDING_REVIEW\n",
            )

    def test_preparation_accepts_only_the_mode_specific_publication_result(self) -> None:
        require_preparation(mode="validate", provider_result="success", publication_result="skipped")
        require_preparation(mode="publish", provider_result="success", publication_result="success")
        for values in (
            ("validate", "failure", "skipped"),
            ("validate", "success", "success"),
            ("publish", "success", "skipped"),
        ):
            with self.subTest(values=values), self.assertRaises(WorkflowGateError):
                require_preparation(
                    mode=values[0], provider_result=values[1], publication_result=values[2]
                )

    def test_aur_gate_requires_every_fresh_stable_candidate(self) -> None:
        require_aur_validation(
            release_kind="stable", anchor_exists=False, validation_result="success"
        )
        require_aur_validation(release_kind="rc", anchor_exists=False, validation_result="skipped")
        require_aur_validation(release_kind="stable", anchor_exists=True, validation_result="skipped")
        with self.assertRaises(WorkflowGateError):
            require_aur_validation(
                release_kind="stable", anchor_exists=False, validation_result="skipped"
            )

    def test_validation_completion_binds_candidate_abi_and_aur_results(self) -> None:
        require_validation_completion(
            anchor_exists=False,
            release_kind="stable",
            candidate_result="success",
            abi_result="success",
            aur_gate_result="success",
            package_gate_result="success",
        )
        require_validation_completion(
            anchor_exists=False,
            release_kind="rc",
            candidate_result="success",
            abi_result="skipped",
            aur_gate_result="success",
            package_gate_result="success",
        )
        require_validation_completion(
            anchor_exists=True,
            release_kind="stable",
            candidate_result="skipped",
            abi_result="skipped",
            aur_gate_result="success",
            package_gate_result="success",
        )
        with self.assertRaises(WorkflowGateError):
            require_validation_completion(
                anchor_exists=False,
                release_kind="stable",
                candidate_result="success",
                abi_result="skipped",
                aur_gate_result="success",
                package_gate_result="success",
            )
        with self.assertRaises(WorkflowGateError):
            require_validation_completion(
                anchor_exists=False,
                release_kind="stable",
                candidate_result="success",
                abi_result="success",
                aur_gate_result="success",
                package_gate_result="failure",
            )

    def test_package_gate_requires_all_fresh_stable_platforms(self) -> None:
        require_package_validation(
            release_kind="stable",
            anchor_exists=False,
            homebrew_result="success",
            conan_linux_result="success",
            conan_windows_result="success",
        )
        for release_kind, anchor_exists in (("rc", False), ("stable", True)):
            require_package_validation(
                release_kind=release_kind,
                anchor_exists=anchor_exists,
                homebrew_result="skipped",
                conan_linux_result="skipped",
                conan_windows_result="skipped",
            )
        with self.assertRaisesRegex(WorkflowGateError, "package validation"):
            require_package_validation(
                release_kind="stable",
                anchor_exists=False,
                homebrew_result="success",
                conan_linux_result="failure",
                conan_windows_result="success",
            )


if __name__ == "__main__":
    unittest.main()
