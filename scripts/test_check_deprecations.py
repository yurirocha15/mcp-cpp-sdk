#!/usr/bin/env python3
"""Tests for the deprecation attribute guard.

Each case copies the headers into a temporary directory, changes one file and asserts which way
the guard reads the change.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_deprecations as check  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


class DeprecationGuardTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        shutil.copytree(REPO / "include", self.root / "include")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def add_header(self, text: str) -> list[str]:
        (self.root / "include" / "mcp" / "probe.hpp").write_text(text, encoding="utf-8")
        return check.find_violations(self.root)

    def test_tree_passes(self) -> None:
        self.assertEqual(check.find_violations(REPO), [])
        self.assertEqual(check.main(["check", str(REPO)]), 0)

    def test_tag_without_attribute_fails(self) -> None:
        violations = self.add_header("/** @deprecated Use g(). */\nvoid f();\n")
        self.assertEqual(len(violations), 1)
        self.assertIn("include/mcp/probe.hpp:1:", violations[0])

    def test_attribute_after_tag_passes(self) -> None:
        text = '/** @deprecated Use g(). */\nMCP_DEPRECATED("use g()")\nvoid f();\n'
        self.assertEqual(self.add_header(text), [])

    def test_standard_attribute_passes(self) -> None:
        text = '/** @deprecated Use g(). */\n[[deprecated("use g()")]] void f();\n'
        self.assertEqual(self.add_header(text), [])

    def test_doc_only_note_passes(self) -> None:
        text = "/**\n * @deprecated doc-only: removal no earlier than 2027-07-28.\n */\nvoid f();\n"
        self.assertEqual(self.add_header(text), [])

    def test_attribute_on_a_later_declaration_does_not_count(self) -> None:
        text = '/** @deprecated Use g(). */\nvoid f();\nMCP_DEPRECATED("x") void h();\n'
        self.assertEqual(len(self.add_header(text)), 1)

    def test_semicolon_inside_the_message_does_not_end_the_declaration(self) -> None:
        text = '/** @deprecated Use g(). */\nMCP_DEPRECATED("use g(); later")\nvoid f();\n'
        self.assertEqual(self.add_header(text), [])

    def test_line_comment_styles_fail(self) -> None:
        for text in (
            "/// @deprecated use B\nvoid a();\n",
            "//! @deprecated use D\nvoid c();\n",
            "/// Brief.\n/// @deprecated use B\nvoid a();\n",
        ):
            with self.subTest(text=text):
                self.assertEqual(len(self.add_header(text)), 1)

    def test_backslash_tag_fails(self) -> None:
        self.assertEqual(len(self.add_header("/** \\deprecated use C */\nvoid b();\n")), 1)

    def test_line_comment_with_attribute_passes(self) -> None:
        text = '/// @deprecated use B\nMCP_DEPRECATED("use B")\nvoid a();\n'
        self.assertEqual(self.add_header(text), [])

    def test_doc_only_must_be_on_the_tag_line(self) -> None:
        text = "/**\n * @deprecated use E\n * @note the doc-only mode is elsewhere\n */\nvoid d();\n"
        self.assertEqual(len(self.add_header(text)), 1)

    def test_doc_only_on_line_comment_passes(self) -> None:
        self.assertEqual(self.add_header("/// \\deprecated doc-only: later\nvoid a();\n"), [])

    def test_untagged_comment_passes(self) -> None:
        self.assertEqual(self.add_header("/** Plain. */\nvoid f();\n"), [])


if __name__ == "__main__":
    unittest.main()
