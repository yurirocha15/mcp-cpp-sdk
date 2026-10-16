#!/usr/bin/env python3
"""Summarize failed resource-collector audits for benchmark manifests."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def _failure_detail(audit: dict[str, Any]) -> str:
    fatal_error = audit.get("fatal_error")
    if isinstance(fatal_error, str) and fatal_error:
        return fatal_error

    failures = audit.get("failures")
    if isinstance(failures, list):
        for failure in reversed(failures):
            if isinstance(failure, dict):
                error = failure.get("error")
                if isinstance(error, str) and error:
                    return error

    termination = audit.get("termination")
    if isinstance(termination, dict):
        reason = termination.get("reason")
        if isinstance(reason, str) and reason:
            return f"collector terminated: {reason}"
    return "collector audit did not include an error"


def summarize_failures(audit_paths: list[Path]) -> str:
    """Return one stable line describing every non-complete collector audit."""
    reasons: list[str] = []
    for audit_path in audit_paths:
        try:
            audit = json.loads(audit_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            reasons.append(f"{audit_path.name}: unavailable audit ({error})")
            continue

        if not isinstance(audit, dict):
            reasons.append(f"{audit_path.name}: collector audit is not an object")
            continue
        if audit.get("status") == "complete":
            continue

        target = audit.get("target")
        if not isinstance(target, str) or not target:
            target = audit_path.name
        reasons.append(f"{target}: {_failure_detail(audit)}")
    return "; ".join(reasons)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("audits", nargs="+", type=Path)
    args = parser.parse_args()

    summary = summarize_failures(args.audits)
    if not summary:
        return 1
    print(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
