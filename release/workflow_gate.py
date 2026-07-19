#!/usr/bin/env python3
"""Pure release-job result gates used by the dispatch workflow."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Sequence


class WorkflowGateError(ValueError):
    """Raised when a workflow result graph differs from the required state."""


def require_publishing_enabled(value: str) -> None:
    """Fail closed unless the protected publication kill switch is exact."""

    if value != "true":
        raise WorkflowGateError("RELEASE_PUBLISHING_ENABLED must be exactly true")


def conan_publication_result(state: str) -> str:
    """Map the only two recipe preparation states to ledger results."""

    results = {
        "prepared": "DISPATCHED_PENDING_REVIEW",
        "identical": "SKIPPED_ALREADY_IDENTICAL",
    }
    try:
        return results[state]
    except KeyError as error:
        raise WorkflowGateError("Conan recipe state is unexpected") from error


def require_preparation(*, mode: str, provider_result: str, publication_result: str) -> None:
    if mode not in {"validate", "publish"}:
        raise WorkflowGateError("release mode is malformed")
    if provider_result != "success":
        raise WorkflowGateError("selected provider preflights did not all succeed")
    expected_publication = "success" if mode == "publish" else "skipped"
    if publication_result != expected_publication:
        raise WorkflowGateError("publication kill-switch gate has an unexpected result")


def require_aur_validation(
    *, release_kind: str, anchor_exists: bool, validation_result: str
) -> None:
    if release_kind not in {"stable", "rc"}:
        raise WorkflowGateError("release kind is malformed")
    expected = "success" if release_kind == "stable" and not anchor_exists else "skipped"
    if validation_result != expected:
        raise WorkflowGateError("AUR construction validation has an unexpected result")


def require_package_validation(
    *,
    release_kind: str,
    anchor_exists: bool,
    homebrew_result: str,
    conan_linux_result: str,
    conan_windows_result: str,
) -> None:
    """Require exact package-manager behavior before a stable anchor exists."""
    if release_kind not in {"stable", "rc"}:
        raise WorkflowGateError("release kind is malformed")
    expected = "success" if release_kind == "stable" and not anchor_exists else "skipped"
    observed = (homebrew_result, conan_linux_result, conan_windows_result)
    if observed != (expected, expected, expected):
        raise WorkflowGateError("pre-anchor package validation has an unexpected result")


def require_validation_completion(
    *,
    anchor_exists: bool,
    release_kind: str,
    candidate_result: str,
    abi_result: str,
    aur_gate_result: str,
    package_gate_result: str,
) -> None:
    if release_kind not in {"stable", "rc"}:
        raise WorkflowGateError("release kind is malformed")
    expected_candidate = "skipped" if anchor_exists else "success"
    expected_abi = "success" if release_kind == "stable" and not anchor_exists else "skipped"
    if candidate_result != expected_candidate:
        raise WorkflowGateError("signed candidate validation has an unexpected result")
    if abi_result != expected_abi:
        raise WorkflowGateError("ABI compatibility validation has an unexpected result")
    if aur_gate_result != "success":
        raise WorkflowGateError("AUR validation gate did not succeed")
    if package_gate_result != "success":
        raise WorkflowGateError("package-manager validation gate did not succeed")


def _boolean(value: str) -> bool:
    if value not in {"true", "false"}:
        raise argparse.ArgumentTypeError("expected true or false")
    return value == "true"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    publishing = commands.add_parser("publishing")
    publishing.add_argument("--enabled", required=True)
    conan = commands.add_parser("conan-result")
    conan.add_argument("--state", required=True)
    conan.add_argument("--github-output", type=Path, required=True)
    preparation = commands.add_parser("preparation")
    preparation.add_argument("--mode", required=True)
    preparation.add_argument("--provider-result", required=True)
    preparation.add_argument("--publication-result", required=True)
    aur = commands.add_parser("aur")
    aur.add_argument("--release-kind", required=True)
    aur.add_argument("--anchor-exists", type=_boolean, required=True)
    aur.add_argument("--validation-result", required=True)
    packages = commands.add_parser("packages")
    packages.add_argument("--release-kind", required=True)
    packages.add_argument("--anchor-exists", type=_boolean, required=True)
    packages.add_argument("--homebrew-result", required=True)
    packages.add_argument("--conan-linux-result", required=True)
    packages.add_argument("--conan-windows-result", required=True)
    validation = commands.add_parser("validation")
    validation.add_argument("--anchor-exists", type=_boolean, required=True)
    validation.add_argument("--release-kind", required=True)
    validation.add_argument("--candidate-result", required=True)
    validation.add_argument("--abi-result", required=True)
    validation.add_argument("--aur-gate-result", required=True)
    validation.add_argument("--package-gate-result", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.command == "publishing":
            require_publishing_enabled(args.enabled)
        elif args.command == "conan-result":
            result = conan_publication_result(args.state)
            with args.github_output.open("a", encoding="utf-8") as output:
                output.write(f"result={result}\n")
        elif args.command == "preparation":
            require_preparation(
                mode=args.mode,
                provider_result=args.provider_result,
                publication_result=args.publication_result,
            )
        elif args.command == "aur":
            require_aur_validation(
                release_kind=args.release_kind,
                anchor_exists=args.anchor_exists,
                validation_result=args.validation_result,
            )
        elif args.command == "packages":
            require_package_validation(
                release_kind=args.release_kind,
                anchor_exists=args.anchor_exists,
                homebrew_result=args.homebrew_result,
                conan_linux_result=args.conan_linux_result,
                conan_windows_result=args.conan_windows_result,
            )
        else:
            require_validation_completion(
                anchor_exists=args.anchor_exists,
                release_kind=args.release_kind,
                candidate_result=args.candidate_result,
                abi_result=args.abi_result,
                aur_gate_result=args.aur_gate_result,
                package_gate_result=args.package_gate_result,
            )
    except (OSError, WorkflowGateError) as error:
        raise SystemExit(f"workflow-gate: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
