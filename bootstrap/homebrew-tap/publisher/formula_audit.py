#!/usr/bin/env python3
"""Run Homebrew's formula checks with one explicit empty-tap bootstrap state."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Sequence
import os
from pathlib import Path
import subprocess
import sys


Runner = Callable[..., subprocess.CompletedProcess[object]]


def audit(
    formula: Path,
    *,
    event_name: str,
    ref_name: str,
    runner: Runner = subprocess.run,
) -> bool:
    """Audit a formula, or return ``False`` for the one allowed empty-tap state."""

    if not formula.is_file() or formula.is_symlink():
        if event_name == "push" and ref_name == "main" and not formula.exists():
            return False
        raise ValueError(f"{formula.as_posix()} is required outside initial main bootstrap")
    commands: Sequence[Sequence[str]] = (
        ("brew", "style", formula.as_posix()),
        ("brew", "audit", "--strict", formula.as_posix()),
    )
    for command in commands:
        runner(command, check=True, timeout=600)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--formula", type=Path, required=True)
    args = parser.parse_args()
    try:
        checked = audit(
            args.formula,
            event_name=os.environ["GITHUB_EVENT_NAME"],
            ref_name=os.environ["GITHUB_REF_NAME"],
        )
        if not checked:
            print("Initial tap bootstrap: formula publication is release-controlled.")
    except (KeyError, OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"formula-audit: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
