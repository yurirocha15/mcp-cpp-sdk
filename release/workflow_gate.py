#!/usr/bin/env python3
"""Pure release-job result gates used by the dispatch workflow."""

from __future__ import annotations

import argparse
from typing import Sequence


class WorkflowGateError(ValueError):
    """Raised when a workflow result graph differs from the required state."""


def require_publishing_enabled(value: str) -> None:
    """Fail closed unless the protected publication kill switch is exact."""

    if value != "true":
        raise WorkflowGateError("RELEASE_PUBLISHING_ENABLED must be exactly true")


def require_package_validation(
    *,
    release_kind: str,
    anchor_exists: bool,
    homebrew_result: str,
    conan_linux_result: str,
    conan_windows_result: str,
    aur_result: str,
) -> None:
    """Require exact package-manager behavior before a stable anchor exists."""
    if release_kind not in {"stable", "rc"}:
        raise WorkflowGateError("release kind is malformed")
    expected = "success" if release_kind == "stable" and not anchor_exists else "skipped"
    observed = (homebrew_result, conan_linux_result, conan_windows_result, aur_result)
    if observed != (expected,) * 4:
        raise WorkflowGateError("pre-anchor package validation has an unexpected result")


def _boolean(value: str) -> bool:
    if value not in {"true", "false"}:
        raise argparse.ArgumentTypeError("expected true or false")
    return value == "true"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    publishing = commands.add_parser("publishing")
    publishing.add_argument("--enabled", required=True)
    packages = commands.add_parser("packages")
    packages.add_argument("--release-kind", required=True)
    packages.add_argument("--anchor-exists", type=_boolean, required=True)
    packages.add_argument("--homebrew-result", required=True)
    packages.add_argument("--conan-linux-result", required=True)
    packages.add_argument("--conan-windows-result", required=True)
    packages.add_argument("--aur-result", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.command == "publishing":
            require_publishing_enabled(args.enabled)
        elif args.command == "packages":
            require_package_validation(
                release_kind=args.release_kind,
                anchor_exists=args.anchor_exists,
                homebrew_result=args.homebrew_result,
                conan_linux_result=args.conan_linux_result,
                conan_windows_result=args.conan_windows_result,
                aur_result=args.aur_result,
            )
        else:
            raise AssertionError(f"unhandled workflow gate: {args.command}")
    except (OSError, WorkflowGateError) as error:
        raise SystemExit(f"workflow-gate: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
