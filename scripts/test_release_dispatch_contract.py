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
            "operation": "validate-all",
            "conan2": "false",
            "apt": "false",
            "rpm": "false",
            "aur": "false",
            "homebrew": "false",
            "chocolatey": "false",
            "confirmation": "validate-all:v0.2.0:all",
            **TRUSTED_CONTEXT,
        }
        arguments.update(overrides)
        return validate_dispatch(**arguments)

    def test_accepts_stable_all_channels(self) -> None:
        contract = self.validate()
        self.assertEqual(contract.release_kind, "stable")
        self.assertEqual(contract.version, "0.2.0")
        self.assertEqual(contract.workflow_outputs()["target_conan2"], "true")
        self.assertEqual(contract.workflow_outputs()["target_apt"], "true")
        self.assertEqual(contract.workflow_outputs()["target_rpm"], "true")
        self.assertEqual(
            contract.workflow_outputs()["cloudsmith_matrix"],
            '[{"format":"apt"},{"format":"rpm"}]',
        )

    def test_accepts_github_only_release_candidate(self) -> None:
        contract = self.validate(
            tag="v0.2.0-rc.1",
            operation="validate-selected",
            confirmation="validate-selected:v0.2.0-rc.1:github",
        )
        self.assertEqual(contract.release_kind, "rc")
        self.assertEqual(contract.channels, ())
        self.assertEqual(contract.selection_label, "github")

    def test_accepts_staged_channel_checkboxes(self) -> None:
        contract = self.validate(
            operation="publish-selected",
            apt="true",
            rpm="true",
            confirmation="publish-selected:v0.2.0:github,apt,rpm",
        )
        self.assertEqual(contract.channels, ("apt", "rpm"))
        self.assertEqual(contract.selection_label, "github,apt,rpm")
        self.assertEqual(contract.workflow_outputs()["target_homebrew"], "false")
        self.assertEqual(
            contract.workflow_outputs()["cloudsmith_matrix"],
            '[{"format":"apt"},{"format":"rpm"}]',
        )

    def test_accepts_github_only_stable_release(self) -> None:
        contract = self.validate(
            operation="publish-selected",
            confirmation="publish-selected:v0.2.0:github",
        )
        self.assertEqual(contract.channels, ())
        self.assertEqual(contract.workflow_outputs()["normalized_channels"], "")

    def test_individual_checkboxes_are_normalized_in_canonical_order(self) -> None:
        contract = self.validate(
            operation="publish-selected",
            conan2="true",
            apt="true",
            rpm="true",
            aur="true",
            homebrew="true",
            chocolatey="true",
            confirmation="publish-selected:v0.2.0:github,conan2,apt,rpm,aur,homebrew,chocolatey",
        )
        self.assertEqual(contract.channels, ("conan2", "apt", "rpm", "aur", "homebrew", "chocolatey"))

    def test_rejects_rc_external_channels(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(
                tag="v0.2.0-rc.1",
                operation="validate-selected",
                homebrew="true",
                confirmation="validate-selected:v0.2.0-rc.1:github,homebrew",
            )

    def test_rejects_leading_zero_semver(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(tag="v0.02.0", confirmation="validate-all:v0.02.0:all")

    def test_rejects_one_x_until_multi_platform_abi_policy_exists(self) -> None:
        for tag in ("v1.0.0", "v1.0.0-rc.1"):
            with self.subTest(tag=tag), self.assertRaisesRegex(
                ContractError, "multi-platform ABI policy"
            ):
                self.validate(tag=tag, confirmation=f"validate-all:{tag}:all")

    def test_rejects_noncanonical_boolean_values(self) -> None:
        for value in ("True", "1", "", "false "):
            with self.subTest(value=value), self.assertRaises(ContractError):
                self.validate(apt=value)

    def test_rejects_all_combined_with_individual_channels(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(apt="true")

    def test_rejects_unknown_operation(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(operation="publish")

    def test_rejects_wrong_confirmation(self) -> None:
        with self.assertRaises(ContractError):
            self.validate(confirmation="publish-all:v0.2.0:all")

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
