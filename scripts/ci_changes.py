#!/usr/bin/env python3
"""
Decide which jobs in ci.yml a change needs.

JOB_INPUTS lists the paths each job reads, so a job runs only when something it
reads changed. The exception is the C++ that the code jobs compile from the
documentation: a change to README.md counts for them when its C++ blocks differ.

The package builds take most of a run's runner time, so a pull request runs one
only when it changes that package's own inputs, such as its packaging files,
VERSION or its build scripts, and not for the build files and source that every
package builds (PULL_REQUEST_UNPACKAGED). Pushes to main run them for both. A pull
request still constructs the source archive and recipes when it changes what
they read. A manual run builds the packages on any branch, and the release
workflow builds and validates every package again before publishing.

The release checks also read the text in the documentation that names the
release version (release/version_bump.py PINS), so a change to that text counts
for them, and CHANGELOG.md, whose entries they check and from which the release
notes are written.

conformance.yml, conformance-guards.yml and docs.yml filter their own triggers
with `paths:`, and CodeQL is GitHub's default setup, which has no path filter.

Prints one ``job=true|false`` line per job for ``$GITHUB_OUTPUT``. Any doubt (no
base commit, a git failure, a file no list names) answers ``true`` for every job
the event runs.
"""
import argparse
import fnmatch
from pathlib import Path
import re
import subprocess
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release.version_bump import PINS  # noqa: E402

# ci.yml defines every job, so a change to it needs them all.
CI_WORKFLOW = ".github/workflows/ci.yml"
# Paths that no job in ci.yml reads. conformance.yml and docs.yml read conformance/ and docs/.
NO_JOB_PATTERNS = (
    "*.md",
    ".clang-format",
    ".devcontainer/*",
    ".github/CODEOWNERS",
    ".github/ISSUE_TEMPLATE/*",
    ".github/codecov.yml",
    ".github/labels.yml",
    ".gitignore",
    ".pre-commit-config.yaml",
    "Makefile",
    "conformance/*",
    "docs/*",
)
LIBRARY = (
    "CMakeLists.txt",
    "VERSION",
    "cmake/*",
    "conanfile.py",
    "include/*",
    "src/*",
    "third_party/*",
)
TESTS = (
    "test/auth/*",
    "test/client/*",
    "test/core/*",
    "test/data/*",
    "test/e2e/*",
    "test/headers/*",
    "test/pipe_stream.hpp",
    "test/server/*",
    "test/support/*",
    "test/test_utils.hpp",
    "test/transport/*",
    "test/tsan.supp",
)
# What package-core reads to construct the source archive and recipes. The packages build from it.
PACKAGE_CORE = (
    ".gitattributes",
    "CMakeLists.txt",
    "VERSION",
    "cmake/*",
    "conanfile.py",
    "keys/release-signing-key.asc",
    "packaging/targets.json",
    "packaging/templates.json",
    "release/__init__.py",
    "release/artifacts.py",
    "release/construct_core.py",
    "release/model.py",
    "release/native_builder_lock.py",
    "release/native_targets.py",
    "release/publication_contract.py",
    "release/release_notes.py",
    "release/source-archive-inventory.txt",
    "release/strict_json.py",
    "release/templates.py",
    "scripts/package_build.py",
    "scripts/run_release_tool.py",
    "test_package/*",
    "third_party/*",
)
PACKAGE_VALIDATION = ("release/consumer_validation.py", "release/package_validation.py")
# The native builder and what it imports. Windows reaches it through collect_build_identity.py.
NATIVE_BUILDER = (
    "release/build_identity.py",
    "release/conan_validation.py",
    "release/consumer_validation.py",
    "release/loopback_archive.py",
    "release/native_build.py",
    "release/native_builder.py",
    "scripts/release/collect_build_identity.py",
)
JOB_INPUTS = {
    "naming": (
        ".clang-tidy",
        "CMakeLists.txt",
        "VERSION",
        "cmake/*",
        "examples/*",
        "include/*",
        "scripts/ci_changes.py",
        "src/*",
        "third_party/*",
    ),
    "code": LIBRARY + TESTS + (
        "examples/*",
        "scripts/build.py",
        "scripts/check_boost_free_headers.py",
        "scripts/check_example_progression.py",
        "scripts/check_json_matrix.py",
        "scripts/check_readme_snippets.py",
        "scripts/check_stdio_streams.py",
        "scripts/gen_json_matrix.py",
        "scripts/init.py",
        "scripts/json_census.py",
        "scripts/json_census_dispositions.json",
        "scripts/run_examples.py",
        "scripts/test_check_example_progression.py",
        "scripts/test_check_json_matrix.py",
    ),
    "conan": LIBRARY + ("LICENSE", "scripts/init.py", "test_package/*"),
    "release": (
        ".github/actionlint.yaml",
        ".github/workflows/*",
        "CMakeLists.txt",
        "VERSION",
        "conanfile.py",
        "keys/*",
        "packaging/*",
        "release/*",
        "scripts/*",
        "test/data/*",
        "test/release/*",
        "third_party/*",
    ),
    "benchmark": ("benchmark/*",),
    "powershell": ("scripts/release/*.ps1",),
    "package_core": PACKAGE_CORE + (
        "packaging/aur/*",
        "packaging/conan-center/*",
        "packaging/homebrew/*",
    ),
    "native_package": PACKAGE_CORE + PACKAGE_VALIDATION + NATIVE_BUILDER + (
        "packaging/debian/*",
        "packaging/rpm/*",
        "release/native-builders/*",
    ),
    "homebrew_package": PACKAGE_CORE + PACKAGE_VALIDATION + (
        "packaging/homebrew/*",
        "scripts/release/validate_homebrew_candidate.sh",
    ),
    "windows_package": PACKAGE_CORE + NATIVE_BUILDER + (
        "packaging/chocolatey/*",
        "packaging/conan-center/*",
        "release/cli.py",
        "release/windows/*",
        "scripts/release/build_deterministic_zip.ps1",
        "scripts/release/build_windows.ps1",
        "scripts/release/install_windows_conan.ps1",
        "scripts/release/validate_conan_candidate.ps1",
    ),
}
PACKAGE_JOBS = frozenset({"native_package", "homebrew_package", "windows_package"})
# Package inputs that most changes touch. A pull request does not build the packages for them.
PULL_REQUEST_UNPACKAGED = (
    ".gitattributes",
    "CMakeLists.txt",
    "cmake/*",
    "conanfile.py",
    "third_party/*",
)
# Source a package builds and tests. Events other than pull_request package these as well.
PACKAGED_SOURCE = LIBRARY + TESTS + (
    "LICENSE",
    "examples/*",
    "scripts/build.py",
    "scripts/init.py",
    "scripts/run_examples.py",
)
# Same block syntax as scripts/check_readme_snippets.py.
# The release checks read it, though no job reads the other Markdown files.
CHANGELOG = "CHANGELOG.md"
CPP_FENCE_RE = re.compile(r"^```cpp\s*$(.*?)^```\s*$", re.MULTILINE | re.DOTALL)
ZERO_SHA = "0" * 40


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def matches(path, patterns):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def readme_cpp_blocks(revision):
    try:
        text = git("show", f"{revision}:README.md")
    except subprocess.CalledProcessError:
        return None
    return CPP_FENCE_RE.findall(text)


def version_pins(revision, path):
    """Return the text that names the release version in a file; an absent file has none."""
    try:
        text = git("show", f"{revision}:{path}")
    except subprocess.CalledProcessError:
        return []
    return [match.group(0) for pin in PINS if pin.path == path for match in pin.pattern().finditer(text)]


def changed_files(base, head):
    """Return the changed paths, or an empty list when the change cannot be determined."""
    if not base or base == ZERO_SHA:
        return []
    try:
        return git("diff", "--name-only", "--no-renames", base, head).splitlines()
    except subprocess.CalledProcessError:
        return []


def jobs_reading(path, event):
    if path == CI_WORKFLOW:
        return all_jobs(event)
    if matches(path, NO_JOB_PATTERNS):
        return set()
    jobs = {job for job, patterns in JOB_INPUTS.items() if matches(path, patterns)}
    if event == "pull_request":
        if matches(path, PULL_REQUEST_UNPACKAGED):
            jobs -= PACKAGE_JOBS
    elif matches(path, PACKAGED_SOURCE):
        jobs |= PACKAGE_JOBS
    return jobs or all_jobs(event)


def all_jobs(event):
    """Every job the event runs when a change cannot be narrowed down."""
    jobs = set(JOB_INPUTS)
    if event == "pull_request":
        jobs -= PACKAGE_JOBS
    return jobs


def needed_jobs(event, base, head):
    changed = changed_files(base, head)
    if event == "workflow_dispatch":
        jobs = set(JOB_INPUTS)
    elif not changed:
        jobs = all_jobs(event)
    else:
        jobs = set().union(*(jobs_reading(path, event) for path in changed))
        if "README.md" in changed and readme_cpp_blocks(base) != readme_cpp_blocks(head):
            jobs.add("code")
        if any(version_pins(base, path) != version_pins(head, path)
               for path in changed if path in {pin.path for pin in PINS}):
            jobs.add("release")
        if CHANGELOG in changed:
            jobs.add("release")
    if jobs & PACKAGE_JOBS:
        jobs.add("package_core")
    return jobs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", required=True)
    parser.add_argument("--base", default="")
    parser.add_argument("--head", required=True)
    args = parser.parse_args()
    jobs = needed_jobs(args.event, args.base, args.head)
    for job in JOB_INPUTS:
        print(f"{job}={'true' if job in jobs else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
