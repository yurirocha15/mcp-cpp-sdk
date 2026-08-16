#!/usr/bin/env python3
"""Path-pair tripwire for WORK_PLAN.local.md item 1.10(b) / Critic M10, #6.

``src/auth/oauth.cpp`` and ``src/transport/http_client.cpp`` are the
OAuth-vs-transport collision seam: OAuth work and transport work are
developed on separate lanes, and a commit range that touches both at once
is treated as a collision.

This check fails any push or pull request whose commit range touches BOTH
files, unconditionally, UNLESS a commit message in that range contains an
explicit override token naming the authorizing plan item:

    PLAN-OVERRIDE: <item>        e.g. PLAN-OVERRIDE: 4.6

The check never inspects plan or status files to decide whether the named
item has "started" or is otherwise legitimate -- the default is fail, and
the author must assert the authorization explicitly in the commit message.

Reads BASE_SHA and HEAD_SHA from the environment (set by the calling
workflow from the push/pull_request event payload).
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
ALL_ZERO_SHA = re.compile(r"^0+$")
TARGET_FILES = (
    "src/auth/oauth.cpp",
    "src/transport/http_client.cpp",
)
OVERRIDE_RE = re.compile(r"PLAN-OVERRIDE:\s*(\S+)")


class TripwireError(RuntimeError):
    """Raised when the path-pair tripwire fires without a valid override."""


def run_git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], capture_output=True, text=True, check=True
    )
    return result.stdout


def resolve_ref(raw: str) -> str:
    raw = (raw or "").strip()
    if not raw or ALL_ZERO_SHA.match(raw):
        # Branch/ref creation: no real "before" commit exists, so diff
        # against git's empty-tree object instead.
        return EMPTY_TREE
    return raw


def changed_files(base: str, head: str) -> set[str]:
    # Three-dot diff (against the merge-base) matches what pull_request
    # "Files changed" shows and degrades to a normal two-dot diff for the
    # linear push-event case.
    if base == EMPTY_TREE:
        output = run_git("diff", "--name-only", base, head)
    else:
        output = run_git("diff", "--name-only", f"{base}...{head}")
    return {line.strip() for line in output.splitlines() if line.strip()}


def commit_messages(base: str, head: str) -> str:
    log_range = head if base == EMPTY_TREE else f"{base}..{head}"
    return run_git("log", "--format=%B", log_range)


def main() -> int:
    base = resolve_ref(os.environ.get("BASE_SHA", ""))
    head = resolve_ref(os.environ.get("HEAD_SHA", ""))

    if head == EMPTY_TREE:
        print("no HEAD_SHA to inspect; nothing to check")
        return 0

    touched = set(TARGET_FILES) & changed_files(base, head)
    if len(touched) < 2:
        print(
            "path-pair tripwire passed: this push/PR does not touch both "
            f"{' and '.join(TARGET_FILES)}"
        )
        return 0

    override_items = OVERRIDE_RE.findall(commit_messages(base, head))
    if override_items:
        print(
            "path-pair tripwire passed: both files are touched, but an explicit "
            f"override was found: PLAN-OVERRIDE: {override_items[0]}"
        )
        return 0

    raise TripwireError(
        "this push/PR touches BOTH src/auth/oauth.cpp and "
        "src/transport/http_client.cpp. These two files are the OAuth-vs-transport "
        "collision seam (WORK_PLAN.local.md 1.10(b), Critic M10/#6): OAuth work and "
        "transport work are developed on separate lanes, and a commit range that "
        "touches both at once fails by default. This check does not look up plan "
        "state -- to override, add a line to a commit message in this range naming "
        "the authorizing plan item, e.g.:\n\n"
        "    PLAN-OVERRIDE: 4.6\n"
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TripwireError as error:
        print(f"path-pair tripwire failed: {error}", file=sys.stderr)
        raise SystemExit(1)
