#!/usr/bin/env python3
"""Guard checks that keep the alpha conformance runner installed from a lockfile and out of CI gating.

The alpha conformance runner side-channel (``conformance/run_alpha.sh``) runs two requirement
sets, 2026-07-28 and 2025-11-25, and stays advisory:

  (i)   ``conformance/runner/package.json`` must pin an exact, non-prerelease
        runner version, and ``conformance/run.sh`` must read that pin instead
        of naming a version of its own.
  (ii)  ``conformance/runner-alpha/package.json`` must pin one exact version of the runner
        (a prerelease is allowed), ``conformance/runner-alpha/package-lock.json`` must lock
        that same version, and ``conformance/run_alpha.sh`` must read the pin, run the runner
        from the install (no ``npx``), run both requirement sets and pass the baseline
        ``conformance/expected-failures-alpha.yml`` with ``--expected-failures``.
  (iii) the alpha workflow must install the runner with ``npm ci --ignore-scripts`` into
        ``conformance/runner-alpha``.
  (iv)  no file under ``scripts/`` (this guard excluded) may reference the
        alpha results directory (``conformance-results-alpha``) or the alpha
        entrypoint (``run_alpha.sh``).
  (v)   a workflow may reference them only in the job ``ALPHA_JOB`` of
        ``.github/workflows/conformance-alpha.yml``. That job must carry
        ``continue-on-error: true`` so its result cannot fail the workflow,
        and no other job may mention it (``needs:`` would make it a
        prerequisite of the gate).
  (vi)  no other workflow may name ``conformance-alpha.yml``, so none calls it
        with ``uses:`` and makes the required CI result wait on it.
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
EXACT_VERSION = re.compile(r"\d+\.\d+\.\d+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?")

ALPHA_RUN_SH = REPO_ROOT / "conformance" / "run_alpha.sh"
ALPHA_PACKAGE_JSON = REPO_ROOT / "conformance" / "runner-alpha" / "package.json"
ALPHA_LOCKFILE = REPO_ROOT / "conformance" / "runner-alpha" / "package-lock.json"
ALPHA_BASELINE = REPO_ROOT / "conformance" / "expected-failures-alpha.yml"
ALPHA_REQUIREMENT_SETS = ("2026-07-28", "2025-11-25")

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
SCRIPTS_DIR = REPO_ROOT / "scripts"

# The one workflow job that may run the alpha runner.
ALPHA_WORKFLOW = "conformance-alpha.yml"
ALPHA_WORKFLOW_PATH = WORKFLOWS_DIR / ALPHA_WORKFLOW
ALPHA_JOB = "alpha-runner"

# This guard and its tests name the forbidden patterns, so the scan skips both files rather than
# tripping over their docstrings and fixtures.
SELF_PATHS = (
    Path(__file__).resolve(),
    Path(__file__).resolve().with_name("test_check_alpha_runner_guard.py"),
)

FORBIDDEN_PATTERN = re.compile(r"conformance-results-alpha|run_alpha\.sh")
# Outside the alpha workflow, naming it can only be a call (``uses:``) or a trigger on it.
ALPHA_WORKFLOW_PATTERN = re.compile(re.escape(ALPHA_WORKFLOW))
# The job id as a whole word, so `alpha-runner-non-gating` and `mcp-alpha-runner` do not count.
ALPHA_JOB_PATTERN = re.compile(rf"(?<![\w-]){re.escape(ALPHA_JOB)}(?![\w-])")


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


def read_alpha_pin(package_json: Path = ALPHA_PACKAGE_JSON) -> str:
    if not package_json.is_file():
        raise GuardError(f"{package_json} does not exist; the alpha runner needs a pinned install")
    try:
        pin = json.loads(package_json.read_text(encoding="utf-8"))["dependencies"][RUNNER_PACKAGE]
    except (KeyError, TypeError, ValueError) as error:
        raise GuardError(f"{package_json} does not pin {RUNNER_PACKAGE}: {error!r}") from error
    if not isinstance(pin, str) or not EXACT_VERSION.fullmatch(pin):
        raise GuardError(
            f"{package_json} pins {RUNNER_PACKAGE} to {pin!r}; the alpha runner needs one exact "
            "version (no range or tag)"
        )
    return pin


def check_alpha_lockfile(pin: str, lockfile: Path = ALPHA_LOCKFILE) -> None:
    if not lockfile.is_file():
        raise GuardError(f"{lockfile} does not exist; `npm ci` needs the lockfile of the alpha runner")
    try:
        packages = json.loads(lockfile.read_text(encoding="utf-8"))["packages"]
        wanted = packages[""]["dependencies"][RUNNER_PACKAGE]
        locked = packages[f"node_modules/{RUNNER_PACKAGE}"]["version"]
    except (KeyError, TypeError, ValueError) as error:
        raise GuardError(f"{lockfile} does not lock {RUNNER_PACKAGE}: {error!r}") from error
    if wanted != pin or locked != pin:
        raise GuardError(
            f"{lockfile} locks {RUNNER_PACKAGE} {locked!r} for the range {wanted!r}, but package.json "
            f"pins {pin!r}; regenerate the lockfile"
        )


def check_alpha_harness(
    run_alpha_sh: Path = ALPHA_RUN_SH,
    baseline: Path = ALPHA_BASELINE,
) -> None:
    if not run_alpha_sh.is_file():
        raise GuardError(f"{run_alpha_sh} does not exist")
    text = run_alpha_sh.read_text(encoding="utf-8")
    problems: list[str] = []
    if "runner-alpha/package.json" not in text:
        problems.append("does not read conformance/runner-alpha/package.json")
    if "runner-alpha/node_modules/.bin/conformance" not in text:
        problems.append("does not run the runner from conformance/runner-alpha/node_modules")
    if re.search(r"\bnpx\b", text):
        problems.append("fetches the runner with npx instead of the locked install")
    if re.search(r"(?<![\d.])\d+\.\d+\.\d+(?![\d.])", text):
        problems.append("names a runner version of its own")
    for requirement_set in ALPHA_REQUIREMENT_SETS:
        if not re.search(rf"REQUIREMENT_SETS=\([^)]*\b{re.escape(requirement_set)}\b", text):
            problems.append(f"does not run the requirement set {requirement_set}")
    if text.count("--requirements") < 2:
        problems.append("does not pass --requirements to both the server and the client run")
    if text.count("--expected-failures \"$BASELINE\"") < 2:
        problems.append("does not pass the baseline with --expected-failures to both runs")
    if "expected-failures-alpha.yml" not in text:
        problems.append("does not use conformance/expected-failures-alpha.yml")
    if problems:
        raise GuardError(f"{run_alpha_sh.name} " + "; it ".join(problems))
    if not baseline.is_file():
        raise GuardError(f"{baseline} does not exist")


def check_alpha_install_step(workflow: Path = ALPHA_WORKFLOW_PATH) -> None:
    if not workflow.is_file():
        raise GuardError(f"{workflow} does not exist")
    text = workflow.read_text(encoding="utf-8")
    if not re.search(r"npm ci --ignore-scripts --prefix conformance/runner-alpha\b", text):
        raise GuardError(
            f"{workflow.name} does not install the alpha runner with "
            "`npm ci --ignore-scripts --prefix conformance/runner-alpha`"
        )
    if re.search(r"\bnpx\b", text):
        raise GuardError(f"{workflow.name} fetches a package with npx instead of the locked install")


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
        if not in_alpha_job and ALPHA_JOB_PATTERN.search(line):
            violations.append(f"{label}:{number}: mentions the {ALPHA_JOB} job outside it: {line.strip()!r}")
        if path.name != ALPHA_WORKFLOW and ALPHA_WORKFLOW_PATTERN.search(line):
            violations.append(f"{label}:{number}: names {ALPHA_WORKFLOW}, which only it may do: {line.strip()!r}")
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
    alpha_pin = read_alpha_pin()
    check_alpha_lockfile(alpha_pin)
    check_alpha_harness()
    check_alpha_install_step()
    check_no_gating_reference()
    print(
        f"alpha-runner guard passed: {RUN_SH.relative_to(REPO_ROOT)} reads the "
        f"{pin} pin from {PACKAGE_JSON.relative_to(REPO_ROOT)}; the alpha runner {alpha_pin} is "
        f"locked in {ALPHA_LOCKFILE.relative_to(REPO_ROOT)}, installed with npm ci and run for "
        f"{', '.join(ALPHA_REQUIREMENT_SETS)}; the alpha runner/results are referenced only by "
        "the advisory job"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GuardError as error:
        print(f"alpha-runner guard failed: {error}", file=sys.stderr)
        raise SystemExit(1)
