#!/usr/bin/env python3
"""Mutation tests for the feature matrix check.

The committed matrix is loaded once; every case then changes it, or a small
repository built in a temporary directory, and asserts what the check says.
"""

from __future__ import annotations

import copy
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_feature_matrix as check  # noqa: E402
import gen_feature_matrix as gen  # noqa: E402


def _committed_page() -> str:
    with open(os.path.join(gen.REPO, gen.PAGE), encoding="utf-8") as handle:
        return handle.read()


def _row(manifest: dict, row_id: int) -> dict:
    return manifest["features"][row_id - 1]


class CommittedMatrixTest(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = gen.load_manifest()

    def problems(self, manifest: dict) -> list[str]:
        """The problems of a changed manifest; the committed page is not what it renders."""
        return check.check(manifest, _committed_page())

    def test_committed_matrix_is_current(self) -> None:
        self.assertEqual(check.check(self.manifest, _committed_page()), [])

    def test_has_the_official_rows_in_order(self) -> None:
        rows = self.manifest["features"]
        self.assertEqual([r["id"] for r in rows], list(range(1, 49)))
        self.assertEqual([r["name"] for r in rows], list(gen.OFFICIAL_FEATURES))
        self.assertTrue(all(r["status"] == "implemented" for r in rows))

    def test_a_missing_row_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        del manifest["features"][10]
        self.assertTrue(any("47 rows" in p for p in self.problems(manifest)))

    def test_rows_out_of_order_fail(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        features = manifest["features"]
        features[0], features[1] = features[1], features[0]
        found = self.problems(manifest)
        self.assertTrue(any("id is 2, expected 1" in p for p in found))
        self.assertTrue(any("name should be" in p for p in found))

    def test_a_renamed_feature_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 5)["name"] = "Tools - sound results"
        self.assertTrue(any("name should be 'Tools - audio results'" in p for p in self.problems(manifest)))

    def test_an_unknown_status_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 3)["status"] = "partial"
        self.assertTrue(any("status 'partial'" in p for p in self.problems(manifest)))

    def test_a_missing_key_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        del _row(manifest, 3)["guide"]
        self.assertTrue(any("keys are" in p for p in self.problems(manifest)))

    def test_an_unknown_symbol_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 2)["api"] = ["call_tool", "call_tool_that_does_not_exist"]
        self.assertTrue(
            any("'call_tool_that_does_not_exist' is in no header" in p for p in self.problems(manifest))
        )

    def test_a_row_without_symbols_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 2)["api"] = []
        self.assertTrue(any("no API symbol" in p for p in self.problems(manifest)))

    def test_a_missing_marker_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 11)["example"]["marker"] = "docs-begin: no-such-marker"
        self.assertTrue(any("no-such-marker" in p for p in self.problems(manifest)))

    def test_a_missing_example_file_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 11)["example"]["file"] = "examples/features/no_such_example.cpp"
        found = self.problems(manifest)
        self.assertTrue(any("does not exist" in p for p in found))
        self.assertTrue(any("not registered" in p for p in found))

    def test_an_unregistered_example_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 11)["example"]["file"] = "examples/features/README.md"
        _row(manifest, 11)["example"]["marker"] = "Quick Start"
        self.assertTrue(any("not registered" in p for p in self.problems(manifest)))

    def test_a_missing_guide_page_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 1)["guide"] = "concepts/no-such-page#using-tools"
        self.assertTrue(any("names no page or section" in p for p in self.problems(manifest)))

    def test_a_missing_guide_anchor_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 1)["guide"] = "concepts/tools#no-such-section"
        self.assertTrue(any("names no page or section" in p for p in self.problems(manifest)))

    def test_a_guide_without_an_anchor_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        _row(manifest, 1)["guide"] = "concepts/tools"
        self.assertTrue(any("names no page or section" in p for p in self.problems(manifest)))

    def test_a_stale_page_fails(self) -> None:
        stale = _committed_page().replace("Tools - listing", "Tools - listing (old)")
        found = check.check(self.manifest, stale)
        self.assertEqual(len(found), 1)
        self.assertIn("is stale", found[0])

    def test_a_missing_page_fails(self) -> None:
        found = check.check(self.manifest, None)
        self.assertEqual(len(found), 1)
        self.assertIn("does not exist", found[0])

    def test_a_row_that_is_not_implemented_changes_the_count(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        row = _row(manifest, 40)
        row["status"] = "not-implemented"
        row["reason"] = "Not provided."
        row["example"] = None
        found = self.problems(manifest)
        self.assertTrue(any("47 implemented rows, expected 48" in p for p in found))
        self.assertTrue(any("does not state the reason" in p for p in found))

    def test_the_page_lists_every_row(self) -> None:
        page = gen.render(self.manifest)
        for name in gen.OFFICIAL_FEATURES:
            self.assertIn(f"     - {name}\n", page)
        self.assertIn("48 features are implemented.", page)


class RowTest(unittest.TestCase):
    """One row against a small repository in a temporary directory."""

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = self.dir.name
        self.write("include/mcp/thing.hpp", "class Widget { void frobnicate(); };\n")
        self.write("examples/features/demo.cpp", "// docs-begin: demo\nint main() {}\n")
        self.write("CMakeLists.txt", "add_executable(demo examples/features/demo.cpp)\n")
        self.write(
            "docs/guide.rst",
            "Guide\n=====\n\nUsing Widgets\n-------------\n\nText.\n\n"
            "Missing Parts\n-------------\n\nThe widget cannot frobnicate.\n\n"
            "Next\n----\n\nOther.\n",
        )
        self.base = {
            "id": 1,
            "name": gen.OFFICIAL_FEATURES[0],
            "method": None,
            "status": "implemented",
            "api": ["frobnicate"],
            "guide": "guide#using-widgets",
            "example": {"file": "examples/features/demo.cpp", "marker": "docs-begin: demo"},
            "reason": None,
        }

    def write(self, relative: str, text: str) -> None:
        path = os.path.join(self.root, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)

    def problems(self, **changes) -> list[str]:
        row = {**self.base, **changes}
        headers = check._header_text(self.root)
        cmake = check._read(self.root, "CMakeLists.txt") or ""
        return check._check_row(row, 1, self.root, headers, cmake)

    def test_a_complete_row_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_a_symbol_matches_whole_words_only(self) -> None:
        self.assertTrue(self.problems(api=["frob"]))

    def test_an_implemented_row_without_an_example_fails(self) -> None:
        self.assertTrue(any("no example" in p for p in self.problems(example=None)))

    def test_an_implemented_row_with_a_reason_fails(self) -> None:
        self.assertTrue(any("has a reason" in p for p in self.problems(reason="Because.")))

    def test_an_unregistered_example_fails(self) -> None:
        self.write("CMakeLists.txt", "add_executable(other examples/features/other.cpp)\n")
        self.assertTrue(any("not registered" in p for p in self.problems()))

    def test_an_example_registered_by_a_loop_passes(self) -> None:
        self.write("examples/sync/demo.cpp", "// docs-begin: demo\n")
        self.write(
            "CMakeLists.txt",
            "set(NAMES demo other)\nforeach(example IN LISTS NAMES)\n"
            "  add_executable(x examples/sync/${example}.cpp)\nendforeach()\n",
        )
        example = {"file": "examples/sync/demo.cpp", "marker": "docs-begin: demo"}
        self.assertEqual(self.problems(example=example), [])

    def test_a_not_implemented_row_states_its_reason(self) -> None:
        row = {
            "status": "not-implemented",
            "guide": "guide#missing-parts",
            "reason": "The widget cannot frobnicate.",
            "example": None,
        }
        self.assertEqual(self.problems(**row), [])

    def test_a_not_implemented_row_needs_a_reason(self) -> None:
        row = {"status": "not-implemented", "guide": "guide#missing-parts", "reason": "", "example": None}
        self.assertTrue(any("needs a reason" in p for p in self.problems(**row)))

    def test_the_reason_must_be_in_the_guide_section(self) -> None:
        row = {
            "status": "not-implemented",
            "guide": "guide#using-widgets",
            "reason": "The widget cannot frobnicate.",
            "example": None,
        }
        self.assertTrue(any("does not state the reason" in p for p in self.problems(**row)))


class SectionTest(unittest.TestCase):
    def test_ids_follow_docutils(self) -> None:
        self.assertEqual(gen.make_id("Using Tools"), "using-tools")
        self.assertEqual(gen.make_id("HTTP+SSE Transport (Deprecated)"), "http-sse-transport-deprecated")
        self.assertEqual(gen.make_id("``StreamableHttpSessionManager``"), "streamablehttpsessionmanager")
        self.assertEqual(gen.make_id("1. Numbered"), "numbered")

    def test_repeated_titles_get_a_suffix(self) -> None:
        text = "Page\n====\n\nLogging\n-------\n\nA.\n\nLogging\n-------\n\nB.\n"
        self.assertEqual([s["id"] for s in gen.sections(text)], ["page", "logging", "logging-1"])

    def test_a_section_ends_at_a_section_of_the_same_level(self) -> None:
        text = (
            "Page\n====\n\nOne\n---\n\nbody one\n\nSub\n~~~\n\nsub body\n\nTwo\n---\n\nbody two\n"
        )
        found = {s["id"]: s for s in gen.sections(text)}
        self.assertIn("sub body", found["one"]["body"])
        self.assertNotIn("body two", found["one"]["body"])
        self.assertNotIn("body one", found["sub"]["body"])

    def test_a_short_underline_is_not_a_heading(self) -> None:
        self.assertEqual(gen.sections("Title\n--\n\ntext\n"), [])


if __name__ == "__main__":
    unittest.main()
