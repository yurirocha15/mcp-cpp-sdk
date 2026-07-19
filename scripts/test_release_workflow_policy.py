#!/usr/bin/env python3
"""Mutation tests for the small release-workflow policy boundary."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

import check_release_workflow as policy
from workflow_yaml import WorkflowYamlError, validate_workflow_yaml, validate_workflows, workflow_paths


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/release.yml"


class ReleaseWorkflowPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")
        self.lines = self.text.splitlines()
        self.blocks = policy.job_blocks(self.lines)

    def assert_policy_error(self, callback) -> None:
        with self.assertRaises(policy.PolicyError):
            callback()

    def mutated_blocks(self) -> dict[str, list[str]]:
        return {name: list(block) for name, block in self.blocks.items()}

    def test_current_workflow_passes(self) -> None:
        self.assertEqual(policy.main(), 0)

    def test_duplicate_key_validation_covers_every_workflow(self) -> None:
        validate_workflows(workflow_paths(ROOT))
        with self.assertRaises(WorkflowYamlError):
            validate_workflow_yaml("jobs:\n  release:\n    permissions:\n      contents: read\n      contents: write\n")
        with self.assertRaises(WorkflowYamlError):
            validate_workflow_yaml("steps:\n  - uses: actions/checkout@abc\n    with:\n      ref: one\n      ref: two\n")

    def test_rejects_extra_trigger_or_dispatch_input(self) -> None:
        lines = list(self.lines)
        lines.insert(lines.index("permissions:"), "  schedule:")
        self.assert_policy_error(lambda: policy.check_trigger_and_dispatch(lines, "\n".join(lines)))
        lines = [line for line in self.lines if line.strip() != "confirmation:"]
        self.assert_policy_error(lambda: policy.check_trigger_and_dispatch(lines, "\n".join(lines)))

    def test_rejects_floating_or_unknown_action(self) -> None:
        for replacement in ("actions/checkout@v7", "unknown/action@" + "a" * 40):
            lines = [
                line.replace(
                    "actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0",
                    replacement,
                    1,
                )
                for line in self.lines
            ]
            self.assert_policy_error(lambda lines=lines: policy.check_action_pins(lines))

    def test_rejects_job_inventory_or_permission_changes(self) -> None:
        blocks = self.mutated_blocks()
        del blocks["candidate-gate"]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))
        blocks = self.mutated_blocks()
        blocks["github-release"] = [
            line.replace("contents: write", "contents: read")
            for line in blocks["github-release"]
        ]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

    def test_rejects_missing_environment_or_unprotected_secret(self) -> None:
        blocks = self.mutated_blocks()
        blocks["signing"] = [line for line in blocks["signing"] if "environment:" not in line]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))
        blocks = self.mutated_blocks()
        blocks["policy"].append("        TOKEN: ${{ secrets.RELEASE_GPG_PASSPHRASE }}")
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

    def test_rejects_unknown_secret_or_persisted_checkout_credentials(self) -> None:
        blocks = self.mutated_blocks()
        blocks["signing"].append("        TOKEN: ${{ secrets.UNKNOWN_RELEASE_TOKEN }}")
        self.assert_policy_error(lambda: policy.check_job_security(blocks))
        blocks = self.mutated_blocks()
        blocks["signing"] = [
            line.replace("persist-credentials: false", "persist-credentials: true")
            for line in blocks["signing"]
        ]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

    def test_rejects_privileged_checkout_not_pinned_to_dispatch_sha(self) -> None:
        blocks = self.mutated_blocks()
        blocks["homebrew"] = [
            line.replace("ref: ${{ github.sha }}", "ref: main")
            for line in blocks["homebrew"]
        ]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

    def test_rejects_publisher_checkbox_or_anchor_bypass(self) -> None:
        blocks = self.mutated_blocks()
        blocks["homebrew"] = [
            line.replace("needs.contract.outputs.target_homebrew == 'true'", "true")
            for line in blocks["homebrew"]
        ]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))
        blocks = self.mutated_blocks()
        blocks["homebrew"] = [line for line in blocks["homebrew"] if "github-anchor" not in line]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))

    def test_rejects_preflight_or_publication_kill_switch_bypass(self) -> None:
        blocks = self.mutated_blocks()
        blocks["preflight-aur"] = [
            line.replace("needs.contract.outputs.target_aur == 'true'", "true")
            for line in blocks["preflight-aur"]
        ]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))
        blocks = self.mutated_blocks()
        blocks["preparation-gate"] = [
            line.replace("release.workflow_gate publishing", "release.workflow_gate bypass")
            for line in blocks["preparation-gate"]
        ]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))

    def test_rejects_selected_only_build_or_validation(self) -> None:
        blocks = self.mutated_blocks()
        blocks["build-native"] = [
            line.replace(
                "needs.contract.outputs.release_kind == 'stable'",
                "needs.contract.outputs.target_apt == 'true'",
            )
            for line in blocks["build-native"]
        ]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))
        blocks = self.mutated_blocks()
        blocks["validate-conan-linux"] = [
            line.replace("candidate-gate", "preparation-gate")
            for line in blocks["validate-conan-linux"]
        ]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))

    def test_rejects_package_or_github_anchor_gate_bypass(self) -> None:
        blocks = self.mutated_blocks()
        blocks["package-validation-gate"] = [
            line.replace("--aur-result", "--skip-result")
            for line in blocks["package-validation-gate"]
        ]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))
        blocks = self.mutated_blocks()
        blocks["github-release"] = [
            line for line in blocks["github-release"] if "package-validation-gate" not in line
        ]
        self.assert_policy_error(lambda: policy.check_job_graph(blocks, self.text))

    def test_rejects_return_of_mutable_issue_ledger(self) -> None:
        self.assert_policy_error(
            lambda: policy.check_job_graph(self.blocks, self.text + "\nledger-coordinator:")
        )

    def test_rejects_inline_python_or_missing_policy_hook(self) -> None:
        self.assert_policy_error(
            lambda: policy.check_delegation_and_ci(self.blocks, self.text + "\npython3 - <<'PY'")
        )
        blocks = self.mutated_blocks()
        blocks["policy"] = [
            line.replace("scripts/check_release_workflow.py", "true")
            for line in blocks["policy"]
        ]
        self.assert_policy_error(lambda: policy.check_delegation_and_ci(blocks, self.text))


if __name__ == "__main__":
    unittest.main()
