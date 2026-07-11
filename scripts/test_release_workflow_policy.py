#!/usr/bin/env python3
"""Mutation tests for the offline release-workflow policy checker."""

from __future__ import annotations

import unittest
from pathlib import Path
from typing import Callable

import check_release_workflow as policy


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

    def test_rejects_extra_trigger(self) -> None:
        mutated = self.text.replace("  workflow_dispatch:\n", "  workflow_dispatch:\n  schedule:\n", 1)
        self.assert_policy_error(lambda: policy.check_trigger(mutated.splitlines()))

    def test_rejects_floating_or_unknown_action(self) -> None:
        floating = self.text.replace(policy.ACTION_ALLOWLIST["actions/checkout"], "v4", 1)
        self.assert_policy_error(lambda: policy.check_action_pins(floating.splitlines()))
        unknown = self.text.replace("actions/checkout@", "example/checkout@", 1)
        self.assert_policy_error(lambda: policy.check_action_pins(unknown.splitlines()))

    def test_rejects_second_issues_writer(self) -> None:
        blocks = self.mutated_blocks(
            "    permissions:\n      contents: read\n      id-token: write\n",
            "    permissions:\n      contents: read\n      id-token: write\n      issues: write\n",
        )
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
            'if [[ "${RELEASE_PUBLISHING_ENABLED}" != true ]]',
            'if [[ -z "${RELEASE_PUBLISHING_ENABLED}" ]]',
        )
        mutated = self.text.replace(
            'if [[ "${RELEASE_PUBLISHING_ENABLED}" != true ]]',
            'if [[ -z "${RELEASE_PUBLISHING_ENABLED}" ]]',
            1,
        )
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, mutated))

    def test_rejects_publisher_without_anchor_handoff(self) -> None:
        blocks = {name: list(block) for name, block in self.blocks.items()}
        blocks["aur"] = [line.replace("Verify fixed anchor handoff", "Trust downloaded files") for line in blocks["aur"]]
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, self.text))

    def test_rejects_unpinned_cloudsmith_cli(self) -> None:
        mutated = self.text.replace('cli-version: "1.19.0"', 'cli-version: "latest"', 1)
        blocks = policy.job_blocks(mutated.splitlines())
        self.assert_policy_error(lambda: policy.check_publication_invariants(blocks, mutated))

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
        mutated = self.text.replace("          from datetime import date", "          from datetime import", 1)
        self.assert_policy_error(lambda: policy.check_embedded_python(mutated.splitlines()))


if __name__ == "__main__":
    unittest.main()
