#!/usr/bin/env python3
"""Guard checks for WORK_PLAN.local.md item 1.10(a) / do-not-do #9.

Keeps the alpha conformance runner side-channel (item 1.6:
``@modelcontextprotocol/conformance@0.2.0-alpha.11``, spec ``2026-07-28``)
permanently non-default and non-CI-gating:

  (i)   ``conformance/run.sh`` must stay pinned to the locked ``0.1.16``
        runner used for tier evidence.
  (ii)  no file under ``.github/workflows/`` or ``scripts/`` (this guard
        excluded) may reference the alpha results directory
        (``conformance-results-alpha``) as a tier-evidence path.
  (iii) no file under ``.github/workflows/`` or ``scripts/`` (this guard
        excluded) may reference the alpha runner entrypoint
        (``run_alpha.sh``) as something it executes and gates on.

This intentionally does not require ``conformance/run_alpha.sh`` to exist:
it only asserts that CI-facing workflows and phase-exit scripts do not
*consume* the alpha runner as a pass/fail condition, per do-not-do #9's
second clause. Pattern-based, not presence-based.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RUN_SH = REPO_ROOT / "conformance" / "run.sh"
PINNED_VERSION = "0.1.16"

SCAN_DIRS = (
    REPO_ROOT / ".github" / "workflows",
    REPO_ROOT / "scripts",
)

# This guard must itself name the forbidden patterns, so it excludes its own
# file from the scan rather than tripping over its own docstring/regex.
SELF_PATH = Path(__file__).resolve()

FORBIDDEN_PATTERN = re.compile(r"conformance-results-alpha|run_alpha\.sh")


class GuardError(RuntimeError):
    """Raised when an alpha-runner non-gating invariant is violated."""


def check_pin() -> None:
    if not RUN_SH.is_file():
        raise GuardError(f"{RUN_SH} does not exist; cannot verify the {PINNED_VERSION} pin")
    text = RUN_SH.read_text(encoding="utf-8")
    if PINNED_VERSION not in text:
        raise GuardError(
            f"{RUN_SH.relative_to(REPO_ROOT)} no longer contains the pinned runner version "
            f"'{PINNED_VERSION}' (WORK_PLAN.local.md 1.6: run.sh stays pinned while the alpha "
            "harness is a separate, non-default, non-CI-gating side channel)"
        )


def iter_scanned_files() -> list[Path]:
    files: list[Path] = []
    for scan_dir in SCAN_DIRS:
        if not scan_dir.is_dir():
            continue
        for path in sorted(scan_dir.rglob("*")):
            if not path.is_file():
                continue
            if path.resolve() == SELF_PATH:
                continue
            files.append(path)
    return files


def check_no_gating_reference() -> None:
    violations: list[str] = []
    for path in iter_scanned_files():
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            if FORBIDDEN_PATTERN.search(line):
                violations.append(f"{path.relative_to(REPO_ROOT)}:{lineno}: {line.strip()!r}")

    if violations:
        joined = "\n  ".join(violations)
        raise GuardError(
            "the alpha runner/results directory is referenced by a CI workflow or "
            "scripts/ guard, which would let it gate CI:\n  "
            f"{joined}\n"
            "The alpha runner (item 1.6) must remain non-default and non-CI-gating "
            "(WORK_PLAN.local.md do-not-do #9). Remove the reference from any file "
            "under .github/workflows/ or scripts/; the alpha harness itself "
            "(conformance/run_alpha.sh) legitimately owns these strings and is not scanned."
        )


def main() -> int:
    check_pin()
    check_no_gating_reference()
    print(
        f"alpha-runner guard passed: {RUN_SH.relative_to(REPO_ROOT)} pinned to "
        f"{PINNED_VERSION}; no CI workflow or scripts/ guard references the alpha "
        "runner/results as a gating condition"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GuardError as error:
        print(f"alpha-runner guard failed: {error}", file=sys.stderr)
        raise SystemExit(1)
