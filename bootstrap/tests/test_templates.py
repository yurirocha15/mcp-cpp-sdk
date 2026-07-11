from __future__ import annotations

import ast
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BROKER = ROOT / "bootstrap" / "conan-release-control"
TAP = ROOT / "bootstrap" / "homebrew-tap"
CHOCO = ROOT / "bootstrap" / "chocolatey-publisher"
ACTION = re.compile(r"uses:\s*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)@([0-9a-f]{40})(?:\s|$)")


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def workflow_inputs(text: str) -> tuple[str, ...]:
    marker = "    inputs:\n"
    start = text.index(marker) + len(marker)
    lines = text[start:].splitlines()
    names = []
    for line in lines:
        if line and not line.startswith("      "):
            break
        match = re.fullmatch(r"      ([a-z0-9_]+):", line)
        if match:
            names.append(match.group(1))
    return tuple(names)


def embedded_python(text: str) -> list[str]:
    lines = text.splitlines()
    scripts = []
    index = 0
    while index < len(lines):
        if "<<'PY'" not in lines[index]:
            index += 1
            continue
        indentation = len(lines[index]) - len(lines[index].lstrip(" "))
        terminator = " " * indentation + "PY"
        start = index + 1
        index = start
        while index < len(lines) and lines[index] != terminator:
            index += 1
        if index == len(lines):
            raise AssertionError(f"unterminated Python heredoc at line {start}")
        scripts.append("\n".join(line[indentation:] for line in lines[start:index]) + "\n")
        index += 1
    return scripts


class ManifestTests(unittest.TestCase):
    def test_manifests_are_value_free_public_contracts(self) -> None:
        for root in (BROKER, TAP, CHOCO):
            manifest = json.loads((root / "repository-settings.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema_version"], 1)
            text = json.dumps(manifest)
            self.assertNotRegex(text, r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
            self.assertNotIn("fingerprint\": \"", text.lower())
            self.assertNotIn("required_reviewer\"", text)
            self.assertNotIn("__", text)

    def test_expected_checks_equal_ci_job_names(self) -> None:
        cases = (
            (BROKER, ["actionlint", "release-control-policy", "publisher-unit-tests", "secret-scan"]),
            (TAP, ["tap-policy", "formula-style-audit", "trusted-bottle-policy"]),
            (CHOCO, ["publisher-policy", "secret-scan"]),
        )
        for root, expected in cases:
            manifest = json.loads((root / "repository-settings.json").read_text())
            workflow = (root / ".github/workflows/ci.yml").read_text()
            self.assertEqual(manifest["main_ruleset"]["required_checks"], expected)
            for check in expected:
                self.assertIn(f"name: {check}\n", workflow)


class ActionPinTests(unittest.TestCase):
    NODE24_PINS = {
        "actions/checkout": "9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0",
        "actions/create-github-app-token": "bcd2ba49218906704ab6c1aa796996da409d3eb1",
        "actions/upload-artifact": "043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
        "actions/download-artifact": "3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
        "actions/attest": "a1948c3f048ba23858d222213b7c278aabede763",
    }

    def test_every_action_reference_is_a_full_allowed_sha(self) -> None:
        for root in (BROKER, TAP, CHOCO):
            manifest = json.loads((root / "repository-settings.json").read_text())
            allowed = manifest["actions"]["allowed_actions"]
            for workflow in (root / ".github/workflows").glob("*.yml"):
                text = workflow.read_text()
                uses_lines = [line.strip() for line in text.splitlines() if "uses:" in line]
                self.assertTrue(uses_lines, workflow)
                for line in uses_lines:
                    match = ACTION.search(line)
                    self.assertIsNotNone(match, line)
                    action, sha = match.groups()
                    self.assertEqual(allowed.get(action), sha, line)

    def test_node24_action_pins_and_client_id_contract_are_fixed(self) -> None:
        broker = json.loads((BROKER / "repository-settings.json").read_text())
        tap = json.loads((TAP / "repository-settings.json").read_text())
        self.assertEqual(broker["actions"]["allowed_actions"]["actions/checkout"], self.NODE24_PINS["actions/checkout"])
        for action, sha in self.NODE24_PINS.items():
            self.assertEqual(tap["actions"]["allowed_actions"][action], sha)
        choco = json.loads((CHOCO / "repository-settings.json").read_text())
        for action in ("actions/checkout", "actions/upload-artifact", "actions/download-artifact"):
            self.assertEqual(choco["actions"]["allowed_actions"][action], self.NODE24_PINS[action])
        workflow = (TAP / ".github/workflows/publish-bottles.yml").read_text()
        self.assertIn("client-id: ${{ vars.HOMEBREW_APP_CLIENT_ID }}", workflow)
        self.assertNotIn("app-id:", workflow)

    def test_embedded_publisher_python_compiles(self) -> None:
        count = 0
        for root in (BROKER, TAP, CHOCO):
            for workflow in (root / ".github/workflows").glob("*.yml"):
                for source in embedded_python(workflow.read_text(encoding="utf-8")):
                    ast.parse(source, filename=str(workflow))
                    count += 1
        self.assertGreater(count, 0)


class BrokerTests(unittest.TestCase):
    EXPECTED_INPUTS = (
        "source_tag",
        "source_commit_sha",
        "github_release_id",
        "source_workflow_run_id",
        "release_manifest_sha256",
        "fork_branch",
        "fork_head_sha",
        "recipe_tree_sha256",
        "request_uuid",
    )

    def setUp(self) -> None:
        self.workflow = (BROKER / ".github/workflows/conan-center-pr.yml").read_text()

    def test_dispatch_contract_is_exact(self) -> None:
        self.assertEqual(workflow_inputs(self.workflow), self.EXPECTED_INPUTS)
        self.assertIn("github.ref == 'refs/heads/main'", self.workflow)
        self.assertIn("conan-center-pr.yml@refs/heads/main", self.workflow)

    def test_pat_job_has_one_last_step_and_no_checkout(self) -> None:
        publish = self.workflow.split("\n  publish:\n", 1)[1]
        self.assertNotIn("actions/checkout", publish)
        self.assertEqual(publish.count("CONAN_CENTER_PR_BOT_PAT: ${{ secrets.CONAN_CENTER_PR_BOT_PAT }}"), 1)
        self.assertEqual(publish.count("      - name:"), 1)
        self.assertTrue(publish.rstrip().endswith("PY"))

    def test_inline_pat_client_has_fixed_stdlib_routes(self) -> None:
        source = self.workflow.split("# PAT_CLIENT_BEGIN\n", 1)[1].split("# PAT_CLIENT_END", 1)[0]
        source = "\n".join(line[10:] if line.startswith("          ") else line for line in source.splitlines())
        tree = ast.parse(source)
        imported = {
            alias.name.split(".")[0]
            for node in ast.walk(tree)
            if isinstance(node, (ast.Import, ast.ImportFrom))
            for alias in node.names
        }
        self.assertEqual(imported, {"http", "json", "os", "re", "sys", "urllib"})
        for literal in (
            'API_HOST = "api.github.com"',
            '"/user"',
            '"/repos/yurirocha15/conan-center-index"',
            '"/repos/conan-io/conan-center-index"',
            '"/repos/conan-io/conan-center-index/pulls"',
        ):
            self.assertIn(literal, source)
        self.assertNotIn("requests", source)
        self.assertNotIn("subprocess", source)

    def test_dispatch_validator_rejects_extra_and_rc(self) -> None:
        module = load_module("broker_validate", BROKER / "publisher/validate_dispatch.py")
        request = {
            "source_tag": "v0.2.0",
            "source_commit_sha": "0" * 40,
            "github_release_id": "1",
            "source_workflow_run_id": "2",
            "release_manifest_sha256": "1" * 64,
            "fork_branch": "package/mcp-cpp-sdk-0.2.0",
            "fork_head_sha": "2" * 40,
            "recipe_tree_sha256": "3" * 64,
            "request_uuid": "00000000-0000-0000-0000-000000000001",
        }
        self.assertEqual(module.validate(request), request)
        request["extra"] = "x"
        with self.assertRaises(ValueError):
            module.validate(request)


class HomebrewTests(unittest.TestCase):
    EXPECTED_INPUTS = (
        "source_tag",
        "source_commit_sha",
        "github_release_id",
        "source_workflow_run_id",
        "release_manifest_sha256",
        "formula_pr_number",
        "formula_branch",
        "formula_head_sha",
        "request_uuid",
    )

    def setUp(self) -> None:
        self.workflow = (TAP / ".github/workflows/publish-bottles.yml").read_text()

    def test_dispatch_contract_is_exact(self) -> None:
        self.assertEqual(workflow_inputs(self.workflow), self.EXPECTED_INPUTS)
        self.assertNotIn("      run_id:\n", self.workflow.split("    inputs:\n", 1)[1].split("\npermissions:", 1)[0])
        self.assertIn("github.ref == 'refs/heads/main'", self.workflow)
        self.assertIn("publish-bottles.yml@refs/heads/main", self.workflow)

    def test_secret_jobs_do_not_checkout_or_build(self) -> None:
        publish = self.workflow.split("\n  publish:\n", 1)[1].split("\n  finalize:\n", 1)[0]
        finalizer = self.workflow.split("\n  finalize:\n", 1)[1]
        for job in (publish, finalizer):
            self.assertNotIn("actions/checkout", job)
            self.assertNotIn("brew install", job)
            self.assertNotIn("cmake", job)
        self.assertNotIn("brew pr-upload", finalizer)
        self.assertNotIn("docker login", finalizer)

    def test_finalizer_is_same_run_and_exact_head_bound(self) -> None:
        finalizer = self.workflow.split("\n  finalize:\n", 1)[1]
        self.assertIn("needs: [verify, publish]", finalizer)
        self.assertIn("environment: homebrew-finalize", finalizer)
        self.assertIn('test "$(jq -r \'.run_id\' "$ledger")" = "$RUN_ID"', finalizer)
        self.assertIn('test "$(jq -r \'.bottled_head_sha\' "$ledger")" = "$EXPECTED_HEAD"', finalizer)
        self.assertIn('-f merge_method=squash -f sha="$EXPECTED_HEAD"', finalizer)

    def test_handoff_rejects_wrong_run_and_digest(self) -> None:
        module = load_module("handoff_verify", TAP / "publisher/verify_handoffs.py")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bottle-tag"
            root.mkdir()
            bottle = root / "bottle.tar.gz"
            metadata = root / "bottle.json"
            bottle.write_bytes(b"bottle")
            import hashlib

            bottle_digest = hashlib.sha256(bottle.read_bytes()).hexdigest()
            metadata.write_text(
                json.dumps(
                    {
                        "root_url": "https://ghcr.io/v2/yurirocha15/mcp-cpp-sdk",
                        "tag": "tag",
                        "sha256": bottle_digest,
                    }
                )
            )

            value = {
                "schema_version": 1,
                "run_id": "10",
                "run_attempt": "1",
                "bottle_tag": "tag",
                "bottle_file": bottle.name,
                "bottle_sha256": bottle_digest,
                "json_file": metadata.name,
                "json_sha256": hashlib.sha256(metadata.read_bytes()).hexdigest(),
            }
            (root / "handoff.json").write_text(json.dumps(value))
            self.assertEqual(len(module.verify(Path(directory), "10", "1")["bottles"]), 1)
            with self.assertRaises(ValueError):
                module.verify(Path(directory), "11", "1")


if __name__ == "__main__":
    unittest.main()
