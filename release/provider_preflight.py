#!/usr/bin/env python3
"""Read-only preflights for GitHub App-backed release providers."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
from typing import Mapping, Sequence

from .github_repository import (
    ApiRequest,
    GhApi,
    GitHubProviderError,
    RepositoryTarget,
    validate_repository,
    validate_workflow,
)
from .model import ValidationError


_JOB_RESULTS = frozenset(
    {"success", "failure", "cancelled", "skipped", "timed_out", "action_required"}
)


class ProviderPreflightError(ValueError):
    """Raised when a provider is unavailable or differs from reviewed state."""


def require_selected_preflights(
    *, targets: Mapping[str, bool], results: Mapping[str, str]
) -> None:
    """Require success for selected providers and an actual skip for all others."""

    if set(targets) != set(results) or not targets:
        raise ProviderPreflightError("provider preflight result inventory is not exact")
    for provider in sorted(targets):
        selected = targets[provider]
        result = results[provider]
        if type(selected) is not bool or result not in _JOB_RESULTS:
            raise ProviderPreflightError(
                f"provider preflight state is malformed: {provider}"
            )
        expected = "success" if selected else "skipped"
        if result != expected:
            raise ProviderPreflightError(
                f"provider preflight {provider} was {result}, expected {expected}"
            )


def preflight_workflow(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    workflow: str,
) -> int:
    """Prove read access and the exact active workflow on protected ``main``."""

    validate_repository(api, target)
    return validate_workflow(api, target, workflow)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    workflow = commands.add_parser("workflow")
    workflow.add_argument("--owner", required=True)
    workflow.add_argument("--repository", required=True)
    workflow.add_argument("--expected-repository-id", type=int, required=True)
    workflow.add_argument("--workflow", required=True)
    workflow.add_argument("--github-output", type=Path)
    gate = commands.add_parser("gate")
    for provider in (
        "cloudsmith", "aur", "homebrew", "chocolatey", "conan"
    ):
        gate.add_argument(f"--target-{provider.replace('_', '-')}", choices=("true", "false"), required=True)
        gate.add_argument(f"--result-{provider.replace('_', '-')}", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "workflow":
            target = RepositoryTarget(
                args.owner, args.repository, args.expected_repository_id
            )
            preflight_workflow(
                GhApi(),
                target=target,
                workflow=args.workflow,
            )
            if args.github_output is not None:
                with args.github_output.open("a", encoding="ascii") as output:
                    output.write(f"repository={target.full_name}\n")
                    output.write(f"repository_id={target.repository_id}\n")
        else:
            providers = ("cloudsmith", "aur", "homebrew", "chocolatey", "conan")
            require_selected_preflights(
                targets={
                    provider: getattr(args, f"target_{provider}") == "true"
                    for provider in providers
                },
                results={
                    provider: getattr(args, f"result_{provider}")
                    for provider in providers
                },
            )
    except (
        GitHubProviderError,
        ProviderPreflightError,
        ValidationError,
        OSError,
        UnicodeError,
        subprocess.SubprocessError,
    ) as error:
        print(f"provider-preflight: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
