from __future__ import annotations

import unittest

from release.model import SemVer, ValidationError
from release.release_notes import MAX_NOTES_CHARACTERS, github_anchor, release_notes

REPOSITORY = "yurirocha15/mcp-cpp-sdk"
CHANGELOG = """# Changelog

## [Unreleased]

### Fixed

- Not released yet.

## [0.4.0] - 2026-12-01

### Migrating from 0.3.0

- `old_name()` is `new_name()`.

### Highlights

- **Faster.** Requests take half the time.
- **Simpler.** `client.sync()` makes blocking calls.

### Fixed

- A fix that is not a highlight.

## [0.3.0] - 2026-11-17

### Highlights

- The previous release.
"""


class ReleaseNotesTests(unittest.TestCase):
    def test_notes_carry_the_highlights_and_links_at_the_tag(self) -> None:
        notes = release_notes(CHANGELOG, SemVer(0, 4, 0), repository=REPOSITORY)
        tree = f"https://github.com/{REPOSITORY}/blob/v0.4.0"
        self.assertTrue(notes.startswith("## Highlights\n\n- **Faster.** Requests take half the time.\n"))
        self.assertIn("- **Simpler.** `client.sync()` makes blocking calls.\n", notes)
        self.assertIn(
            f"## Upgrading from 0.3.0\n\nCode written for 0.3.0 needs changes: see "
            f"[Migrating from 0.3.0]({tree}/CHANGELOG.md#migrating-from-030).\n",
            notes,
        )
        self.assertIn(f"- [Full 0.4.0 changelog]({tree}/CHANGELOG.md#040---2026-12-01)\n", notes)
        self.assertIn(f"- [Installation]({tree}/docs/installation.rst)", notes)
        for text in ("A fix that is not a highlight", "Not released yet", "The previous release"):
            self.assertNotIn(text, notes)

    def test_a_section_without_migration_notes_has_no_upgrade_part(self) -> None:
        notes = release_notes(CHANGELOG, SemVer(0, 3, 0), repository=REPOSITORY)
        self.assertIn("- The previous release.\n", notes)
        self.assertNotIn("Upgrading", notes)

    def test_missing_section_or_highlights_fail(self) -> None:
        with self.assertRaisesRegex(ValidationError, "no dated section"):
            release_notes(CHANGELOG, SemVer(0, 5, 0), repository=REPOSITORY)
        without = CHANGELOG.replace("### Highlights\n\n- **Faster.** Requests take half the time.\n"
                                    "- **Simpler.** `client.sync()` makes blocking calls.\n\n", "")
        with self.assertRaisesRegex(ValidationError, "Highlights"):
            release_notes(without, SemVer(0, 4, 0), repository=REPOSITORY)
        empty = CHANGELOG.replace("- The previous release.\n", "")
        with self.assertRaisesRegex(ValidationError, "Highlights"):
            release_notes(empty, SemVer(0, 3, 0), repository=REPOSITORY)

    def test_release_candidate_and_length_limit(self) -> None:
        candidate = CHANGELOG.replace("## [0.4.0] - 2026-12-01", "## [0.4.0-rc.1] - 2026-12-01")
        notes = release_notes(candidate, SemVer(0, 4, 0, rc=1), repository=REPOSITORY)
        self.assertIn("/blob/v0.4.0-rc.1/CHANGELOG.md#040-rc1---2026-12-01", notes)
        huge = CHANGELOG.replace("- The previous release.\n", "- " + "x" * MAX_NOTES_CHARACTERS + "\n")
        with self.assertRaisesRegex(ValidationError, "exceed"):
            release_notes(huge, SemVer(0, 3, 0), repository=REPOSITORY)

    def test_anchors_follow_github(self) -> None:
        self.assertEqual(github_anchor("[0.3.0] - 2026-11-17"), "030---2026-11-17")
        self.assertEqual(github_anchor("Migrating from 0.2.0"), "migrating-from-020")


if __name__ == "__main__":
    unittest.main()
