#!/usr/bin/env python3
"""Mutation tests for the peer-input matrix staleness check and its ratchet.

The real repository is rendered once; every case then mutates the committed
text or manifest in memory and asserts which way the check reads the change.
"""

from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_json_matrix as check  # noqa: E402
import gen_json_matrix as gen  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAGS = ("absent", "null", "wrong_type", "oversized")


class JsonMatrixCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        core = os.path.join(REPO, "test", "core")
        with open(os.path.join(core, "json_peer_input_matrix_test.cpp"), encoding="utf-8") as fh:
            cls.committed = fh.read()
        with open(os.path.join(core, "json_matrix_manifest.json"), encoding="utf-8") as fh:
            cls.committed_manifest = json.load(fh)
        cls.rendered, cls.manifest = gen.render(REPO)
        cls.parsed = gen.parse_matrix(cls.rendered)

    def run_check(self, text: str, manifest: dict | None = None) -> tuple[int, str]:
        code, lines = check.check(
            text, self.manifest if manifest is None else manifest, self.rendered, self.manifest
        )
        return code, "\n".join(lines)

    def find_row(self, predicate) -> tuple[str, str]:
        for test, block in self.parsed["tests"].items():
            for key, row in block["rows"].items():
                if predicate(row):
                    return test, key
        self.fail("no rendered row matches")

    def block_span(self, text: str, test: str) -> tuple[int, int]:
        heads = list(gen._TEST_RE.finditer(text))
        for i, head in enumerate(heads):
            if head.group(1) == test:
                end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
                return head.start(), end
        self.fail(f"no TEST {test}")

    def edit_row(self, text: str, test: str, key: str, new_row) -> str:
        """Replaces (or with None, removes) one row of one TEST in `text`."""
        start, end = self.block_span(text, test)
        block = text[start:end]
        for m in gen._ROW_RE.finditer(block):
            if m.group(1) != key:
                continue
            if new_row is None:
                # The row and its trailing comma.
                block = block[: m.start()] + block[m.end() + 1 :]
            else:
                flags = ", ".join(str(new_row[f]).lower() for f in FLAGS)
                row = f'{{"{key}", R"json({m.group(2)})json", {m.group(3)}, {flags}}}'
                block = block[: m.start()] + row + block[m.end() :]
            return text[:start] + block + text[end:]
        self.fail(f"no row {test}.{key}")

    def flipped(self, test: str, key: str, **cols: bool) -> str:
        row = dict(self.parsed["tests"][test]["rows"][key], **cols)
        return self.edit_row(self.rendered, test, key, row)

    # 1
    def test_committed_matrix_matches_render(self) -> None:
        code, out = self.run_check(self.committed, self.committed_manifest)
        self.assertEqual(code, 0, out)
        self.assertIn("matches scripts/gen_json_matrix.py", out)

    # 2
    def test_line_wrapping_and_crlf_do_not_count(self) -> None:
        wrapped = self.rendered.replace('json::parse(R"json', 'json::parse(\n        R"json')
        wrapped = wrapped.replace("\n", "\r\n")
        self.assertNotEqual(wrapped, self.rendered)
        code, out = self.run_check(wrapped)
        self.assertEqual(code, 0, out)
        self.assertEqual(gen.parse_matrix(wrapped), self.parsed)

    # 3
    def test_toward_tolerance_is_stale(self) -> None:
        test, key = self.find_row(lambda r: not r["absent"] and not r["null"])
        code, out = self.run_check(self.flipped(test, key, null=True))
        self.assertEqual(code, 1, out)
        self.assertIn(f"TEST(JsonPeerInputMatrix, {test}): {key} null true->false", out)
        self.assertIn("decoder fix landed", out)
        self.assertNotIn("REGRESSION", out)

    # 4
    def test_optional_member_throwing_on_null_is_a_regression(self) -> None:
        test, key = self.find_row(lambda r: not r["absent"] and r["null"])
        committed = self.flipped(test, key, null=False)
        # A committed file written before the regression also counted one
        # null-fragile row fewer in its header; that alone is not harness drift.
        rows = [r for b in self.parsed["tests"].values() for r in b["rows"].values()]
        fragile = sum(not r["absent"] and r["null"] for r in rows)
        before = f"// {fragile} of the "
        self.assertIn(before, committed)
        committed = committed.replace(before, f"// {fragile - 1} of the ", 1)
        code, out = self.run_check(committed)
        self.assertEqual(code, 2, out)
        self.assertIn("REGRESSION", out)
        self.assertIn(f"{test}): {key} null false->true (optional member", out)
        self.assertIn("treat an explicit null as absent", out)
        self.assertNotIn("became required", out)
        self.assertNotIn("harness text", out)

    # 5
    def test_member_becoming_required_is_a_regression(self) -> None:
        test, key = self.find_row(lambda r: r["absent"])
        code, out = self.run_check(self.flipped(test, key, absent=False))
        self.assertEqual(code, 2, out)
        self.assertIn(f"{test}): {key} absent false->true (member is now required)", out)
        self.assertIn("became required", out)
        self.assertNotIn("treat an explicit null", out)

    # 6
    def test_stricter_type_or_domain_validation_is_stale(self) -> None:
        for col in ("wrong_type", "oversized"):
            test, key = self.find_row(lambda r, c=col: r[c])
            code, out = self.run_check(self.flipped(test, key, **{col: False}))
            self.assertEqual(code, 1, out)
            self.assertIn(f"{key} {col} false->true (stricter validation)", out)
            self.assertNotIn("REGRESSION", out)

    # 7
    def test_new_rows_are_judged_by_their_null_column(self) -> None:
        test, key = self.find_row(lambda r: not r["absent"] and r["null"])
        code, out = self.run_check(self.edit_row(self.rendered, test, key, None))
        self.assertEqual(code, 2, out)
        self.assertIn(f"{test}): field '{key}' added as null-fragile", out)

        test, key = self.find_row(lambda r: not r["null"])
        code, out = self.run_check(self.edit_row(self.rendered, test, key, None))
        self.assertEqual(code, 1, out)
        self.assertIn(f"{test}): field '{key}' added", out)

    # 8
    def test_missing_test_is_stale(self) -> None:
        # A TEST with no null-fragile row, so dropping it is only stale.
        test = next(
            t
            for t, block in self.parsed["tests"].items()
            if block["rows"] and all(r["absent"] or not r["null"] for r in block["rows"].values())
        )
        start, end = self.block_span(self.rendered, test)
        code, out = self.run_check(self.rendered[:start] + self.rendered[end:])
        self.assertEqual(code, 1, out)
        self.assertIn(f"TEST(JsonPeerInputMatrix, {test}): only in the generated file", out)

    # 9
    def test_harness_edit_is_stale(self) -> None:
        edited = self.rendered.replace('SCOPED_TRACE("null");', 'SCOPED_TRACE("nul");', 1)
        self.assertNotEqual(edited, self.rendered)
        code, out = self.run_check(edited)
        self.assertEqual(code, 1, out)
        self.assertIn("harness text", out)
        self.assertIn("Review the diff", out)
        self.assertNotIn("decoder fix landed", out)

        # Still named when a row difference is reported alongside it.
        test, key = self.find_row(lambda r: not r["absent"] and not r["null"])
        row = dict(self.parsed["tests"][test]["rows"][key], null=True)
        both = self.edit_row(edited, test, key, row)
        code, out = self.run_check(both)
        self.assertEqual(code, 1, out)
        self.assertIn(f"{key} null true->false", out)
        self.assertIn("harness text", out)

    # 10
    def test_manifest_difference_is_stale(self) -> None:
        manifest = dict(self.manifest, case_count=self.manifest["case_count"] + 4)
        code, out = self.run_check(self.rendered, manifest)
        self.assertEqual(code, 1, out)
        self.assertIn("manifest key 'case_count'", out)

    # 11
    def test_generator_refuses_unaccepted_regressions(self) -> None:
        test, key = self.find_row(lambda r: not r["absent"] and r["null"])
        committed = self.flipped(test, key, null=False)

        code, lines = gen.guard(committed, self.rendered, [])
        self.assertEqual(code, 2)
        self.assertTrue(any(f"{test}): {key} null false->true" in line for line in lines))

        for accepted in (f"{test}.{key}", test):
            code, lines = gen.guard(committed, self.rendered, [accepted])
            self.assertEqual((code, lines), (0, []), accepted)

        code, lines = gen.guard(self.rendered, self.rendered, ["NoSuchType.key"])
        self.assertEqual(code, 1)
        self.assertEqual(lines, ["--accept-regression NoSuchType.key matches no regressed row"])

        # A nested type may be named as written in C++, with :: for the _.
        test, key = next(
            (t, k)
            for t, block in self.parsed["tests"].items()
            if "_" in t
            for k, r in block["rows"].items()
            if not r["absent"] and r["null"]
        )
        committed = self.flipped(test, key, null=False)
        code, lines = gen.guard(committed, self.rendered, [test.replace("_", "::") + "." + key])
        self.assertEqual((code, lines), (0, []))

    # 13
    def test_required_member_made_optional_but_null_fragile_is_a_regression(self) -> None:
        test, key = self.find_row(lambda r: not r["absent"] and r["null"])
        committed = self.flipped(test, key, absent=True)
        code, out = self.run_check(committed)
        self.assertEqual(code, 2, out)
        self.assertIn(f"{test}): {key} became optional but throws on explicit null", out)
        self.assertNotIn("now tolerated", out)
        self.assertEqual(gen.guard(committed, self.rendered, [])[0], 2)

    # 14
    def test_guard_falls_back_to_the_committed_matrix(self) -> None:
        test, key = self.find_row(lambda r: not r["absent"] and r["null"])
        head = self.flipped(test, key, null=False)
        # Emptying or deleting the output file must not leave nothing to compare.
        for out_text in ("", None, "// truncated\n"):
            baseline = gen.guard_baseline(out_text, head)
            self.assertEqual(baseline, head)
            self.assertEqual(gen.guard(baseline, self.rendered, [])[0], 2)
        self.assertEqual(gen.guard_baseline(self.rendered, head), self.rendered)
        self.assertIsNone(gen.guard_baseline("", None))
        self.assertIsNone(gen.guard_baseline(None, ""))

    # 12
    def test_every_emitted_line_is_ascii(self) -> None:
        fragile = self.find_row(lambda r: not r["absent"] and r["null"])
        tolerant = self.find_row(lambda r: not r["absent"] and not r["null"])
        regressed = self.flipped(*fragile, null=False)
        tolerant_row = dict(self.parsed["tests"][tolerant[0]]["rows"][tolerant[1]], null=True)
        both = self.edit_row(regressed, *tolerant, tolerant_row)
        harness = self.rendered.replace('SCOPED_TRACE("absent");', 'SCOPED_TRACE("gone");', 1)
        manifest = dict(self.manifest, mode_count=5)
        emitted: list[str] = []
        for text, man in (
            (self.committed, self.committed_manifest),
            (both, manifest),
            (harness, self.manifest),
            ("no tests here", self.manifest),
        ):
            emitted += check.check(text, man, self.rendered, self.manifest)[1]
        emitted += gen.guard(regressed, self.rendered, ["NoSuchType"])[1]
        self.assertIn("REGRESSION", "\n".join(emitted))
        self.assertIn("is stale", "\n".join(emitted))
        for line in emitted:
            self.assertTrue(line.isascii(), line)

if __name__ == "__main__":
    unittest.main()
