from __future__ import annotations

import unittest

from release.abi_baseline import (
    AbiPolicyError,
    BaselineSelection,
    select_baseline,
)


def release(
    tag: str,
    database_id: int,
    *,
    draft: bool = False,
    immutable: bool = True,
    prerelease: bool | None = None,
) -> dict[str, object]:
    if prerelease is None:
        prerelease = "-rc." in tag
    return {
        "databaseId": database_id,
        "isDraft": draft,
        "isImmutable": immutable,
        "isPrerelease": prerelease,
        "tagName": tag,
    }


class BaselineSelectionTests(unittest.TestCase):
    def test_zero_major_selects_latest_stable_in_same_minor_series(self) -> None:
        catalog = [
            release("v0.1.99", 1),
            release("v0.2.0", 2),
            release("v0.2.1-rc.1", 3),
            release("v0.3.0", 4),
            release("v1.0.0", 5),
        ]
        selection = select_baseline("v0.2.1", catalog)
        self.assertEqual(selection.current.comparison_series, "0.2")
        self.assertEqual(selection.current.loader_identity, "0.2.1")
        self.assertEqual(selection.baseline.version.tag, "v0.2.0")

    def test_nonzero_major_uses_major_as_comparison_series(self) -> None:
        catalog = [
            release("v1.4.9", 1),
            release("v1.5.0-rc.1", 2),
            release("v1.4.10", 3),
            release("v0.9.99", 4),
            release("v2.0.0", 5),
        ]
        selection = select_baseline("v1.5.0", catalog)
        self.assertEqual(selection.current.comparison_series, "1")
        self.assertEqual(selection.current.loader_identity, "1")
        self.assertEqual(selection.baseline.version.tag, "v1.4.10")

    def test_new_zero_minor_comparison_series_is_explicit(self) -> None:
        selection = select_baseline(
            "v0.3.0",
            [[release("v0.2.9", 1)], [release("v1.0.0", 2)]],
        )
        self.assertFalse(selection.has_baseline)
        self.assertEqual(
            selection.to_mapping(),
            {
                "schema_version": 1,
                "status": "new-abi-line",
                "current_tag": "v0.3.0",
                "abi_line": "0.3",
                "baseline_tag": None,
                "baseline_release_id": None,
            },
        )

    def test_higher_version_does_not_replace_lower_backport_baseline(self) -> None:
        selection = select_baseline(
            "v1.2.4",
            [release("v1.2.3", 1), release("v1.3.0", 2)],
        )
        self.assertEqual(selection.baseline.version.tag, "v1.2.3")

    def test_draft_is_not_a_baseline(self) -> None:
        selection = select_baseline(
            "v1.2.2",
            [release("v1.2.1", 1, draft=True, immutable=False), release("v1.2.0", 2)],
        )
        self.assertEqual(selection.baseline.version.tag, "v1.2.0")

    def test_published_mutable_stable_on_line_fails_closed(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "mutable"):
            select_baseline("v1.2.2", [release("v1.2.1", 1, immutable=False)])

    def test_rc_candidate_is_rejected(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "stable releases"):
            select_baseline("v0.2.0-rc.1", [])

    def test_existing_candidate_release_is_rejected(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "already has"):
            select_baseline("v0.2.0", [release("v0.2.0", 1)])

    def test_duplicate_ids_or_tags_are_rejected_across_pages(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "duplicate"):
            select_baseline("v1.2.2", [[release("v1.2.0", 1)], [release("v1.2.1", 1)]])
        with self.assertRaisesRegex(AbiPolicyError, "duplicate"):
            select_baseline("v1.2.2", [release("v1.2.0", 1), release("v1.2.0", 2)])

    def test_catalog_requires_exact_typed_metadata(self) -> None:
        malformed = release("v1.2.0", 1)
        malformed["unexpected"] = True
        with self.assertRaisesRegex(AbiPolicyError, "fields"):
            select_baseline("v1.2.1", [malformed])
        malformed = release("v1.2.0", 1)
        malformed["databaseId"] = True
        with self.assertRaisesRegex(AbiPolicyError, "databaseId"):
            select_baseline("v1.2.1", [malformed])
        malformed = release("v1.2.0-rc.1", 1, prerelease=False)
        with self.assertRaisesRegex(AbiPolicyError, "disagrees"):
            select_baseline("v1.2.1", [malformed])

    def test_selection_round_trip_and_mutations(self) -> None:
        expected = select_baseline("v1.2.2", [release("v1.2.1", 7)])
        value = expected.to_mapping()
        self.assertEqual(BaselineSelection.from_mapping(value), expected)
        for field, replacement in (
            ("schema_version", 2),
            ("abi_line", "2"),
            ("status", "unknown"),
            ("baseline_tag", "v2.3.0"),
            ("baseline_release_id", True),
        ):
            mutated = dict(value)
            mutated[field] = replacement
            with self.subTest(field=field), self.assertRaises(AbiPolicyError):
                BaselineSelection.from_mapping(mutated)

if __name__ == "__main__":
    unittest.main()
