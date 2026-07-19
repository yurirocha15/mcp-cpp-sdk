from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/release/validate_homebrew_candidate.sh"


class HomebrewCandidateScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_audits_and_installs_the_exact_production_formula(self) -> None:
        order = (
            "release.package_validation compare-files",
            'brew style "${production}"',
            'brew audit --strict --formula "${tap}/mcp-cpp-sdk"',
            'brew --cache --build-from-source "${tap}/mcp-cpp-sdk"',
            "release.package_validation copy-exact-file",
            'brew install --build-from-source "${tap}/mcp-cpp-sdk"',
            'brew test "${tap}/mcp-cpp-sdk"',
            "release.package_validation homebrew-consumer",
        )
        positions = [self.text.index(fragment) for fragment in order]
        self.assertEqual(positions, sorted(positions))

    def test_disables_updates_and_contains_no_publication_or_credentials(self) -> None:
        self.assertIn("HOMEBREW_NO_AUTO_UPDATE=1", self.text)
        self.assertIn("HOMEBREW_NO_ANALYTICS=1", self.text)
        self.assertNotIn("python3 -c", self.text)
        self.assertGreaterEqual(self.text.count("release.package_validation compare-files"), 2)
        self.assertNotRegex(
            self.text,
            re.compile(r"\b(?:push|upload|publish)\b|GH_TOKEN|HOMEBREW_GITHUB_API_TOKEN"),
        )

    def test_requires_exact_stable_version_and_new_work_directory(self) -> None:
        self.assertIn("test ! -e \"${work_dir}\"", self.text)
        self.assertIn("^(0|[1-9][0-9]*)", self.text)


if __name__ == "__main__":
    unittest.main()
