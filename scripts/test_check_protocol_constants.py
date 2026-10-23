#!/usr/bin/env python3
"""Tests for the protocol constant guard.

Each case copies the guarded part of the real tree into a temporary directory,
changes one file and asserts which way the guard reads the change.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_protocol_constants as check  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


class ProtocolConstantGuardTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        for directory in check.SCAN_DIRS:
            shutil.copytree(REPO / directory, self.root / directory)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def append(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.write_text(path.read_text(encoding="utf-8") + text, encoding="utf-8")

    def test_allowlist_matches_known_sites(self) -> None:
        self.assertEqual(check.find_violations(REPO), [])
        self.assertEqual(check.main(["check", str(REPO)]), 0)

    def test_fails_on_stray_latest_use(self) -> None:
        self.append(
            "src/server/server.cpp",
            "\nstd::string_view stray() { return mcp::g_LATEST_PROTOCOL_VERSION; }\n",
        )
        violations = check.find_violations(self.root)
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("src/server/server.cpp", violations[0])
        self.assertIn("g_LATEST_PROTOCOL_VERSION", violations[0])

    def test_fails_on_another_use_in_the_defining_header(self) -> None:
        self.append(
            "include/mcp/protocol/capabilities.hpp",
            "\nconstexpr bool g_stray = mcp::is_supported_protocol_version(\"x\");\n",
        )
        violations = check.find_violations(self.root)
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("is_supported_protocol_version", violations[0])

    def test_fails_on_32042(self) -> None:
        self.append("src/server/server.cpp", "\nconstexpr int g_stray_code = -32042;\n")
        violations = check.find_violations(self.root)
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("-32042", violations[0])

    def test_fails_on_32002_outside_the_connection_closed_constant(self) -> None:
        self.append("src/client/client.cpp", "\nconstexpr int g_stray_code = - 32002;\n")
        violations = check.find_violations(self.root)
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("src/client/client.cpp", violations[0])

    def test_comments_and_longer_numbers_are_ignored(self) -> None:
        self.append(
            "src/server/server.cpp",
            "\n// g_LATEST_PROTOCOL_VERSION and -32042 in a comment\n"
            "/* is_supported_protocol_version(-32002) */\n"
            "constexpr int g_other_code = -320420;\n",
        )
        self.assertEqual(check.find_violations(self.root), [])

    def test_removed_site_must_leave_the_allowlist(self) -> None:
        path = self.root / "include/mcp/protocol/base.hpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn("= -32002;", text)
        path.write_text(text.replace("= -32002;", "= -32099;"), encoding="utf-8")
        violations = check.find_violations(self.root)
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("shrink the allowlist", violations[0])


if __name__ == "__main__":
    unittest.main()
