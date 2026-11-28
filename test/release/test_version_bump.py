from __future__ import annotations

from datetime import date
from pathlib import Path
import shutil
import tempfile
import unittest

from release.model import SemVer
from release.version_bump import (
    PINS,
    VersionBumpError,
    check_pins,
    cut_changelog,
    prepare,
    read_version,
    soversion,
)


ROOT = Path(__file__).resolve().parents[2]
CHANGELOG = """# Changelog

## [Unreleased]

### Highlights

- The fix.

### Fixed

- A fix.

## [0.3.0] - 2026-11-17

### Added

- A feature.
"""


class VersionBumpTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for relative in {"VERSION", *(pin.path for pin in PINS)}:
            (self.root / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, self.root / relative)
        (self.root / "CHANGELOG.md").write_text(CHANGELOG, encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_repository_documentation_names_its_version(self) -> None:
        version = read_version(ROOT)
        if version.is_prerelease:
            self.skipTest("a release candidate keeps the documentation at the last stable release")
        self.assertEqual(check_pins(ROOT, version), [])

    def test_prepare_sets_every_pin_and_dates_the_changelog(self) -> None:
        current = read_version(self.root)
        new = SemVer(current.major, current.minor + 1, 0)
        changed = prepare(self.root, new, date(2026, 12, 1))

        self.assertEqual(read_version(self.root), new)
        self.assertEqual(check_pins(self.root, new), [])
        self.assertIn("VERSION", changed)
        self.assertIn("docs/installation/windows.rst", changed)
        changelog = (self.root / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertIn(f"## [Unreleased]\n\n## [{new}] - 2026-12-01\n\n### Highlights\n\n- The fix.\n", changelog)
        self.assertIn("## [0.3.0] - 2026-11-17", changelog)

    def test_release_candidate_leaves_the_documentation_alone(self) -> None:
        current = read_version(self.root)
        before = {pin.path: (self.root / pin.path).read_text(encoding="utf-8") for pin in PINS}
        candidate = SemVer(current.major, current.minor + 1, 0, rc=1)
        self.assertEqual(prepare(self.root, candidate, date(2026, 12, 1)), ["CHANGELOG.md", "VERSION"])
        for path, text in before.items():
            self.assertEqual((self.root / path).read_text(encoding="utf-8"), text, path)
        self.assertIn(f"## [{candidate}] - 2026-12-01", (self.root / "CHANGELOG.md").read_text(encoding="utf-8"))

    def test_prepare_refuses_a_version_that_does_not_follow(self) -> None:
        current = read_version(self.root)
        for version in (current, SemVer(current.major, current.minor, current.patch, rc=1)):
            with self.subTest(version=str(version)), self.assertRaisesRegex(VersionBumpError, "does not follow"):
                prepare(self.root, version, date(2026, 12, 1))

    def test_check_reports_a_stale_or_missing_pin(self) -> None:
        version = read_version(self.root)
        readme = self.root / "README.md"
        readme.write_text(readme.read_text(encoding="utf-8").replace(f"GIT_TAG v{version}", "GIT_TAG v0.0.1"), encoding="utf-8")
        self.assertEqual(len(check_pins(self.root, version)), 1)
        self.assertIn("names 0.0.1", check_pins(self.root, version)[0])
        readme.write_text("no pin here\n", encoding="utf-8")
        self.assertIn("found 0", check_pins(self.root, version)[0])

    def test_changelog_cut_rules(self) -> None:
        version = SemVer(0, 4, 0)
        with self.assertRaisesRegex(VersionBumpError, "Highlights"):
            cut_changelog(CHANGELOG.replace("### Highlights\n\n- The fix.\n\n", ""), version, date(2026, 12, 1))
        with self.assertRaisesRegex(VersionBumpError, "Highlights"):
            cut_changelog(CHANGELOG.replace("### Highlights\n\n- The fix.\n\n### Fixed\n\n- A fix.\n\n", ""), version, date(2026, 12, 1))
        with self.assertRaisesRegex(VersionBumpError, "already has a section"):
            cut_changelog(CHANGELOG, SemVer(0, 3, 0), date(2026, 12, 1))
        with self.assertRaisesRegex(VersionBumpError, "exactly one"):
            cut_changelog(CHANGELOG.replace("## [Unreleased]", "## Unreleased"), version, date(2026, 12, 1))

    def test_soversion_is_the_core_version_before_one(self) -> None:
        self.assertEqual(soversion(SemVer(0, 4, 0)), "0.4.0")
        self.assertEqual(soversion(SemVer(0, 4, 0, rc=2)), "0.4.0")
        self.assertEqual(soversion(SemVer(1, 2, 3)), "1")


if __name__ == "__main__":
    unittest.main()
