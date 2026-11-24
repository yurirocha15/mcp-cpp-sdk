#!/usr/bin/env python3
"""Tests for the job classifier, against real Git commits and this repository's files."""

from __future__ import annotations

import ast
import fnmatch
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ci_changes  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ALL_JOBS = set(ci_changes.JOB_INPUTS)
PACKAGES = {"package_core", "native_package", "homebrew_package", "windows_package"}
# The package builds run on pushes and manual runs, never on pull requests.
PACKAGE_BUILDS = PACKAGES - {"package_core"}
PULL_REQUEST_JOBS = ALL_JOBS - PACKAGE_BUILDS
LIBRARY_JOBS = {"code", "conan", "lint"}
# Release modules each job starts, directly or through a script in scripts/release/.
ENTRY_MODULES = {
    "package_core": ("release/construct_core.py",),
    "native_package": ("release/native_builder.py", "release/native_build.py",
                       "release/package_validation.py"),
    "homebrew_package": ("release/package_validation.py",),
    "windows_package": ("release/cli.py", "release/conan_validation.py",
                        "scripts/release/collect_build_identity.py"),
}
README = """# sdk

Prose.

```cpp
int main() { return 0; }
```
"""
GUIDE = """Guide
=====

Prose.

.. code-block:: cpp

   int main() { return 0; }

.. code-block:: cpp

   client.ping();
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
        self.write("docs/guide.rst", GUIDE)
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

    def jobs(self, event: str, head: str, base: str | None = None) -> set[str]:
        lines = subprocess.run(
            [sys.executable, ci_changes.__file__, "--event", event,
             "--base", self.base if base is None else base, "--head", head],
            check=True, capture_output=True, text=True,
        ).stdout.split()
        outputs = dict(line.split("=") for line in lines)
        self.assertEqual(set(outputs), ALL_JOBS)
        return {job for job, value in outputs.items() if value == "true"}

    def jobs_for_change(self, path: str, event: str = "pull_request") -> set[str]:
        self.write(path, f"{path} {self.git('rev-parse', 'HEAD')}\n")
        head = self.commit()
        jobs = self.jobs(event, head)
        self.base = head
        return jobs

    def test_files_no_job_reads_run_nothing(self) -> None:
        for path in ("docs/index.rst", "examples/README.md", "release/README.md",
                     ".github/ISSUE_TEMPLATE/bug.yml", ".clang-format", "Makefile",
                     "conformance/run.sh"):
            for event in ("pull_request", "push"):
                with self.subTest(path=path, event=event):
                    self.assertEqual(self.jobs_for_change(path, event), set())

    def test_readme_prose_runs_nothing(self) -> None:
        self.write("README.md", README.replace("Prose.", "Better prose."))
        self.assertEqual(self.jobs("pull_request", self.commit()), set())

    def test_readme_cpp_block_runs_code_jobs(self) -> None:
        self.write("README.md", README.replace("return 0;", "return 1;"))
        self.assertEqual(self.jobs("pull_request", self.commit()), {"code"})

    def test_version_pin_text_runs_the_release_checks(self) -> None:
        self.write("README.md", README.replace("Prose.", "Prose.\n\n    GIT_TAG v9.9.9\n"))
        self.assertEqual(self.jobs("pull_request", self.commit()), {"release"})
        self.write("docs/installation.rst", "choco install mcp-cpp-sdk --version=9.9.9\n")
        pinned = self.commit()
        self.assertEqual(self.jobs("pull_request", pinned), {"release"})
        self.write("docs/installation.rst", "choco install mcp-cpp-sdk --version=9.9.9\n\nProse.\n")
        self.assertEqual(self.jobs("pull_request", self.commit(), base=pinned), set())

    def test_changelog_runs_the_release_checks(self) -> None:
        for event in ("pull_request", "push"):
            with self.subTest(event=event):
                self.assertEqual(self.jobs_for_change("CHANGELOG.md", event), {"release"})

    def test_doc_prose_and_code_run_nothing(self) -> None:
        self.write("docs/guide.rst",
                   GUIDE.replace("Prose.", "Better prose.").replace("ping", "close")
                   .replace("return 0;", "return 1;"))
        self.write("docs/other.rst", GUIDE)
        self.assertEqual(self.jobs("pull_request", self.commit()), set())

    def test_removed_doc_page_runs_nothing(self) -> None:
        self.git("rm", "-q", "docs/index.rst")
        self.assertEqual(self.jobs("pull_request", self.commit()), set())

    def test_pull_request_runs_only_the_jobs_reading_the_path(self) -> None:
        expected = {
            "src/main.cpp": LIBRARY_JOBS,
            "include/mcp/sdk.hpp": LIBRARY_JOBS,
            "test/core/protocol_test.cpp": {"code"},
            "test/data/json-schema/suite.json": {"code", "release"},
            "test/include/support/pipe_stream.hpp": {"code"},
            "test/perf/schema_workload.cpp": {"code"},
            "scripts/schema_ir_gate.py": {"release"},
            "examples/features/pagination.cpp": {"code", "lint"},
            "scripts/run_examples.py": {"code", "release"},
            "test_package/conanfile.py": {"conan"} | PACKAGES,
            "LICENSE": {"conan"},
            "benchmark/run.sh": {"benchmark"},
            "test/release/test_signing.py": {"release"},
            "release/github_anchor.py": {"release"},
            "scripts/ci_changes.py": {"release", "lint"},
            ".clang-tidy": {"lint"},
            ".github/workflows/release.yml": {"release"},
            ".github/workflows/release-candidate.yml": {"release"},
            ".github/workflows/conformance.yml": {"release"},
            "packaging/aur/PKGBUILD.in": {"release", "package_core"},
            "packaging/debian/control.in": {"release", "package_core", "native_package"},
            "release/native-builders/lock.json": {"release", "package_core", "native_package"},
            "packaging/homebrew/mcp-cpp-sdk.rb.in": {"release", "package_core", "homebrew_package"},
            "scripts/release/validate_homebrew_candidate.sh": {"release", "package_core", "homebrew_package"},
            "packaging/chocolatey/mcp-cpp-sdk.nuspec.in": {"release", "package_core", "windows_package"},
            "scripts/release/build_windows.ps1": {"release", "powershell", "package_core", "windows_package"},
            "scripts/release/check_powershell_syntax.ps1": {"release", "powershell"},
            "packaging/targets.json": {"release"} | PACKAGES,
            "release/construct_core.py": {"release"} | PACKAGES,
            "scripts/package_build.py": {"release"} | PACKAGES,
            "CMakeLists.txt": LIBRARY_JOBS | {"release", "package_core"},
            "cmake/config.cmake.in": LIBRARY_JOBS | {"package_core"},
            "conanfile.py": (LIBRARY_JOBS - {"lint"}) | {"release", "package_core"},
            "VERSION": LIBRARY_JOBS | {"release"} | PACKAGES,
            ".github/workflows/ci.yml": PULL_REQUEST_JOBS,
        }
        for path, jobs in expected.items():
            with self.subTest(path=path):
                self.assertEqual(self.jobs_for_change(path), jobs)

    def test_push_builds_the_packages_reading_the_path(self) -> None:
        expected = {
            "test_package/conanfile.py": {"conan"} | PACKAGES,
            "packaging/aur/PKGBUILD.in": {"release", "package_core"},
            "packaging/debian/control.in": {"release", "package_core", "native_package"},
            "release/native-builders/lock.json": {"release", "package_core", "native_package"},
            "packaging/homebrew/mcp-cpp-sdk.rb.in":
                {"release", "package_core", "homebrew_package"},
            "scripts/release/validate_homebrew_candidate.sh":
                {"release", "package_core", "homebrew_package"},
            "packaging/chocolatey/mcp-cpp-sdk.nuspec.in":
                {"release", "package_core", "windows_package"},
            "scripts/release/build_windows.ps1":
                {"release", "powershell", "package_core", "windows_package"},
            "packaging/targets.json": {"release"} | PACKAGES,
            "release/construct_core.py": {"release"} | PACKAGES,
            "scripts/package_build.py": {"release"} | PACKAGES,
            "CMakeLists.txt": LIBRARY_JOBS | {"release"} | PACKAGES,
            "cmake/config.cmake.in": LIBRARY_JOBS | PACKAGES,
            "conanfile.py": (LIBRARY_JOBS - {"lint"}) | {"release"} | PACKAGES,
            "VERSION": LIBRARY_JOBS | {"release"} | PACKAGES,
            ".github/workflows/ci.yml": ALL_JOBS,
        }
        for path, jobs in expected.items():
            with self.subTest(path=path):
                self.assertEqual(self.jobs_for_change(path, "push"), jobs)

    def test_other_events_also_package_source(self) -> None:
        for path, jobs in (("src/main.cpp", LIBRARY_JOBS), ("test/core/protocol_test.cpp", {"code"}),
                           ("examples/features/pagination.cpp", {"code", "lint"}),
                           ("LICENSE", {"conan"})):
            with self.subTest(path=path):
                self.assertEqual(self.jobs_for_change(path, "push"), jobs | PACKAGES)

    def test_push_runs_the_same_jobs_outside_source(self) -> None:
        for path, jobs in ((".github/workflows/release.yml", {"release"}),
                           ("benchmark/run.sh", {"benchmark"}),
                           ("packaging/rpm/mcp-cpp-sdk.spec.in",
                            {"release", "package_core", "native_package"})):
            with self.subTest(path=path):
                self.assertEqual(self.jobs_for_change(path, "push"), jobs)

    def test_a_change_runs_the_jobs_of_every_path(self) -> None:
        self.write("docs/index.rst", "Index, edited\n")
        self.write("benchmark/run.sh", "true\n")
        self.write("src/main.cpp", "int y;\n")
        self.assertEqual(self.jobs("pull_request", self.commit()), LIBRARY_JOBS | {"benchmark"})

    def test_a_file_no_list_names_runs_every_job(self) -> None:
        self.assertEqual(self.jobs_for_change("new-directory/file.txt", "push"), ALL_JOBS)
        self.assertEqual(self.jobs_for_change("other-directory/file.txt"), PULL_REQUEST_JOBS)

    def test_manual_runs_always_need_every_job(self) -> None:
        self.write("docs/index.rst", "Index, edited\n")
        head = self.commit()
        self.assertEqual(self.jobs("workflow_dispatch", head), ALL_JOBS)
        self.assertEqual(self.jobs("pull_request", head), set())

    def test_doubt_runs_every_job(self) -> None:
        head = self.git("rev-parse", "HEAD")
        for base in ("", ci_changes.ZERO_SHA, "f" * 40):
            with self.subTest(base=base):
                self.assertEqual(self.jobs("push", head, base=base), ALL_JOBS)
                self.assertEqual(self.jobs("pull_request", head, base=base), PULL_REQUEST_JOBS)
        self.assertEqual(self.jobs("pull_request", self.commit()), PULL_REQUEST_JOBS)  # empty diff


def release_modules(path: Path) -> set[str]:
    """Return the release/ modules that path imports, directly or through another module."""
    seen: set[str] = set()
    pending = [path]
    while pending:
        tree = ast.parse(pending.pop().read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if not isinstance(node, ast.ImportFrom):
                continue
            if node.module is None:
                names = [alias.name for alias in node.names]
            elif node.level or node.module.startswith("release."):
                names = [node.module.removeprefix("release.")]
            else:
                continue
            for name in names:
                module = f"release/{name}.py"
                if module not in seen:
                    seen.add(module)
                    pending.append(ROOT / module)
    return seen


class RepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tracked = subprocess.run(
            ["git", "ls-files"], cwd=ROOT, check=True, capture_output=True, text=True
        ).stdout.splitlines()

    def workflow_paths(self, workflow: str) -> list[list[str]]:
        text = (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8")
        blocks = re.findall(r"^    paths:\n((?:      - .+\n)+)", text, re.MULTILINE)
        return [re.findall(r"^      - '(.+)'$", block, re.MULTILINE) for block in blocks]

    def test_every_tracked_file_is_classified(self) -> None:
        patterns = ci_changes.NO_JOB_PATTERNS + tuple(
            pattern for inputs in ci_changes.JOB_INPUTS.values() for pattern in inputs
        )
        unclassified = [path for path in self.tracked if not ci_changes.matches(path, patterns)]
        self.assertEqual(unclassified, [])

    def test_every_pattern_names_a_tracked_file(self) -> None:
        for patterns in (ci_changes.NO_JOB_PATTERNS, ci_changes.PACKAGED_SOURCE,
                         *ci_changes.JOB_INPUTS.values()):
            for pattern in patterns:
                with self.subTest(pattern=pattern):
                    self.assertTrue(fnmatch.filter(self.tracked, pattern))

    def test_package_jobs_list_the_release_modules_they_import(self) -> None:
        for job, entries in ENTRY_MODULES.items():
            modules = {module for entry in entries for module in release_modules(ROOT / entry)}
            modules.update(entry for entry in entries)
            for module in sorted(modules):
                with self.subTest(job=job, module=module):
                    self.assertTrue(ci_changes.matches(module, ci_changes.JOB_INPUTS[job]))

    def test_ci_workflow_has_an_output_for_every_job(self) -> None:
        text = (ROOT / ci_changes.CI_WORKFLOW).read_text(encoding="utf-8")
        declared = set(re.findall(r"^      (\w+): \$\{\{ steps\.classify\.outputs\.\1 \}\}$", text,
                                  re.MULTILINE))
        used = set(re.findall(r"needs\.changes\.outputs\.(\w+)", text))
        self.assertEqual(declared, ALL_JOBS)
        self.assertEqual(used, ALL_JOBS)

    def test_ci_result_needs_every_other_job(self) -> None:
        text = (ROOT / ci_changes.CI_WORKFLOW).read_text(encoding="utf-8")
        jobs = text[text.index("\njobs:\n"):]
        declared = set(re.findall(r"^  ([\w-]+):$", jobs, re.MULTILINE))
        result = re.search(r"^  result:\n(?:    .*\n|\n)*?    needs:\n((?:      - .+\n)+)", jobs,
                           re.MULTILINE)
        self.assertIsNotNone(result)
        needed = set(re.findall(r"^      - ([\w-]+)$", result.group(1), re.MULTILINE))
        self.assertEqual(needed, declared - {"result"})

    def test_workflow_path_filters_name_tracked_files(self) -> None:
        for workflow in ("conformance.yml", "conformance-guards.yml", "docs.yml"):
            blocks = self.workflow_paths(workflow)
            self.assertEqual(len(blocks), 2, workflow)
            self.assertEqual(blocks[0], blocks[1], workflow)
            for pattern in blocks[0]:
                with self.subTest(workflow=workflow, pattern=pattern):
                    self.assertTrue(
                        pattern.startswith("!") or fnmatch.filter(self.tracked, pattern)
                    )


if __name__ == "__main__":
    unittest.main()
