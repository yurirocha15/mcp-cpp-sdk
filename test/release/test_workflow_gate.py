from __future__ import annotations

import unittest

from release.workflow_gate import (
    WorkflowGateError,
    require_publishing_enabled,
    require_package_validation,
)


class WorkflowGateTests(unittest.TestCase):
    def test_publication_kill_switch_is_exact(self) -> None:
        require_publishing_enabled("true")
        for value in ("", "TRUE", "false", " true"):
            with self.subTest(value=value), self.assertRaises(WorkflowGateError):
                require_publishing_enabled(value)

    def test_package_gate_requires_all_fresh_stable_platforms(self) -> None:
        require_package_validation(
            release_kind="stable",
            anchor_exists=False,
            homebrew_result="success",
            conan_linux_result="success",
            conan_windows_result="success",
            aur_result="success",
        )
        for release_kind, anchor_exists in (("rc", False), ("stable", True)):
            require_package_validation(
                release_kind=release_kind,
                anchor_exists=anchor_exists,
                homebrew_result="skipped",
                conan_linux_result="skipped",
                conan_windows_result="skipped",
                aur_result="skipped",
            )
        with self.assertRaisesRegex(WorkflowGateError, "package validation"):
            require_package_validation(
                release_kind="stable",
                anchor_exists=False,
                homebrew_result="success",
                conan_linux_result="failure",
                conan_windows_result="success",
                aur_result="success",
            )
        with self.assertRaisesRegex(WorkflowGateError, "package validation"):
            require_package_validation(
                release_kind="stable",
                anchor_exists=False,
                homebrew_result="success",
                conan_linux_result="success",
                conan_windows_result="success",
                aur_result="skipped",
            )


if __name__ == "__main__":
    unittest.main()
