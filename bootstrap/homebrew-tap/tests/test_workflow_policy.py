from __future__ import annotations

import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github/workflows"
FULL_SHA_ACTION = re.compile(r"uses:\s*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)@([0-9a-f]{40})(?:\s|$)")


class SecurityPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = (WORKFLOWS / "publish-bottles.yml").read_text()
        self.ci_workflow = (WORKFLOWS / "ci.yml").read_text()
        self.manifest = json.loads((ROOT / "repository-settings.json").read_text())

    def test_actions_are_full_sha_allowlisted(self) -> None:
        allowed = self.manifest["actions"]["allowed_actions"]
        for path in WORKFLOWS.glob("*.yml"):
            for line in path.read_text().splitlines():
                if "uses:" not in line:
                    continue
                match = FULL_SHA_ACTION.search(line)
                self.assertIsNotNone(match, line)
                self.assertEqual(allowed.get(match.group(1)), match.group(2))

    def test_secret_jobs_never_checkout_or_build(self) -> None:
        publish = self.workflow.split("\n  publish:\n", 1)[1].split("\n  finalize:\n", 1)[0]
        finalize = self.workflow.split("\n  finalize:\n", 1)[1]
        for job in (publish, finalize):
            self.assertNotIn("actions/checkout", job)
            self.assertNotIn("brew install", job)
            self.assertNotIn("cmake", job)
        self.assertNotIn("brew pr-upload", finalize)
        self.assertIn("needs: [verify, publish]", finalize)

    def test_contents_token_is_minted_only_after_upload_and_public_resolution(self) -> None:
        publish = self.workflow.split("\n  publish:\n", 1)[1].split("\n  finalize:\n", 1)[0]
        token = publish.index("Create repository-scoped publisher App token")
        self.assertLess(publish.index("brew pr-upload"), token)
        self.assertLess(publish.index("publisher/ghcr.py record"), token)
        self.assertNotIn("<<'PY'", publish)
        finalize = self.workflow.split("\n  finalize:\n", 1)[1]
        final_token = finalize.index("Create repository-scoped publisher App token")
        self.assertLess(finalize.index("publisher/ghcr.py verify"), final_token)
        self.assertNotIn("<<'PY'", finalize)

    def test_artifacts_and_reruns_are_attempt_bound(self) -> None:
        for value in (
            "bottle-${{ matrix.tag }}-${{ github.run_id }}-${{ github.run_attempt }}",
            "publication-bundle-${{ github.run_id }}-${{ github.run_attempt }}",
            "finalization-ledger-${{ github.run_id }}-${{ github.run_attempt }}",
        ):
            self.assertIn(value, self.workflow)
        self.assertIn("--timeout-seconds 3600", self.workflow)
        self.assertIn("timeout-minutes: 70", self.workflow)
        self.assertIn("publisher/consumer_test.py", self.workflow)

    def test_initial_tap_bootstrap_is_the_only_formula_audit_exception(self) -> None:
        self.assertIn(
            "python3 -I -S publisher/formula_audit.py --formula Formula/mcp-cpp-sdk.rb",
            self.ci_workflow,
        )
        self.assertNotIn("if [[", self.ci_workflow)
        helper = (ROOT / "publisher/formula_audit.py").read_text(encoding="utf-8")
        for fragment in (
            'event_name == "push"',
            'ref_name == "main"',
            '("brew", "style", formula.as_posix())',
            '("brew", "audit", "--strict", formula.as_posix())',
        ):
            self.assertIn(fragment, helper)


if __name__ == "__main__":
    unittest.main()
