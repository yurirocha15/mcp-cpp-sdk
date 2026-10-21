from __future__ import annotations

import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "scripts/run_release_tool.py"


class ReleaseToolBootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.bin = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_tool(self, module: str, *, gh_version: str = "2.96.0") -> subprocess.CompletedProcess[str]:
        gh = self.bin / "gh"
        gh.write_text(f"#!/bin/sh\necho 'gh version {gh_version} (2026-07-02)'\n", encoding="utf-8")
        gh.chmod(0o755)
        return subprocess.run(
            [sys.executable, "-I", "-S", str(BOOTSTRAP), module, "--help"],
            cwd=ROOT,
            env={**os.environ, "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}"},
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_every_allowed_module_imports_under_isolated_python(self) -> None:
        modules = runpy.run_path(str(BOOTSTRAP))["_ALLOWED_MODULES"]
        for module in modules:
            with self.subTest(module=module):
                result = self.run_tool(module)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_networked_modules_require_a_minimum_github_cli(self) -> None:
        for version, accepted in (("2.95.9", False), ("2.96.0", True), ("2.101.0", True), ("3.0.0", True)):
            with self.subTest(version=version):
                result = self.run_tool("release.github_anchor", gh_version=version)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)

    def test_unreviewed_module_is_rejected(self) -> None:
        result = subprocess.run(
            [sys.executable, "-I", "-S", str(BOOTSTRAP), "pathlib", "--help"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("allowed", result.stderr)

    def test_github_cli_version_covers_every_networked_module(self) -> None:
        namespace = runpy.run_path(str(BOOTSTRAP))
        self.assertEqual(namespace["_GH_MINIMUM_VERSION"], (2, 96, 0))
        self.assertEqual(
            namespace["_GH_MODULES"],
            {
                "release.github_anchor",
                "release.github_provider",
                "release.github_publication",
                "release.provider_preflight",
                "release.repository_readiness",
            },
        )


if __name__ == "__main__":
    unittest.main()
