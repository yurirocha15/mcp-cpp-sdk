#!/usr/bin/env python3
"""Tests for the documentation-only change classifier, against real Git commits."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ci_changes  # noqa: E402

README = """# sdk

Prose.

```cpp
int main() { return 0; }
```
"""


class ClassifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)
        self._cwd = os.getcwd()
        os.chdir(self.root)
        self.git("init", "-q")
        self.git("config", "user.email", "ci@example.invalid")
        self.git("config", "user.name", "CI")
        self.git("config", "commit.gpgsign", "false")
        self.write("README.md", README)
        self.write("docs/index.rst", "Index\n")
        self.write("src/main.cpp", "int x;\n")
        self.base = self.commit()

    def tearDown(self) -> None:
        os.chdir(self._cwd)
        self._directory.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(
            ["git", *args], check=True, capture_output=True, text=True
        ).stdout.strip()

    def write(self, path: str, text: str) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def commit(self) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", "change")
        return self.git("rev-parse", "HEAD")

    def code(self, head: str, base: str | None = None) -> bool:
        return ci_changes.needs_code_jobs(self.base if base is None else base, head)

    def test_docs_and_markdown_skip_code_jobs(self) -> None:
        self.write("docs/index.rst", "Index, edited\n")
        self.write("examples/README.md", "Example notes\n")
        self.write("LICENSE", "License\n")
        self.write(".github/ISSUE_TEMPLATE/bug.yml", "name: bug\n")
        self.assertFalse(self.code(self.commit()))

    def test_readme_prose_skips_code_jobs(self) -> None:
        self.write("README.md", README.replace("Prose.", "Better prose."))
        self.assertFalse(self.code(self.commit()))

    def test_readme_cpp_block_needs_code_jobs(self) -> None:
        self.write("README.md", README.replace("return 0;", "return 1;"))
        self.assertTrue(self.code(self.commit()))

    def test_any_other_file_needs_code_jobs(self) -> None:
        self.write("docs/index.rst", "Index, edited\n")
        self.write("src/main.cpp", "int y;\n")
        self.assertTrue(self.code(self.commit()))

    def test_workflow_change_needs_code_jobs(self) -> None:
        self.write(".github/workflows/ci.yml", "name: CI\n")
        self.assertTrue(self.code(self.commit()))

    def test_doubt_needs_code_jobs(self) -> None:
        head = self.git("rev-parse", "HEAD")
        self.assertTrue(self.code(head, base=""))
        self.assertTrue(self.code(head, base=ci_changes.ZERO_SHA))
        self.assertTrue(self.code(head, base="f" * 40))
        self.assertTrue(self.code(self.commit()))  # empty diff

    def classify(self, event: str, head: str, base: str | None = None) -> list[str]:
        return subprocess.run(
            [sys.executable, ci_changes.__file__, "--event", event,
             "--base", self.base if base is None else base, "--head", head],
            check=True, capture_output=True, text=True,
        ).stdout.split()

    def test_manual_runs_always_need_every_job(self) -> None:
        self.write("docs/index.rst", "Index, edited\n")
        head = self.commit()
        self.assertEqual(
            self.classify("workflow_dispatch", head), ["code=true", "packaging=true"]
        )
        self.assertEqual(
            self.classify("pull_request", head), ["code=false", "packaging=false"]
        )

    def test_pull_request_source_change_skips_packaging(self) -> None:
        self.write("src/main.cpp", "int y;\n")
        head = self.commit()
        self.assertEqual(self.classify("pull_request", head), ["code=true", "packaging=false"])
        self.assertEqual(self.classify("push", head), ["code=true", "packaging=true"])

    def test_pull_request_packaging_change_runs_packaging(self) -> None:
        for path in ("CMakeLists.txt", "cmake/config.cmake.in", "packaging/targets.json",
                     "release/model.py", "scripts/release/build_windows.ps1",
                     "scripts/package_build.py", ".github/workflows/ci.yml",
                     ".github/workflows/release-candidate.yml", "conanfile.py", "VERSION"):
            with self.subTest(path=path):
                self.write(path, f"{path}\n")
                head = self.commit()
                self.assertEqual(
                    self.classify("pull_request", head), ["code=true", "packaging=true"]
                )
                self.base = head

    def test_documentation_push_skips_packaging(self) -> None:
        self.write("docs/index.rst", "Index, edited\n")
        self.assertEqual(self.classify("push", self.commit()), ["code=false", "packaging=false"])

    def test_doubt_runs_packaging(self) -> None:
        self.write("src/main.cpp", "int y;\n")
        head = self.commit()
        self.assertEqual(
            self.classify("pull_request", head, base=""), ["code=true", "packaging=true"]
        )
        self.assertEqual(
            self.classify("pull_request", head, base="f" * 40), ["code=true", "packaging=true"]
        )


if __name__ == "__main__":
    unittest.main()
