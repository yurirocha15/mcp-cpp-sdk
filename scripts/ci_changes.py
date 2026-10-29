#!/usr/bin/env python3
"""
Decide whether a CI run needs the build and test jobs.

A change that only touches documentation cannot change what the build, test,
sanitizer and packaging jobs check, so those jobs are skipped for it. README.md
is the one exception: CI compiles its C++ blocks, so a README change counts as
code when those blocks differ.

Prints ``code=true`` or ``code=false`` for ``$GITHUB_OUTPUT``. Any doubt (no
base commit, a git failure, an unknown file) answers ``code=true``.
"""
import argparse
import fnmatch
import re
import subprocess
import sys

# Paths that no CI job reads. docs/ is built only by docs.yml, which runs on main.
DOC_PATTERNS = ("docs/*", "*.md", "LICENSE", ".github/ISSUE_TEMPLATE/*")
# Same block syntax as scripts/check_readme_snippets.py.
CPP_FENCE_RE = re.compile(r"^```cpp\s*$(.*?)^```\s*$", re.MULTILINE | re.DOTALL)
ZERO_SHA = "0" * 40


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def is_doc(path):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in DOC_PATTERNS)


def readme_cpp_blocks(revision):
    try:
        text = git("show", f"{revision}:README.md")
    except subprocess.CalledProcessError:
        return None
    return CPP_FENCE_RE.findall(text)


def needs_code_jobs(base, head):
    if not base or base == ZERO_SHA:
        return True
    try:
        changed = git("diff", "--name-only", "--no-renames", base, head).splitlines()
    except subprocess.CalledProcessError:
        return True
    if not changed:
        return True
    if any(not is_doc(path) for path in changed):
        return True
    if "README.md" in changed:
        return readme_cpp_blocks(base) != readme_cpp_blocks(head)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", required=True)
    parser.add_argument("--base", default="")
    parser.add_argument("--head", required=True)
    args = parser.parse_args()
    code = args.event == "workflow_dispatch" or needs_code_jobs(args.base, args.head)
    print(f"code={'true' if code else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
