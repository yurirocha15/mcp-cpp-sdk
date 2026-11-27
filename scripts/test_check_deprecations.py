#!/usr/bin/env python3
"""Tests for the deprecation registry check.

Each case copies the headers, the tests and the registry into a temporary directory, changes one
of them and asserts which way the check reads the change.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_deprecations as check  # noqa: E402

REPO = Path(__file__).resolve().parent.parent

SPEC_MESSAGE = "SEP-9999: Probing is deprecated since protocol revision 2030-01-01; earliest removal: never"


class DeprecationRegistryCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        shutil.copytree(REPO / "include", self.root / "include")
        shutil.copytree(
            REPO / "test",
            self.root / "test",
            ignore=lambda _, names: [name for name in names if name.endswith((".json", ".txt"))],
        )
        (self.root / "scripts").mkdir()
        shutil.copy(REPO / check.REGISTRY, self.root / check.REGISTRY)
        self.registry = json.loads((self.root / check.REGISTRY).read_text(encoding="utf-8"))
        self.registry["stems"].append("probing")
        self.probe_row = {
            "kind": "spec",
            "id": "probing",
            "proposal": "SEP-9999",
            "since": "2030-01-01",
            "earliest_removal": "never",
            "declarations": [],
            "runtime_only": [],
            "exemptions": [],
            "notice_test": None,
        }
        self.registry["rows"].append(self.probe_row)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_check(self, header: str | None = None) -> list[str]:
        if header is not None:
            (self.root / "include" / "mcp" / "probe.hpp").write_text(header, encoding="utf-8")
        (self.root / check.REGISTRY).write_text(json.dumps(self.registry), encoding="utf-8")
        return check.find_violations(self.root)

    def test_tree_passes(self) -> None:
        self.assertEqual(check.find_violations(REPO), [])
        self.assertEqual(check.main(["check", str(REPO)]), 0)

    def test_listed_function_with_its_message_passes(self) -> None:
        self.probe_row["declarations"] = ["include/mcp/probe.hpp:start_probing"]
        header = f'/** @deprecated Probing. */\nMCP_DEPRECATED("{SPEC_MESSAGE}")\n[[nodiscard]] MCP_API int start_probing(int count);\n'
        self.assertEqual(self.run_check(header), [])

    def test_listed_enumerator_and_variable_pass(self) -> None:
        self.probe_row["declarations"] = [
            "include/mcp/probe.hpp:eProbing",
            "include/mcp/probe.hpp:g_PROBING",
        ]
        header = (
            f'enum class Mode {{ eOff, eProbing MCP_DEPRECATED("{SPEC_MESSAGE}"), eOn }};\n'
            f'MCP_DEPRECATED("{SPEC_MESSAGE}")\ninline constexpr int g_PROBING = 1;\n'
        )
        self.assertEqual(self.run_check(header), [])

    def test_message_split_over_literals_is_joined(self) -> None:
        self.probe_row["declarations"] = ["include/mcp/probe.hpp:start_probing"]
        header = 'MCP_DEPRECATED("SEP-9999: Probing is deprecated; "\n               "earliest removal: never")\nvoid start_probing();\n'
        self.assertEqual(self.run_check(header), [])

    def test_listed_declaration_without_marker_fails(self) -> None:
        self.probe_row["declarations"] = ["include/mcp/probe.hpp:start_probing"]
        violations = self.run_check("void start_probing();\n")
        self.assertEqual(len(violations), 1)
        self.assertIn("start_probing (probing) has no MCP_DEPRECATED marker", violations[0])

    def test_listed_declaration_missing_from_its_header_fails(self) -> None:
        self.probe_row["declarations"] = ["include/mcp/probe.hpp:start_probing"]
        violations = self.run_check("void other();\n")
        self.assertEqual(len(violations), 1)
        self.assertIn("is not declared there", violations[0])

    def test_message_without_the_proposal_or_removal_fails(self) -> None:
        self.probe_row["declarations"] = ["include/mcp/probe.hpp:start_probing"]
        violations = self.run_check('MCP_DEPRECATED("Probing is deprecated")\nvoid start_probing();\n')
        self.assertEqual(len(violations), 1)
        self.assertIn("'SEP-9999'", violations[0])
        self.assertIn("'earliest removal'", violations[0])

    def test_marker_outside_every_row_fails(self) -> None:
        violations = self.run_check(f'MCP_DEPRECATED("{SPEC_MESSAGE}")\nvoid quiet();\n')
        self.assertEqual(len(violations), 1)
        self.assertIn("MCP_DEPRECATED on quiet belongs to no row", violations[0])

    def test_standard_attribute_is_not_a_marker(self) -> None:
        self.probe_row["declarations"] = ["include/mcp/probe.hpp:start_probing"]
        violations = self.run_check(f'[[deprecated("{SPEC_MESSAGE}")]] void start_probing();\n')
        self.assertEqual(len(violations), 1)
        self.assertIn("has no MCP_DEPRECATED marker", violations[0])

    def test_unclassified_identifier_with_a_stem_fails(self) -> None:
        violations = self.run_check("struct ProbingOptions { int depth = 0; };\n")
        self.assertEqual(len(violations), 1)
        self.assertIn("ProbingOptions names a deprecated feature", violations[0])

    def test_stem_inside_comments_and_strings_is_ignored(self) -> None:
        header = '// probing\n/** Probing. */\ninline const char* g_NAME = "probing";\n'
        self.assertEqual(self.run_check(header), [])

    def test_exempted_identifier_passes(self) -> None:
        self.probe_row["exemptions"] = [{"pattern": "ProbingOptions", "reason": "a type"}]
        self.assertEqual(self.run_check("struct ProbingOptions { int depth = 0; };\n"), [])

    def test_exemption_matches_the_whole_identifier(self) -> None:
        self.probe_row["exemptions"] = [{"pattern": "Probing", "reason": "a type"}]
        self.assertEqual(len(self.run_check("struct ProbingOptions { int depth = 0; };\n")), 1)

    def test_tag_without_marker_fails_and_doc_only_is_gone(self) -> None:
        for header in (
            "/** @deprecated Use g(). */\nvoid f();\n",
            "/**\n * @deprecated doc-only: removal no earlier than 2027-07-28.\n */\nvoid f();\n",
            "/// \\deprecated use B\nvoid a();\n",
            "//! @deprecated use D\nvoid c();\n",
        ):
            with self.subTest(header=header):
                violations = self.run_check(header)
                self.assertEqual(len(violations), 1)
                self.assertIn("@deprecated needs MCP_DEPRECATED", violations[0])

    def test_tag_on_a_runtime_only_declaration_passes(self) -> None:
        self.probe_row["runtime_only"] = [
            {"declaration": "include/mcp/probe.hpp:probing_depth", "reason": "a member"}
        ]
        header = "struct Options {\n    /** @deprecated Probing. */\n    int probing_depth = 0;\n};\n"
        self.assertEqual(self.run_check(header), [])

    def test_runtime_only_declaration_missing_from_its_header_fails(self) -> None:
        self.probe_row["runtime_only"] = [
            {"declaration": "include/mcp/probe.hpp:probing_depth", "reason": "a member"}
        ]
        violations = self.run_check("struct Options {};\n")
        self.assertEqual(len(violations), 1)
        self.assertIn("probing_depth (probing) is not declared there", violations[0])

    def test_marker_on_a_later_declaration_does_not_count_for_the_tag(self) -> None:
        self.probe_row["declarations"] = ["include/mcp/probe.hpp:start_probing"]
        header = f'/** @deprecated Use g(). */\nvoid f();\nMCP_DEPRECATED("{SPEC_MESSAGE}") void start_probing();\n'
        violations = self.run_check(header)
        self.assertEqual(len(violations), 1)
        self.assertIn("@deprecated needs MCP_DEPRECATED", violations[0])

    def test_missing_notice_test_fails(self) -> None:
        self.probe_row["notice_test"] = "ProbingTest.DoesNotExist"
        violations = self.run_check()
        self.assertEqual(len(violations), 1)
        self.assertIn("notice_test ProbingTest.DoesNotExist of probing does not exist", violations[0])

    def test_sdk_row_needs_replacement_and_release(self) -> None:
        self.registry["rows"].append(
            {
                "kind": "sdk",
                "id": "old-factory",
                "replacement": "make_new()",
                "since_release": "0.5.0",
                "earliest_removal": "1.0.0",
                "declarations": ["include/mcp/probe.hpp:make_old", "include/mcp/probe.hpp:make_older"],
                "exemptions": [],
            }
        )
        header = (
            'MCP_DEPRECATED("use make_new(); removal no earlier than 1.0.0") void make_old();\n'
            'MCP_DEPRECATED("use make_new()") void make_older();\n'
        )
        violations = self.run_check(header)
        self.assertEqual(len(violations), 1)
        self.assertIn("make_older lacks 'removal no earlier than 1.0.0'", violations[0])


class DeclaredNameTest(unittest.TestCase):
    def test_names(self) -> None:
        cases = {
            "[[nodiscard]] MCP_API LoggingLevel get_log_level() const": "get_log_level",
            "MCP_API Task<void> log(LoggingLevel level, std::string_view msg": "log",
            "void on_sampling(\n std::function<Task<R>(const P&)> handler)": "on_sampling",
            "inline constexpr std::string_view g_THIS_SERVER = \"x\"": "g_THIS_SERVER",
            "std::optional<LoggingLevel> log_level": "log_level",
            "int values[4]": "values",
        }
        for head, name in cases.items():
            with self.subTest(head=head):
                self.assertEqual(check.declared_name(head), name)


if __name__ == "__main__":
    unittest.main()
