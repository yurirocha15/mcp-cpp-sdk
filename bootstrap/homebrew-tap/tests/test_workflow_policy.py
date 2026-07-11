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


if __name__ == "__main__":
    unittest.main()
