#!/usr/bin/env python3
"""Tests for scripts/check_doc_snippets.py.

Extraction is checked on in-memory rst text. The end-to-end cases write a tiny
docs tree and a synthetic compile_commands.json that names the host C++ compiler,
so they need no SDK build tree.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_doc_snippets as check  # noqa: E402

VALID = "int main() { return 0; }"
BROKEN = "int main() { return undeclared_name; }"


def _extract(text: str):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "page.rst"
        path.write_text(text, encoding="utf-8")
        return check.extract_snippets(path)


def _block(code: str, options: str = "", indent: str = "") -> str:
    body = "\n".join(f"{indent}   {line}" if line else "" for line in code.split("\n"))
    opts = "".join(f"{indent}   {opt}\n" for opt in options.split("|") if opt)
    return f"{indent}.. code-block:: cpp\n{opts}\n{body}\n"


class ExtractionTest(unittest.TestCase):
    def test_body_is_dedented_and_ends_at_less_indented_text(self):
        text = "Title\n=====\n\n" + _block("int a;\n\nint main() {\n    return 0;\n}") + "\nAfter.\n"
        self.assertEqual(_extract(text), [(4, "int a;\n\nint main() {\n    return 0;\n}\n")])

    def test_options_are_not_part_of_the_body(self):
        text = _block(VALID, ":caption: A caption|:linenos:")
        self.assertEqual(_extract(text), [(1, VALID + "\n")])

    def test_nested_directive_keeps_relative_indentation(self):
        text = "- item\n\n" + _block("int x;\n  int y;", indent="  ") + "- next\n"
        self.assertEqual(_extract(text), [(3, "int x;\n  int y;\n")])

    def test_other_languages_and_literalinclude_are_ignored(self):
        text = (".. code-block:: bash\n\n   int main(\n\n"
                ".. literalinclude:: x.cpp\n   :language: cpp\n")
        self.assertEqual(_extract(text), [])

    def test_two_blocks_report_their_own_lines(self):
        text = _block("int a;") + "\nText.\n\n" + _block(VALID)
        self.assertEqual([line for line, _ in _extract(text)], [1, 7])


class EndToEndTest(unittest.TestCase):
    def setUp(self):
        compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.fail("no host C++ compiler on PATH")
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.build = self.tmp / "build"
        self.build.mkdir()
        (self.build / "compile_commands.json").write_text(json.dumps([{
            "directory": str(self.build),
            "file": "/src/examples/demo.cpp",
            "arguments": [compiler, "-std=c++20", "-c", "/src/examples/demo.cpp"],
        }]), encoding="utf-8")
        self.docs = self.tmp / "docs"
        self.docs.mkdir()

    def _run(self, *pages: str):
        for number, text in enumerate(pages):
            (self.docs / f"page{number}.rst").write_text(text, encoding="utf-8")
        argv = ["check_doc_snippets.py", "--build-dir", str(self.build),
                "--docs-dir", str(self.docs)]
        out = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(out):
            code = check.main()
        return code, out.getvalue()

    def test_rst_main_block_compiled(self):
        code, output = self._run(_block(VALID) + "\n" + _block("int fragment;"))
        self.assertEqual(code, 0, output)
        self.assertIn("1 compiled, 1 skipped, 0 failed", output)

    def test_broken_snippet_fails(self):
        code, output = self._run(_block(BROKEN))
        self.assertEqual(code, 1, output)
        self.assertIn("0 compiled, 0 skipped, 1 failed", output)

    def test_only_fragments_proves_nothing(self):
        code, output = self._run(_block("int fragment;"))
        self.assertEqual(code, 1, output)


if __name__ == "__main__":
    unittest.main()
