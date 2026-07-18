from __future__ import annotations

import ast
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github/workflows"
FULL_SHA_ACTION = re.compile(r"uses:\s*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)@([0-9a-f]{40})(?:\s|$)")


class WorkflowSyntaxTests(unittest.TestCase):
    def test_workflows_have_known_structure(self) -> None:
        for path in WORKFLOWS.glob("*.yml"):
            text = path.read_text()
            self.assertIn("name:", text)
            self.assertIn("on:", text)
            self.assertIn("permissions:", text)
            self.assertIn("jobs:", text)
            self.assertNotIn("\t", text)


class SecurityPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = (WORKFLOWS / "conan-center-pr.yml").read_text()
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

    def test_pat_is_in_one_final_no_checkout_step(self) -> None:
        publish = self.workflow.split("\n  publish:\n", 1)[1]
        self.assertNotIn("actions/checkout", publish)
        self.assertEqual(publish.count("      - name:"), 1)
        self.assertEqual(publish.count("secrets.CONAN_CENTER_PR_BOT_PAT"), 1)
        self.assertTrue(publish.rstrip().endswith("PY"))

    def test_inline_client_is_valid_stdlib_python(self) -> None:
        source = self.workflow.split("# PAT_CLIENT_BEGIN\n", 1)[1].split("# PAT_CLIENT_END", 1)[0]
        source = "\n".join(line[10:] if line.startswith("          ") else line for line in source.splitlines())
        tree = ast.parse(source)
        imports = {
            alias.name.split(".")[0]
            for node in ast.walk(tree)
            if isinstance(node, (ast.Import, ast.ImportFrom))
            for alias in node.names
        }
        self.assertEqual(imports, {"http", "json", "os", "re", "sys", "urllib"})
        self.assertIn('API_HOST = "api.github.com"', source)


if __name__ == "__main__":
    unittest.main()
