from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "scripts/run_release_tool.py"


class ReleaseToolBootstrapTests(unittest.TestCase):
    def test_every_allowed_module_imports_under_isolated_python(self) -> None:
        modules = (
            "release.cloudsmith_publish",
            "release.conan_fork",
            "release.aur_publish",
            "release.construct_core",
            "release.github_publication",
            "release.github_anchor",
            "release.github_provider",
            "release.homebrew_formula",
            "release.native_build",
            "release.provider_preflight",
            "release.repository_immutability",
            "release.repository_readiness",
            "release.verify_candidate",
        )
        for module in modules:
            with self.subTest(module=module):
                result = subprocess.run(
                    [sys.executable, "-I", "-S", str(BOOTSTRAP), module, "--help"],
                    cwd=ROOT,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

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


if __name__ == "__main__":
    unittest.main()
