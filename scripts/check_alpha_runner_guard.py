#!/usr/bin/env python3
"""Guard checks that keep the alpha conformance runner out of CI gating.

Keeps the alpha conformance runner side-channel (
``@modelcontextprotocol/conformance@0.2.0-alpha.12``, spec ``2026-07-28``)
permanently non-default and non-CI-gating:

  (i)   ``conformance/runner/package.json`` must pin an exact, non-prerelease
        runner version, and ``conformance/run.sh`` must read that pin instead
        of naming a version of its own.
  (ii)  no file under ``.github/workflows/`` or ``scripts/`` (this guard
        excluded) may reference the alpha results directory
        (``conformance-results-alpha``) as a tier-evidence path.
  (iii) no file under ``.github/workflows/`` or ``scripts/`` (this guard
        excluded) may reference the alpha runner entrypoint
        (``run_alpha.sh``) as something it executes and gates on.

This intentionally does not require ``conformance/run_alpha.sh`` to exist:
it only asserts that CI-facing workflows and phase-exit scripts do not
*consume* the alpha runner as a pass/fail condition. Pattern-based, not
presence-based.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RUN_SH = REPO_ROOT / "conformance" / "run.sh"
PACKAGE_JSON = REPO_ROOT / "conformance" / "runner" / "package.json"
RUNNER_PACKAGE = "@modelcontextprotocol/conformance"
EXACT_RELEASE = re.compile(r"\d+\.\d+\.\d+")

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


def read_pin(package_json: Path = PACKAGE_JSON) -> str:
    if not package_json.is_file():
        raise GuardError(f"{package_json} does not exist; cannot read the runner pin")
    try:
        pin = json.loads(package_json.read_text(encoding="utf-8"))["dependencies"][RUNNER_PACKAGE]
    except (KeyError, TypeError, ValueError) as error:
        raise GuardError(f"{package_json} does not pin {RUNNER_PACKAGE}: {error!r}") from error
    if not isinstance(pin, str) or not EXACT_RELEASE.fullmatch(pin):
        raise GuardError(
            f"{package_json.relative_to(REPO_ROOT) if package_json.is_relative_to(REPO_ROOT) else package_json} "
            f"pins {RUNNER_PACKAGE} to {pin!r}; the gate needs an exact release version "
            "(no range and no -alpha, -beta or -rc suffix)"
        )
    return pin


def check_pin(run_sh: Path = RUN_SH, package_json: Path = PACKAGE_JSON) -> str:
    pin = read_pin(package_json)
    if not run_sh.is_file():
        raise GuardError(f"{run_sh} does not exist; cannot verify it reads the runner pin")
    text = run_sh.read_text(encoding="utf-8")
    if "runner/package.json" not in text:
        raise GuardError(
            "conformance/run.sh does not read conformance/runner/package.json, so it "
            "would not check the installed runner against the single pin"
        )
    if re.search(r"(?<![\d.])\d+\.\d+\.\d+(?![\d.])", text):
        raise GuardError(
            "conformance/run.sh names a runner version of its own; it must take the "
            "version from conformance/runner/package.json"
        )
    return pin


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
            "The alpha runner must remain non-default and non-CI-gating. "
            "Remove the reference from any file "
            "under .github/workflows/ or scripts/; the alpha harness itself "
            "(conformance/run_alpha.sh) legitimately owns these strings and is not scanned."
        )


def main() -> int:
    pin = check_pin()
    check_no_gating_reference()
    print(
        f"alpha-runner guard passed: {RUN_SH.relative_to(REPO_ROOT)} reads the "
        f"{pin} pin from {PACKAGE_JSON.relative_to(REPO_ROOT)}; no CI workflow or scripts/ guard references the alpha "
        "runner/results as a gating condition"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GuardError as error:
        print(f"alpha-runner guard failed: {error}", file=sys.stderr)
        raise SystemExit(1)
