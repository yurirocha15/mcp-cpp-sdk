#!/usr/bin/env python3
"""Unit tests for strict release workflow-dispatch validation."""

from __future__ import annotations

import unittest

from release_dispatch_contract import ContractError, validate_dispatch


TRUSTED_CONTEXT = {
    "event_name": "workflow_dispatch",
    "workflow_ref": "refs/heads/main",
    "repository": "example/mcp-cpp-sdk",
    "repository_id": "123456789",
    "owner_id": "987654321",
    "expected_repository": "example/mcp-cpp-sdk",
    "expected_repository_id": "123456789",
    "expected_owner_id": "987654321",
}


class ReleaseDispatchContractTest(unittest.TestCase):
    def validate(self, **overrides):
        arguments = {
            "tag": "v0.2.0",
            "mode": "validate",
            "targets_input": "all",
            "ledger_issue": "42",
            "confirmation": "validate:v0.2.0:all:42",
            **TRUSTED_CONTEXT,
        }
        arguments.update(overrides)
        return validate_dispatch(**arguments)

    def test_accepts_stable_all_targets(self) -> None:
        contract = self.validate()
        self.assertEqual(contract.release_kind, "stable")
        self.assertEqual(contract.version, "0.2.0")
        self.assertEqual(contract.workflow_outputs()["target_conan2"], "true")
        self.assertEqual(contract.workflow_outputs()["target_apt"], "true")
        self.assertEqual(contract.workflow_outputs()["target_rpm"], "true")

    def test_accepts_github_only_rc(self) -> None:
        contract = self.validate(
            tag="v0.2.0-rc.1",
            targets_input="github",
            confirmation="validate:v0.2.0-rc.1:github:42",
        )
        self.assertEqual(contract.release_kind, "rc")
        self.assertEqual(contract.targets, ("github",))

    def test_accepts_canonical_partial_retry_targets(self) -> None:
        contract = self.validate(
            mode="publish",
            targets_input="apt,rpm,aur",
            confirmation="publish:v0.2.0:apt,rpm,aur:42",
        )
        self.assertEqual(contract.targets, ("apt", "rpm", "aur"))
        self.assertTrue(contract.retry)

    def test_full_stable_and_rc_publications_are_not_retries(self) -> None:
        stable = self.validate(
            mode="publish",
            confirmation="publish:v0.2.0:all:42",
        )
        release_candidate = self.validate(
            tag="v0.2.0-rc.1",
            mode="publish",
            targets_input="github",
            confirmation="publish:v0.2.0-rc.1:github:42",
        )
        self.assertFalse(stable.retry)
        self.assertFalse(release_candidate.retry)

    def test_rejects_rc_external_targets(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(
                tag="v0.2.0-rc.1",
                confirmation="validate:v0.2.0-rc.1:all:42",
            )

    def test_rejects_leading_zero_semver(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(tag="v0.02.0", confirmation="validate:v0.02.0:all:42")

    def test_rejects_duplicate_or_out_of_order_targets(self) -> None:
        for targets in ("aur,github", "github,github", "github, apt"):
            with self.subTest(targets=targets), self.assertRaises(ContractError):
                self.validate(targets_input=targets)

    def test_rejects_wrong_confirmation(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(confirmation="publish:v0.2.0:all:42")

    def test_rejects_noncanonical_ledger_issue(self) -> None:
        for issue in ("", "0", "01", "1 2"):
            with self.subTest(issue=issue), self.assertRaises(ContractError):
                self.validate(ledger_issue=issue)

    def test_rejects_non_main_workflow_ref(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(workflow_ref="refs/heads/release-test")

    def test_rejects_wrong_numeric_repository_identity(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(repository_id="1")

    def test_rejects_missing_or_mismatched_expected_identity_variables(self) -> None:
        for field, value in (
            ("expected_repository", ""),
            ("expected_repository_id", ""),
            ("expected_owner_id", "01"),
            ("expected_repository_id", "1"),
        ):
            with self.subTest(field=field), self.assertRaises(ContractError):
                self.validate(**{field: value})


if __name__ == "__main__":
    unittest.main()
