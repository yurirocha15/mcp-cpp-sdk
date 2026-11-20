#!/usr/bin/env python3
"""Guard checks that keep the alpha conformance runner out of CI gating.

Keeps the alpha conformance runner side-channel (``conformance/run_alpha.sh``,
spec ``2026-07-28``) permanently non-gating:

  (i)   ``conformance/runner/package.json`` must pin an exact, non-prerelease
        runner version, and ``conformance/run.sh`` must read that pin instead
        of naming a version of its own.
  (ii)  no file under ``scripts/`` (this guard excluded) may reference the
        alpha results directory (``conformance-results-alpha``) or the alpha
        entrypoint (``run_alpha.sh``).
  (iii) a workflow may reference them only in the job ``ALPHA_JOB`` of
        ``.github/workflows/conformance.yml``. That job must carry
        ``continue-on-error: true`` so its result cannot fail the workflow,
        and no other job may mention it (``needs:`` would make it a
        prerequisite of the gate).

This intentionally does not require ``conformance/run_alpha.sh`` or the job to
exist: it only asserts that nothing *consumes* the alpha runner as a pass/fail
condition. Pattern-based, not presence-based.
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

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
SCRIPTS_DIR = REPO_ROOT / "scripts"

# The one workflow job that may run the alpha runner.
ALPHA_WORKFLOW = "conformance.yml"
ALPHA_JOB = "conformance-2026-07-28-alpha"

# This guard and its tests name the forbidden patterns, so the scan skips both files rather than
# tripping over their docstrings and fixtures.
SELF_PATHS = (
    Path(__file__).resolve(),
    Path(__file__).resolve().with_name("test_check_alpha_runner_guard.py"),
)

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


def iter_files(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return [
        path
        for path in sorted(directory.rglob("*"))
        if path.is_file() and path.resolve() not in SELF_PATHS
    ]


def job_blocks(workflow: str) -> dict[str, tuple[int, int]]:
    """Maps each job id of a workflow to its 1-based first and last line."""
    lines = workflow.splitlines()
    starts: list[tuple[str, int]] = []
    in_jobs = False
    for number, line in enumerate(lines, start=1):
        if re.match(r"^\s*(#.*)?$", line):
            continue
        if re.match(r"^\S", line):
            in_jobs = line.startswith("jobs:")
            continue
        match = re.match(r"^  ([A-Za-z0-9_-]+):\s*(#.*)?$", line)
        if in_jobs and match:
            starts.append((match.group(1), number))
    blocks: dict[str, tuple[int, int]] = {}
    for index, (name, first) in enumerate(starts):
        last = starts[index + 1][1] - 1 if index + 1 < len(starts) else len(lines)
        blocks[name] = (first, last)
    return blocks


def check_workflow(path: Path, label: str) -> list[str]:
    """Returns the violations of one workflow file."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    allowed = None
    if path.name == ALPHA_WORKFLOW:
        allowed = job_blocks(text).get(ALPHA_JOB)
    violations: list[str] = []
    for number, line in enumerate(lines, start=1):
        in_alpha_job = allowed is not None and allowed[0] <= number <= allowed[1]
        if FORBIDDEN_PATTERN.search(line) and not in_alpha_job:
            violations.append(f"{label}:{number}: {line.strip()!r}")
        if allowed is not None and not in_alpha_job and re.search(rf"\b{re.escape(ALPHA_JOB)}\b", line):
            violations.append(f"{label}:{number}: mentions the {ALPHA_JOB} job outside it: {line.strip()!r}")
    if allowed is not None:
        block = lines[allowed[0] : allowed[1]]
        if not any(re.fullmatch(r"    continue-on-error:\s*true\s*(#.*)?", line) for line in block):
            violations.append(
                f"{label}:{allowed[0]}: job {ALPHA_JOB} must set continue-on-error: true at the job level"
            )
    return violations


def check_no_gating_reference(
    workflows_dir: Path = WORKFLOWS_DIR, scripts_dir: Path = SCRIPTS_DIR
) -> None:
    violations: list[str] = []
    for path in iter_files(workflows_dir):
        try:
            violations.extend(check_workflow(path, f"{workflows_dir.name}/{path.name}"))
        except (UnicodeDecodeError, OSError):
            continue
    for path in iter_files(scripts_dir):
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(text.splitlines(), start=1):
            if FORBIDDEN_PATTERN.search(line):
                violations.append(f"{scripts_dir.name}/{path.name}:{number}: {line.strip()!r}")

    if violations:
        joined = "\n  ".join(violations)
        raise GuardError(
            "the alpha runner or its results directory is referenced where it could gate CI:\n  "
            f"{joined}\n"
            f"Only the {ALPHA_JOB} job of .github/workflows/{ALPHA_WORKFLOW} may run it, with "
            "continue-on-error: true, and no other job may depend on that job. Scripts under "
            "scripts/ must not reference it; the alpha harness itself (conformance/run_alpha.sh) "
            "legitimately owns these strings and is not scanned."
        )


def main() -> int:
    pin = check_pin()
    check_no_gating_reference()
    print(
        f"alpha-runner guard passed: {RUN_SH.relative_to(REPO_ROOT)} reads the "
        f"{pin} pin from {PACKAGE_JSON.relative_to(REPO_ROOT)}; the alpha runner/results are "
        "referenced only by the non-gating job"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GuardError as error:
        print(f"alpha-runner guard failed: {error}", file=sys.stderr)
        raise SystemExit(1)
