#!/usr/bin/env python3
"""
Decide which CI jobs a change needs.

A change that only touches documentation cannot change what the build, test,
sanitizer and packaging jobs check, so those jobs are skipped for it. README.md
is the one exception: CI compiles its C++ blocks, so a README change counts as
code when those blocks differ.

Packaging jobs take most of a run's runners, and runner concurrency is shared
across the account, so a pull request runs them only when it changes something
they build from. Every other event that runs code jobs also runs packaging, so
main still checks each merge.

Prints ``code=`` and ``packaging=`` lines for ``$GITHUB_OUTPUT``. Any doubt (no
base commit, a git failure, an unknown file) answers ``true``.
"""
import argparse
import fnmatch
import re
import subprocess
import sys

# Paths that no CI job reads. docs/ is built only by docs.yml, which runs on main.
DOC_PATTERNS = ("docs/*", "*.md", "LICENSE", ".github/ISSUE_TEMPLATE/*")
# Paths that define how the native, Homebrew and Windows packages are assembled. Source files are
# left out because main still packages every merge.
PACKAGING_PATTERNS = (
    ".gitattributes",
    ".github/workflows/ci.yml",
    ".github/workflows/release*.yml",
    "CMakeLists.txt",
    "VERSION",
    "cmake/*",
    "conanfile.py",
    "keys/*",
    "packaging/*",
    "release/*",
    "scripts/distro_build.py",
    "scripts/package_build.py",
    "scripts/release/*",
    "scripts/run_release_tool.py",
    "test_package/*",
)
# Same block syntax as scripts/check_readme_snippets.py.
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


def changed_files(base, head):
    """Return the changed paths, or an empty list when the change cannot be determined."""
    if not base or base == ZERO_SHA:
        return []
    try:
        return git("diff", "--name-only", "--no-renames", base, head).splitlines()
    except subprocess.CalledProcessError:
        return []


def needs_code_jobs(base, head):
    changed = changed_files(base, head)
    if not changed:
        return True
    if any(not matches(path, DOC_PATTERNS) for path in changed):
        return True
    if "README.md" in changed:
        return readme_cpp_blocks(base) != readme_cpp_blocks(head)
    return False


def needs_packaging_jobs(event, base, head):
    if event != "pull_request":
        return True
    changed = changed_files(base, head)
    return not changed or any(matches(path, PACKAGING_PATTERNS) for path in changed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", required=True)
    parser.add_argument("--base", default="")
    parser.add_argument("--head", required=True)
    args = parser.parse_args()
    code = args.event == "workflow_dispatch" or needs_code_jobs(args.base, args.head)
    packaging = code and needs_packaging_jobs(args.event, args.base, args.head)
    print(f"code={'true' if code else 'false'}")
    print(f"packaging={'true' if packaging else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
