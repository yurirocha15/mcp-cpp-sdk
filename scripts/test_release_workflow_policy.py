#!/usr/bin/env python3
"""Mutation tests for the small release-workflow policy boundary."""

from __future__ import annotations

from pathlib import Path
import unittest

import check_release_workflow as policy
from workflow_yaml import WorkflowYamlError, validate_workflow_yaml, validate_workflows, workflow_paths


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/release.yml"
CANDIDATE_WORKFLOW = ROOT / ".github/workflows/release-candidate.yml"
PUBLICATION_WORKFLOW = ROOT / ".github/workflows/release-publication.yml"
PROVIDER_PREFLIGHT_WORKFLOW = ROOT / ".github/workflows/release-provider-preflight.yml"
PROVIDER_DISPATCH_WORKFLOW = ROOT / ".github/workflows/release-provider-dispatch.yml"


class ReleaseWorkflowPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")
        self.lines = self.text.splitlines()
        self.blocks = policy.job_blocks(self.lines)
        self.candidate_text = CANDIDATE_WORKFLOW.read_text(encoding="utf-8")
        self.candidate_blocks = policy.job_blocks(self.candidate_text.splitlines())
        self.publication_text = PUBLICATION_WORKFLOW.read_text(encoding="utf-8")
        self.publication_blocks = policy.job_blocks(self.publication_text.splitlines())
        self.provider_preflight_blocks = policy.job_blocks(
            PROVIDER_PREFLIGHT_WORKFLOW.read_text(encoding="utf-8").splitlines()
        )
        self.provider_dispatch_blocks = policy.job_blocks(
            PROVIDER_DISPATCH_WORKFLOW.read_text(encoding="utf-8").splitlines()
        )
        self.combined_text = "\n".join(
            (self.text, self.candidate_text, self.publication_text)
        )

    def assert_policy_error(self, callback) -> None:
        with self.assertRaises(policy.PolicyError):
            callback()

    @staticmethod
    def copied(blocks: dict[str, list[str]]) -> dict[str, list[str]]:
        return {name: list(block) for name, block in blocks.items()}

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
        lines = [line for line in self.lines if line.strip() != "tag:"]
        self.assert_policy_error(lambda: policy.check_trigger_and_dispatch(lines, "\n".join(lines)))

    def test_rejects_non_call_trigger_on_reusable_workflow(self) -> None:
        lines = self.candidate_text.splitlines()
        lines.insert(lines.index("permissions:"), "  workflow_dispatch:")
        self.assert_policy_error(
            lambda: policy.check_reusable_trigger(lines, "candidate")
        )

    def test_rejects_floating_or_unknown_action(self) -> None:
        for replacement in ("actions/checkout@v7", "unknown/action@" + "a" * 40):
            lines = [
                line.replace(
                    "actions/checkout@" + policy.ACTION_ALLOWLIST["actions/checkout"],
                    replacement,
                    1,
                )
                for line in self.lines
            ]
            self.assert_policy_error(lambda lines=lines: policy.check_action_pins(lines))

    def test_rejects_job_inventory_or_permission_changes(self) -> None:
        blocks = self.copied(self.blocks)
        del blocks["candidate"]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

        provider_blocks = self.copied(self.provider_dispatch_blocks)
        provider_blocks["dispatch"] = [
            line.replace("contents: read", "contents: write")
            for line in provider_blocks["dispatch"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_security(
                provider_blocks,
                {"dispatch": policy.READ},
                {"dispatch": "${{ inputs.environment }}"},
            )
        )
        blocks = self.copied(self.blocks)
        blocks["github-release"] = [
            line.replace("contents: write", "contents: read")
            for line in blocks["github-release"]
        ]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

    def test_rejects_missing_environment_or_unprotected_secret(self) -> None:
        blocks = self.copied(self.candidate_blocks)
        blocks["signing"] = [line for line in blocks["signing"] if "environment:" not in line]
        self.assert_policy_error(
            lambda: policy.check_job_security(
                blocks,
                policy.CANDIDATE_JOB_PERMISSIONS,
                policy.CANDIDATE_ENVIRONMENTS,
            )
        )
        blocks = self.copied(self.blocks)
        blocks["policy"].append("        TOKEN: ${{ secrets.RELEASE_GPG_PASSPHRASE }}")
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

    def test_forwarded_secrets_match_the_reviewed_calls(self) -> None:
        policy.check_job_security(self.blocks)
        policy.check_job_security(
            self.publication_blocks,
            policy.PUBLICATION_JOB_PERMISSIONS,
            policy.PUBLICATION_ENVIRONMENTS,
            policy.PUBLICATION_FORWARDED_SECRETS,
        )
        self.assertEqual(
            policy.forwarded_secrets(self.blocks["candidate"])[0],
            policy.MAIN_FORWARDED_SECRETS["candidate"],
        )

    def test_rejects_extra_missing_renamed_or_misplaced_forwarded_secret(self) -> None:
        blocks = self.copied(self.blocks)
        index = blocks["candidate"].index("    secrets:")
        blocks["candidate"].insert(
            index + 1, "      RELEASE_ADMIN_APP_PRIVATE_KEY: ${{ secrets.RELEASE_ADMIN_APP_PRIVATE_KEY }}"
        )
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

        blocks = self.copied(self.blocks)
        blocks["candidate"] = [
            line for line in blocks["candidate"] if "RELEASE_GPG_PASSPHRASE" not in line
        ]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

        blocks = self.copied(self.blocks)
        blocks["candidate"] = [
            line.replace(
                "RELEASE_GPG_PASSPHRASE: ${{ secrets.RELEASE_GPG_PASSPHRASE }}",
                "RELEASE_GPG_PASSPHRASE: ${{ secrets.CLOUDSMITH_API_KEY }}",
            )
            for line in blocks["candidate"]
        ]
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

        blocks = self.copied(self.blocks)
        blocks["policy"].extend(
            ["    secrets:", "      RELEASE_GPG_PASSPHRASE: ${{ secrets.RELEASE_GPG_PASSPHRASE }}"]
        )
        self.assert_policy_error(lambda: policy.check_job_security(blocks))

    def test_rejects_unknown_secret_or_persisted_checkout_credentials(self) -> None:
        blocks = self.copied(self.candidate_blocks)
        blocks["signing"].append("        TOKEN: ${{ secrets.UNKNOWN_RELEASE_TOKEN }}")
        self.assert_policy_error(
            lambda: policy.check_job_security(
                blocks,
                policy.CANDIDATE_JOB_PERMISSIONS,
                policy.CANDIDATE_ENVIRONMENTS,
            )
        )
        blocks = self.copied(self.candidate_blocks)
        blocks["signing"] = [
            line.replace("persist-credentials: false", "persist-credentials: true")
            for line in blocks["signing"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_security(
                blocks,
                policy.CANDIDATE_JOB_PERMISSIONS,
                policy.CANDIDATE_ENVIRONMENTS,
            )
        )

    def test_rejects_privileged_checkout_not_pinned_to_dispatch_sha(self) -> None:
        blocks = self.copied(self.candidate_blocks)
        blocks["signing"] = [
            line.replace("ref: ${{ github.sha }}", "ref: main")
            for line in blocks["signing"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_security(
                blocks,
                policy.CANDIDATE_JOB_PERMISSIONS,
                policy.CANDIDATE_ENVIRONMENTS,
            )
        )

    def test_rejects_publisher_checkbox_or_anchor_bypass(self) -> None:
        publication_blocks = self.copied(self.publication_blocks)
        publication_blocks["homebrew"] = [
            line.replace("inputs.target_homebrew == 'true'", "true")
            for line in publication_blocks["homebrew"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                self.blocks,
                self.combined_text,
                self.candidate_blocks,
                publication_blocks,
            )
        )
        publication_blocks = self.copied(self.publication_blocks)
        publication_blocks["homebrew"].append("    secrets: inherit")
        unsafe_text = self.combined_text + "\n    secrets: inherit"
        self.assert_policy_error(
            lambda: policy.check_provider_workflows(
                self.blocks,
                publication_blocks,
                unsafe_text,
            )
        )
        publication_blocks = self.copied(self.publication_blocks)
        publication_blocks["homebrew"] = [
            line.replace("release-provider-dispatch.yml", "unreviewed-provider.yml")
            for line in publication_blocks["homebrew"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                self.blocks,
                self.combined_text,
                self.candidate_blocks,
                publication_blocks,
            )
        )

    def test_rejects_provider_dispatch_before_direct_publishers(self) -> None:
        def graph(publication_blocks):
            return lambda: policy.check_job_graph(
                self.blocks,
                self.combined_text,
                self.candidate_blocks,
                publication_blocks,
            )

        for job in ("homebrew", "chocolatey", "conan"):
            with self.subTest(job=job):
                publication_blocks = self.copied(self.publication_blocks)
                publication_blocks[job] = [
                    line for line in publication_blocks[job] if line != "      - aur"
                ]
                self.assert_policy_error(graph(publication_blocks))

                publication_blocks = self.copied(self.publication_blocks)
                publication_blocks[job] = [
                    line.replace("!cancelled()", "always()")
                    for line in publication_blocks[job]
                ]
                self.assert_policy_error(graph(publication_blocks))

                publication_blocks = self.copied(self.publication_blocks)
                publication_blocks[job] = [
                    line.replace('"success","skipped"]\'), needs.aur', '"success","failure"]\'), needs.aur')
                    for line in publication_blocks[job]
                ]
                self.assert_policy_error(graph(publication_blocks))

    def test_rejects_preflight_or_publication_kill_switch_bypass(self) -> None:
        blocks = self.copied(self.blocks)
        blocks["preflight-aur"] = [
            line.replace("needs.contract.outputs.target_aur == 'true'", "true")
            for line in blocks["preflight-aur"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                blocks,
                self.combined_text,
                self.candidate_blocks,
                self.publication_blocks,
            )
        )
        blocks = self.copied(self.blocks)
        blocks["candidate"] = [
            line.replace("needs.preparation-gate.result == 'success'", "true")
            for line in blocks["candidate"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                blocks,
                self.combined_text,
                self.candidate_blocks,
                self.publication_blocks,
            )
        )
        blocks = self.copied(self.blocks)
        blocks["preparation-gate"] = [
            line.replace("release.workflow_gate publishing", "release.workflow_gate bypass")
            for line in blocks["preparation-gate"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                blocks,
                self.combined_text,
                self.candidate_blocks,
                self.publication_blocks,
            )
        )

    def test_rejects_selected_only_build_or_validation(self) -> None:
        candidate_blocks = self.copied(self.candidate_blocks)
        candidate_blocks["build-native"] = [
            line.replace(
                "inputs.release_kind == 'stable'",
                "inputs.target_apt == 'true'",
            )
            for line in candidate_blocks["build-native"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                self.blocks,
                self.combined_text,
                candidate_blocks,
                self.publication_blocks,
            )
        )
        candidate_blocks = self.copied(self.candidate_blocks)
        candidate_blocks["validate-conan-linux"] = [
            line.replace("candidate-gate", "preparation-gate")
            for line in candidate_blocks["validate-conan-linux"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                self.blocks,
                self.combined_text,
                candidate_blocks,
                self.publication_blocks,
            )
        )

    def test_rejects_package_or_github_anchor_gate_bypass(self) -> None:
        candidate_blocks = self.copied(self.candidate_blocks)
        candidate_blocks["package-validation-gate"] = [
            line.replace("--aur-result", "--skip-result")
            for line in candidate_blocks["package-validation-gate"]
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                self.blocks,
                self.combined_text,
                candidate_blocks,
                self.publication_blocks,
            )
        )
        blocks = self.copied(self.blocks)
        blocks["github-release"] = [
            line for line in blocks["github-release"] if line.strip() != "- candidate"
        ]
        self.assert_policy_error(
            lambda: policy.check_job_graph(
                blocks,
                self.combined_text,
                self.candidate_blocks,
                self.publication_blocks,
            )
        )

    def test_rejects_inline_python_or_missing_policy_hook(self) -> None:
        self.assert_policy_error(
            lambda: policy.check_delegation_and_ci(self.blocks, self.text + "\npython3 - <<'PY'")
        )
        blocks = self.copied(self.blocks)
        blocks["policy"] = [
            line.replace("scripts/check_release_workflow.py", "true")
            for line in blocks["policy"]
        ]
        self.assert_policy_error(lambda: policy.check_delegation_and_ci(blocks, self.text))


if __name__ == "__main__":
    unittest.main()
