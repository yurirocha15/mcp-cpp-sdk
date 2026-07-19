#!/usr/bin/env python3
"""Mutation tests for the offline release-workflow policy checker."""

from __future__ import annotations

import unittest
from unittest import mock
from pathlib import Path
import tempfile
from typing import Callable

import check_release_workflow as policy
from workflow_yaml import (
    WorkflowYamlError,
    validate_workflow_yaml,
    validate_workflows,
    workflow_paths,
)


class ReleaseWorkflowPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = Path(".github/workflows/release.yml").read_text(encoding="utf-8")
        cls.lines = cls.text.splitlines()
        cls.blocks = policy.job_blocks(cls.lines)

    def assert_policy_error(self, callback: Callable[[], object]) -> None:
        with self.assertRaises(policy.PolicyError):
            callback()

    def mutated_blocks(self, old: str, new: str, count: int = 1) -> dict[str, list[str]]:
        mutated = self.text.replace(old, new, count)
        self.assertNotEqual(mutated, self.text)
        return policy.job_blocks(mutated.splitlines())

    def test_current_workflow_passes(self) -> None:
        self.assertEqual(policy.main(), 0)

    def test_strict_duplicate_key_validation_covers_every_workflow(self) -> None:
        paths = workflow_paths()
        self.assertEqual(
            {str(path) for path in paths},
            {
                ".github/workflows/ci.yml",
                ".github/workflows/docs.yml",
                ".github/workflows/release-builders.yml",
                ".github/workflows/release.yml",
                "bootstrap/chocolatey-publisher/.github/workflows/ci.yml",
                "bootstrap/chocolatey-publisher/.github/workflows/publish.yml",
                "bootstrap/conan-release-control/.github/workflows/ci.yml",
                "bootstrap/conan-release-control/.github/workflows/conan-center-pr.yml",
                "bootstrap/homebrew-tap/.github/workflows/ci.yml",
                "bootstrap/homebrew-tap/.github/workflows/publish-bottles.yml",
            },
        )
        validate_workflows(paths)
        for path in paths:
            text = path.read_text(encoding="utf-8")
            with self.subTest(path=path), self.assertRaises(WorkflowYamlError):
                validate_workflow_yaml(f"name: duplicate\n{text}", label=str(path))

    def test_strict_duplicate_key_validation_handles_nested_and_sequence_mappings(self) -> None:
        for document in (
            "jobs:\n  policy:\n    timeout-minutes: 5\n    timeout-minutes: 10\n",
            "jobs:\n  policy:\n    steps:\n      - name: first\n        name: duplicate\n",
            "name: first\n\"name\": duplicate\n",
        ):
            with self.subTest(document=document), self.assertRaises(WorkflowYamlError):
                validate_workflow_yaml(document)
        validate_workflow_yaml(
            "jobs:\n  policy:\n    steps:\n      - run: |\n          name: shell text\n"
            "      - run: echo safe\n"
        )

    def test_rejects_extra_trigger(self) -> None:
        mutated = self.text.replace("  workflow_dispatch:\n", "  workflow_dispatch:\n  schedule:\n", 1)
        self.assert_policy_error(lambda: policy.check_trigger(mutated.splitlines()))

    def test_rejects_floating_or_unknown_action(self) -> None:
        floating = self.text.replace(policy.ACTION_ALLOWLIST["actions/checkout"], "v4", 1)
        self.assert_policy_error(lambda: policy.check_action_pins(floating.splitlines()))
        unknown = self.text.replace("actions/checkout@", "example/checkout@", 1)
        self.assert_policy_error(lambda: policy.check_action_pins(unknown.splitlines()))

    def test_rejects_second_issues_writer(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["signing"] = [
            line.replace("      issues: read", "      issues: write")
            for line in blocks["signing"]
        ]
        self.assert_policy_error(lambda: policy.check_permissions(blocks))

    def test_rejects_privileged_checkout_or_source_execution(self) -> None:
        for fragment in (
            "      - uses: actions/checkout@" + policy.ACTION_ALLOWLIST["actions/checkout"],
            "        run: python3 scripts/build.py",
            "        run: cmake --build build",
        ):
            with self.subTest(fragment=fragment):
                blocks = {name: list(block) for name, block in self.blocks.items()}
                blocks["aur"].append(fragment)
                self.assert_policy_error(lambda blocks=blocks: policy.check_privileged_jobs(blocks))

    def test_rejects_secret_outside_environment_or_unknown_secret(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["construct-core"].append("          BAD: ${{ secrets.BAD }}")
        self.assert_policy_error(lambda: policy.check_secret_scope(blocks))
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["signing"].append("          BAD: ${{ secrets.BAD }}")
        self.assert_policy_error(lambda: policy.check_secret_scope(blocks))

    def test_rejects_disabled_or_ambiguous_kill_switch(self) -> None:
        blocks = self.mutated_blocks(
            "release.workflow_gate publishing",
            "release.workflow_gate preparation",
        )
        mutated = self.text.replace(
            "release.workflow_gate publishing",
            "release.workflow_gate preparation",
            1,
        )
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, mutated))

    def test_rejects_package_validation_bypass_before_anchor(self) -> None:
        for job in ("validation-complete", "attestation", "github-release"):
            blocks = {name: list(block) for name, block in self.blocks.items()}
            blocks[job] = [
                line for line in blocks[job] if "package-validation-gate" not in line
            ]
            with self.subTest(job=job):
                self.assert_policy_error(
                    lambda blocks=blocks: policy.check_artifact_construction(blocks)
                )

    def test_rejects_publisher_without_anchor_handoff(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["aur"] = [line.replace("Verify fixed anchor handoff", "Trust downloaded files") for line in blocks["aur"]]
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, self.text))

    def test_rejects_anchor_without_prior_manifest_conflict_gate(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["github-anchor"] = [
            line.replace("PRIOR_MANIFEST_SHA256", "IGNORED_PRIOR_MANIFEST")
            for line in blocks["github-anchor"]
        ]
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, self.text))

    def test_rejects_unpaginated_release_asset_verification(self) -> None:
        source = Path("release/github_anchor.py").read_text(encoding="utf-8")
        mutated = source.replace('"--paginate"', '"--no-pagination"', 1)
        self.assertNotEqual(mutated, source)
        self.assert_policy_error(lambda: policy.check_github_anchor_source(mutated))

    def test_rejects_github_release_adapter_without_live_tag_or_absence_gate(self) -> None:
        source = Path("release/github_publication.py").read_text(encoding="utf-8")
        for old, new in (
            ("verify_live_github_tag(", "trust_live_github_tag("),
            ("probe_existing_release(", "assume_release_absent("),
        ):
            mutated = source.replace(old, new)
            self.assertNotEqual(mutated, source)
            with self.subTest(old=old):
                self.assert_policy_error(
                    lambda mutated=mutated: policy.check_github_release_source(mutated)
                )

    def test_rejects_inline_github_publication_logic(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["github-release"].append("        run: gh release create unsafe")
        self.assert_policy_error(
            lambda: policy.check_publication_invariants(blocks, self.text)
        )

    def test_rejects_publisher_without_checkbox_gate(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["homebrew"] = [
            line.replace(
                "needs.contract.outputs.target_homebrew == 'true'",
                "needs.contract.outputs.mode == 'publish'",
            )
            for line in blocks["homebrew"]
        ]
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, self.text))

    def test_rejects_provider_preflight_without_checkbox_gate(self) -> None:
        for job, expected in policy.PROVIDER_PREFLIGHT_CONDITIONS.items():
            blocks = {name: list(block) for name, block in self.blocks.items()}
            blocks[job] = [
                line.replace(expected, "${{ always() }}")
                for line in blocks[job]
            ]
            self.assertNotEqual(blocks[job], self.blocks[job])
            with self.subTest(job=job):
                self.assert_policy_error(
                    lambda blocks=blocks: policy.check_publication_invariants(
                        blocks, self.text
                    )
                )

    def test_rejects_mutated_aur_preflight_package_target(self) -> None:
        preflight = Path("scripts/release/preflight_aur.sh").read_text(encoding="utf-8")
        publisher = Path("scripts/release/publish_aur.sh").read_text(encoding="utf-8")
        mutated = preflight.replace(
            '--package "${AUR_PACKAGE_BASE}"', "--package attacker-target", 1
        )
        self.assertNotEqual(mutated, preflight)
        self.assert_policy_error(
            lambda: policy.check_aur_target_binding(
                self.blocks, mutated, publisher
            )
        )

    def test_rejects_mutated_aur_publisher_repository_target(self) -> None:
        preflight = Path("scripts/release/preflight_aur.sh").read_text(encoding="utf-8")
        publisher = Path("scripts/release/publish_aur.sh").read_text(encoding="utf-8")
        mutated = publisher.replace(
            "ssh://aur@aur.archlinux.org/mcp-cpp-sdk.git",
            "ssh://aur@aur.archlinux.org/attacker-target.git",
            1,
        )
        self.assertNotEqual(mutated, publisher)
        self.assert_policy_error(
            lambda: policy.check_aur_target_binding(
                self.blocks, preflight, mutated
            )
        )

    def test_each_aur_job_requires_signed_target_binding(self) -> None:
        for job in ("preflight-aur", "aur"):
            blocks = {name: list(block) for name, block in self.blocks.items()}
            blocks[job] = [
                line.replace("release.publication_contract", "release.construct_core")
                for line in blocks[job]
            ]
            self.assertNotEqual(blocks[job], self.blocks[job])
            with self.subTest(job=job):
                self.assert_policy_error(
                    lambda blocks=blocks: policy.check_aur_target_binding(blocks)
                )

    def test_each_cloudsmith_boundary_requires_payload_verification(self) -> None:
        for job in ("preflight-cloudsmith", "publish-apt", "publish-rpm"):
            blocks = {name: list(block) for name, block in self.blocks.items()}
            blocks[job] = [
                line.replace(
                    "run: bash scripts/release/verify_cloudsmith_cli.sh",
                    "run: true",
                )
                for line in blocks[job]
            ]
            self.assertNotEqual(blocks[job], self.blocks[job])
            with self.subTest(job=job):
                self.assert_policy_error(
                    lambda blocks=blocks: policy.check_cloudsmith_cli_integrity(blocks)
                )

    def test_rejects_mutated_cloudsmith_cli_digest(self) -> None:
        verifier = policy.CLOUDSMITH_VERIFY_PATH.read_text(encoding="utf-8")
        mutated = verifier.replace(
            "c076e4b002ee07f26774c0f8a9134f52a73b16a3fb10adb31891475485e28038",
            "0" * 64,
            1,
        )
        self.assertNotEqual(mutated, verifier)
        self.assert_policy_error(
            lambda: policy.check_cloudsmith_cli_integrity(self.blocks, mutated)
        )

    def test_rejects_target_gated_native_artifact_build(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["build-apt"] = [
            line.replace(
                "needs.contract.outputs.release_kind == 'stable'",
                "needs.contract.outputs.target_apt == 'true'",
            )
            for line in blocks["build-apt"]
        ]
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_rejects_repository_readiness_without_existing_anchor_policy(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["repository-readiness"] = [
            line.replace("--anchor-exists", "--unsafe-ignore-anchor")
            for line in blocks["repository-readiness"]
        ]
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_rejects_historical_release_without_retained_signer_policy(self) -> None:
        source = Path("release/repository_readiness.py").read_text(encoding="utf-8")
        mutated = source.replace(
            "accepted_signers = registry.signers",
            "accepted_signers = (registry.active,)",
            1,
        )
        self.assertNotEqual(mutated, source)
        self.assert_policy_error(
            lambda: policy.check_repository_readiness_source(mutated)
        )

    def test_rejects_target_gated_immutable_assembly(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        replaced = False
        mutated = []
        for line in blocks["assemble-unsigned"]:
            if not replaced and "if: ${{ needs.contract.outputs.release_kind == 'stable' }}" in line:
                line = line.replace(
                    "needs.contract.outputs.release_kind == 'stable'",
                    "needs.contract.outputs.target_apt == 'true'",
                )
                replaced = True
            mutated.append(line)
        self.assertTrue(replaced)
        blocks["assemble-unsigned"] = mutated
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_rejects_target_gated_aur_construction_validation(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["validate-aur-packages"] = [
            line.replace(
                "needs.contract.outputs.release_kind == 'stable'",
                "needs.contract.outputs.target_aur == 'true'",
            )
            for line in blocks["validate-aur-packages"]
        ]
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_rejects_aur_gate_that_fails_read_only_validation(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["aur-validation-gate"] = [
            line.replace("release.workflow_gate aur", "release.workflow_gate bypass")
            for line in blocks["aur-validation-gate"]
        ]
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_rejects_attestation_before_native_validation(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["attestation"] = [
            line for line in blocks["attestation"] if "- aur-validation-gate" not in line
        ]
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_rejects_anchor_before_package_manager_validation(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["github-release"] = [
            line for line in blocks["github-release"]
            if "- package-validation-gate" not in line
        ]
        self.assert_policy_error(lambda: policy.check_package_manager_validation(blocks))

    def test_rejects_selected_channel_only_package_validation(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["validate-homebrew-package"] = [
            line.replace(
                "needs.contract.outputs.release_kind == 'stable'",
                "needs.contract.outputs.target_homebrew == 'true'",
            )
            for line in blocks["validate-homebrew-package"]
        ]
        self.assert_policy_error(lambda: policy.check_package_manager_validation(blocks))

    def test_rejects_missing_abi_comparison(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["build-abi"] = [
            line.replace("release.abi_build container-compare", "release.abi_build validate-identity")
            for line in blocks["build-abi"]
        ]
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_every_credential_boundary_rejects_a_missing_ledger_recheck(self) -> None:
        for job in (
            "immutability-gate", "preflight-cloudsmith", "preflight-homebrew",
            "preflight-aur", "preflight-chocolatey", "preflight-conan-fork", "preflight-conan-broker",
            "signing", "attestation", "github-release", "publish-apt", "publish-rpm",
            "aur", "homebrew", "chocolatey", "conan-recipe", "conan",
            "ledger-coordinator",
        ):
            blocks = {name: list(block) for name, block in self.blocks.items()}
            blocks[job] = [
                line.replace("scripts/release/recheck_ledger.sh", "true")
                for line in blocks[job]
            ]
            with self.subTest(job=job):
                self.assert_policy_error(lambda blocks=blocks: policy.check_credential_rechecks(blocks))

    def test_each_privileged_dispatcher_requires_a_tagged_checkout(self) -> None:
        for job in ("homebrew", "chocolatey", "conan"):
            blocks = {name: list(block) for name, block in self.blocks.items()}
            blocks[job] = [line for line in blocks[job] if "uses: actions/checkout@" not in line]
            with self.subTest(job=job):
                self.assert_policy_error(lambda blocks=blocks: policy.check_privileged_jobs(blocks))

    def test_rejects_publication_without_verified_candidate_handoff(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["github-release"] = [
            line.replace("release-candidate-verified-", "release-signed-")
            for line in blocks["github-release"]
            if "- candidate-gate" not in line
        ]
        self.assert_policy_error(lambda: policy.check_artifact_construction(blocks))

    def test_rejects_incomplete_native_target_inventory(self) -> None:
        source = Path(policy.NATIVE_TARGETS_PATH).read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "targets.json"
            values = __import__("json").loads(source)
            path.write_text(__import__("json").dumps(values[:-1]), encoding="utf-8")
            self.assert_policy_error(lambda: policy.check_native_targets(path))

    def test_rejects_aur_without_passphrase_protection(self) -> None:
        aur = "\n".join(self.blocks["aur"])
        for path in (
            "scripts/release/publish_aur.sh",
            "scripts/release/aur_ssh_agent.sh",
            "release/aur_ssh.py",
        ):
            aur += "\n" + Path(path).read_text(encoding="utf-8")
        mutated = aur.replace("SSH_ASKPASS_REQUIRE=force", "SSH_ASKPASS_REQUIRE=never", 1)
        self.assertNotEqual(mutated, aur)
        self.assert_policy_error(lambda: policy.check_aur_publisher_text(mutated))

    def test_rejects_aur_without_batch_mode(self) -> None:
        aur = "\n".join(self.blocks["aur"])
        for path in (
            "scripts/release/publish_aur.sh",
            "scripts/release/aur_ssh_agent.sh",
            "release/aur_ssh.py",
        ):
            aur += "\n" + Path(path).read_text(encoding="utf-8")
        mutated = aur.replace("BatchMode=yes", "BatchMode=no", 1)
        self.assertNotEqual(mutated, aur)
        self.assert_policy_error(lambda: policy.check_aur_publisher_text(mutated))

    def test_rejects_aur_without_the_reviewed_host_fingerprint(self) -> None:
        aur = "\n".join(self.blocks["aur"])
        for path in (
            "scripts/release/publish_aur.sh",
            "scripts/release/aur_ssh_agent.sh",
            "release/aur_ssh.py",
        ):
            aur += "\n" + Path(path).read_text(encoding="utf-8")
        mutated = aur.replace(
            "SHA256:RFzBCUItH9LZS0cKB5UE6ceAYhBD5C8GeOBip8Z11+4",
            "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
            1,
        )
        self.assertNotEqual(mutated, aur)
        self.assert_policy_error(lambda: policy.check_aur_publisher_text(mutated))

    def test_rejects_unpinned_cloudsmith_cli(self) -> None:
        mutated = self.text.replace('cli-version: "1.19.0"', 'cli-version: "latest"', 1)
        blocks = policy.job_blocks(mutated.splitlines())
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, mutated))

    def test_rejects_cloudsmith_publisher_without_exact_service_identity(self) -> None:
        original = (
            "CLOUDSMITH_PUBLISH_SERVICE_USERNAME: "
            "${{ vars.CLOUDSMITH_PUBLISH_SERVICE_USERNAME }}"
        )
        mutated = self.text.replace(
            original,
            "CLOUDSMITH_PUBLISH_SERVICE_USERNAME: wrong-publisher",
            1,
        )
        self.assertNotEqual(mutated, self.text)
        blocks = policy.job_blocks(mutated.splitlines())
        self.assert_policy_error(
            lambda: policy.check_publication_invariants(blocks, mutated)
        )

    def test_rejects_dispatcher_without_direct_control_preflight_dependency(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        original = blocks["homebrew"]
        blocks["homebrew"] = [
            line for line in original if line.strip() != "- preflight-homebrew"
        ]
        self.assertNotEqual(blocks["homebrew"], original)
        self.assert_policy_error(
            lambda: policy.check_publication_invariants(blocks, self.text)
        )

    def test_rejects_dispatcher_that_drops_preflighted_control_sha(self) -> None:
        mutated = self.text.replace(
            '--provider-control-sha "${PROVIDER_CONTROL_SHA}"',
            '--provider-control-sha "0000000000000000000000000000000000000000"',
            1,
        )
        self.assertNotEqual(mutated, self.text)
        blocks = policy.job_blocks(mutated.splitlines())
        self.assert_policy_error(
            lambda: policy.check_publication_invariants(blocks, mutated)
        )

    def test_rejects_immutability_gate_without_admin_read(self) -> None:
        mutated = self.text.replace("permission-administration: read", "permission-contents: read", 1)
        blocks = policy.job_blocks(mutated.splitlines())
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, mutated))

    def test_rejects_live_result_claim(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["aur"].append("        run: echo 'result=LIVE' >> \"${GITHUB_OUTPUT}\"")
        self.assert_policy_error(lambda: policy.check_fixed_outputs(blocks))

    def test_rejects_persisted_checkout_credentials(self) -> None:
        mutated = self.text.replace("persist-credentials: false", "persist-credentials: true", 1)
        blocks = policy.job_blocks(mutated.splitlines())
        self.assert_policy_error(lambda: policy.check_checkout_safety(mutated.splitlines(), blocks))

    def test_rejects_hardcoded_numeric_repository_id(self) -> None:
        contract = Path(policy.CONTRACT_PATH).read_text(encoding="utf-8")
        self.assertIsNone(
            __import__("re").search(r'EXPECTED_(?:REPOSITORY|OWNER)_ID\s*=\s*"[0-9]+"', contract)
        )

    def test_rejects_invalid_embedded_python(self) -> None:
        mutated = self.text + "\n        run: python3 - <<'PY'\n"
        self.assert_policy_error(lambda: policy.check_embedded_python(mutated.splitlines()))

    def test_rejects_direct_release_module_execution(self) -> None:
        mutated = self.text + "\n        run: python3 -I -S release/verify_candidate.py\n"
        self.assert_policy_error(lambda: policy.check_embedded_python(mutated.splitlines()))

    def test_requires_checksum_pinned_actionlint_for_every_workflow(self) -> None:
        policy.check_required_actionlint()
        runner_path = policy.ACTIONLINT_RUNNER_PATH
        original = runner_path.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            mutated_path = Path(directory) / "lint_workflows.sh"
            mutated_path.write_text(
                original.replace("sha256sum --check --status", "true"), encoding="utf-8"
            )
            with mock.patch.object(policy, "ACTIONLINT_RUNNER_PATH", mutated_path):
                self.assert_policy_error(policy.check_required_actionlint)

    def test_requires_windows_powershell_syntax_validation(self) -> None:
        ci_source = policy.CI_WORKFLOW_PATH.read_text(encoding="utf-8")
        checker_source = policy.POWERSHELL_SYNTAX_PATH.read_text(encoding="utf-8")
        policy.check_windows_powershell_syntax_ci(ci_source, checker_source)
        self.assert_policy_error(
            lambda: policy.check_windows_powershell_syntax_ci(
                ci_source.replace("shell: pwsh", "shell: bash", 1),
                checker_source,
            )
        )
        self.assert_policy_error(
            lambda: policy.check_windows_powershell_syntax_ci(
                ci_source,
                checker_source.replace("if ($errors.Count -ne 0)", "if ($false)", 1),
            )
        )


if __name__ == "__main__":
    unittest.main()
