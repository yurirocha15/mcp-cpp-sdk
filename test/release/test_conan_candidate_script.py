from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/release/validate_conan_candidate.ps1"


class ConanCandidatePowerShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_checks_exact_windows_toolchain_and_native_architecture(self) -> None:
        for fragment in (
            "Set-StrictMode -Version Latest",
            "$env:VCToolsVersion.Trim().TrimEnd('\\') -cne $ExpectedVCToolsVersion",
            "$env:VisualStudioVersion -notmatch '^17\\.'",
            "$env:VSCMD_ARG_TGT_ARCH -cne 'x64'",
            "$env:VSCMD_ARG_HOST_ARCH -cne 'x64'",
            "$env:PROCESSOR_ARCHITECTURE -cne 'AMD64'",
            "std::cout << _MSC_VER",
            "$actualMscVer -cne [string]$ExpectedMscVer",
            "^compiler\\.version=$profileCompilerVersion$",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, self.text)

    def test_runs_exact_rendered_recipe_validator_without_publication(self) -> None:
        for fragment in (
            "release.conan_validation",
            "--assets $candidateRoot",
            "--archive $archive",
            "--build-profile $profile",
            "--host-profile $profile",
            "--lockfile $lockfile",
            "--evidence $Evidence",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, self.text)
        self.assertIsNone(
            re.search(r"\b(?:upload|publish|push|api[_-]?key|token|password)\b", self.text, re.I)
        )


if __name__ == "__main__":
    unittest.main()
