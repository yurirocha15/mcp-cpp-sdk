#!/usr/bin/env python3
"""Tests for scripts/check_example_progression.py, on in-memory programs."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_example_progression as progression  # noqa: E402

BASE = """// The base program.
int main() {
    start();
    serve();
}
"""

ADDS_A_LINE = """// The base program plus a feature.
int main() {
    start();
      feature();
    serve();
}
"""

REPLACES_SERVE = """// The base program, served elsewhere.
int main() {
    start();
    serve_elsewhere();
}
"""


def run_check(files, add_ons, combined=None):
    family = {"add_ons": add_ons}
    if combined:
        family["all"] = combined
    return progression.check({"base.cpp": family}, lambda name: files[name])


class ProgressionTest(unittest.TestCase):
    def test_an_add_on_that_only_adds_lines_passes(self):
        problems = run_check({"base.cpp": BASE, "a.cpp": ADDS_A_LINE}, {"a.cpp": []})
        self.assertEqual(problems, [])

    def test_comments_blank_lines_and_indentation_are_not_compared(self):
        reindented = BASE.replace("    start();", "\n// a note\n        start();")
        problems = run_check({"base.cpp": BASE, "a.cpp": reindented}, {"a.cpp": []})
        self.assertEqual(problems, [])

    def test_a_replaced_line_must_be_listed(self):
        problems = run_check({"base.cpp": BASE, "r.cpp": REPLACES_SERVE}, {"r.cpp": []})
        self.assertEqual(len(problems), 1)
        self.assertIn("serve();", problems[0])
        self.assertEqual(
            run_check({"base.cpp": BASE, "r.cpp": REPLACES_SERVE}, {"r.cpp": ["serve();"]}), [])

    def test_a_listed_line_the_add_on_keeps_is_reported(self):
        problems = run_check({"base.cpp": BASE, "a.cpp": ADDS_A_LINE}, {"a.cpp": ["serve();"]})
        self.assertEqual(len(problems), 1)
        self.assertIn("keeps it", problems[0])

    def test_lines_moved_out_of_order_count_as_changed(self):
        moved = "int main() {\n    serve();\n    start();\n}\n"
        problems = run_check({"base.cpp": BASE, "m.cpp": moved}, {"m.cpp": []})
        self.assertTrue(problems)

    def test_the_combined_program_needs_every_add_on_line_and_kept_base_line(self):
        both = "int main() {\n    start();\n    feature();\n    serve_elsewhere();\n}\n"
        files = {"base.cpp": BASE, "a.cpp": ADDS_A_LINE, "r.cpp": REPLACES_SERVE, "all.cpp": both}
        add_ons = {"a.cpp": [], "r.cpp": ["serve();"]}
        combined = {"file": "all.cpp", "combines": ["a.cpp", "r.cpp"]}
        self.assertEqual(run_check(files, add_ons, combined), [])

        files["all.cpp"] = "int main() {\n    start();\n    serve_elsewhere();\n}\n"
        problems = run_check(files, add_ons, combined)
        self.assertEqual(len(problems), 1)
        self.assertIn("feature();", problems[0])

        files["all.cpp"] = both.replace("    start();\n", "")
        problems = run_check(files, add_ons, combined)
        self.assertEqual(len(problems), 1)
        self.assertIn("start();", problems[0])

    def test_the_combined_program_names_only_add_ons_of_its_base(self):
        files = {"base.cpp": BASE, "a.cpp": ADDS_A_LINE, "all.cpp": ADDS_A_LINE}
        problems = run_check(files, {"a.cpp": []}, {"file": "all.cpp", "combines": ["x.cpp"]})
        self.assertEqual(len(problems), 1)
        self.assertIn("x.cpp", problems[0])

    def test_a_program_the_manifest_does_not_name_is_reported(self):
        manifest = {"base.cpp": {"add_ons": {"a.cpp": []}, "all": {"file": "all.cpp",
                                                                    "combines": ["a.cpp"]}}}
        self.assertEqual(
            progression.unlisted_problems(manifest, ["base.cpp", "a.cpp", "all.cpp"]), [])
        problems = progression.unlisted_problems(manifest, ["base.cpp", "a.cpp", "b.cpp"])
        self.assertEqual(len(problems), 1)
        self.assertIn("b.cpp", problems[0])

    def test_the_readme_must_show_the_base_programs_as_they_are(self):
        files = {"server_base.cpp": BASE, "client_base.cpp": REPLACES_SERVE}
        readme = f"# Title\n```cpp\n{BASE}```\ntext\n```cpp\n{REPLACES_SERVE}```\n"
        self.assertEqual(progression.readme_problems(readme, lambda name: files[name]), [])
        stale = readme.replace("serve_elsewhere();", "serve();")
        problems = progression.readme_problems(stale, lambda name: files[name])
        self.assertEqual(len(problems), 1)
        self.assertIn("client_base.cpp", problems[0])

    def test_the_opening_comment_is_left_out_of_diffs(self):
        lines = progression.without_opening_comment(BASE)
        self.assertEqual(lines[0], "int main() {\n")


if __name__ == "__main__":
    unittest.main()
