#!/usr/bin/env python3
"""Read-only preflights for GitHub App-backed release providers."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys
from typing import Mapping, Sequence
from urllib.parse import quote

from .github_repository import (
    ApiRequest,
    GhApi,
    GitHubProviderError,
    RepositoryTarget,
    validate_repository,
    validate_workflow,
)
from .model import ValidationError
from .publication_contract import load_publication_contract
from .strict_json import load_strict


_SHA1 = re.compile(r"[0-9a-f]{40}")
_CONTROL_TAG = re.compile(r"release-control-v[1-9][0-9]*")
_SAFE_FOLDER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+-]{0,99}")
_CONTROL_PROVIDERS = frozenset({"homebrew", "chocolatey", "conan"})
_JOB_RESULTS = frozenset(
    {"success", "failure", "cancelled", "skipped", "timed_out", "action_required"}
)


class ProviderPreflightError(ValueError):
    """Raised when a provider is unavailable or differs from reviewed state."""


@dataclass(frozen=True)
class ControlSnapshot:
    """Exact immutable downstream control release reviewed by this repository."""

    repository: str
    tag: str
    commit: str
    workflow: str


@dataclass(frozen=True)
class ControlManifest:
    """Reviewed local inventory and immutable release tag for one provider."""

    repository: str
    tag: str
    workflow: str


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


def _mapping(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise ProviderPreflightError(f"{label} is not a JSON object")
    return value


def _sha(value: object, label: str) -> str:
    if not isinstance(value, str) or _SHA1.fullmatch(value) is None:
        raise ProviderPreflightError(f"{label} is not a lowercase full SHA")
    return value


def preflight_workflow(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    workflow: str,
) -> int:
    """Prove read access and the exact active workflow on protected ``main``."""

    validate_repository(api, target)
    return validate_workflow(api, target, workflow)


def _control_manifest(path: Path, provider: str) -> ControlManifest:
    if provider not in _CONTROL_PROVIDERS:
        raise ProviderPreflightError("unknown downstream control provider")
    try:
        root = load_strict(path, label="provider control manifest")
    except ValidationError as error:
        raise ProviderPreflightError(str(error)) from error
    if not isinstance(root, dict) or set(root) != {"schema_version", "providers"}:
        raise ProviderPreflightError("provider control manifest schema is not exact")
    providers = root.get("providers")
    if (
        root.get("schema_version") != 1
        or not isinstance(providers, dict)
        or set(providers) != _CONTROL_PROVIDERS
    ):
        raise ProviderPreflightError("provider control manifest identity is invalid")
    entry = providers.get(provider)
    if not isinstance(entry, dict) or set(entry) != {
        "repository", "control_tag", "workflow"
    }:
        raise ProviderPreflightError("provider control entry schema is not exact")
    repository = entry.get("repository")
    control_tag = entry.get("control_tag")
    workflow = entry.get("workflow")
    if (
        not isinstance(repository, str)
        or len(repository.split("/")) != 2
        or any(_SAFE_FOLDER.fullmatch(part) is None for part in repository.split("/"))
        or not isinstance(control_tag, str)
        or _CONTROL_TAG.fullmatch(control_tag) is None
        or workflow != {
            "homebrew": "publish-bottles.yml",
            "chocolatey": "publish.yml",
            "conan": "conan-center-pr.yml",
        }[provider]
    ):
        raise ProviderPreflightError(
            "provider control repository, tag, or workflow is not canonical"
        )
    return ControlManifest(
        repository=repository,
        tag=control_tag,
        workflow=workflow,
    )


def _verify_immutable_control_release(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    manifest: ControlManifest,
) -> str:
    if manifest.repository != target.full_name:
        raise ProviderPreflightError(
            "provider control manifest repository identity differs from configuration"
        )
    ref = _mapping(
        api("GET", f"repos/{target.full_name}/git/ref/tags/{manifest.tag}"),
        "provider control tag",
    )
    ref_object = _mapping(ref.get("object"), "provider control tag object")
    if (
        ref.get("ref") != f"refs/tags/{manifest.tag}"
        or ref_object.get("type") != "commit"
    ):
        raise ProviderPreflightError(
            "provider control tag is not a direct immutable commit reference"
        )
    commit = _sha(ref_object.get("sha"), "provider control tag commit")
    release = _mapping(
        api(
            "GET",
            f"repos/{target.full_name}/releases/tags/"
            + quote(manifest.tag, safe=""),
        ),
        "provider control release",
    )
    assets = release.get("assets")
    if (
        type(release.get("id")) is not int
        or release["id"] < 1
        or release.get("tag_name") != manifest.tag
        or release.get("target_commitish") != commit
        or release.get("draft") is not False
        or release.get("prerelease") is not False
        or release.get("immutable") is not True
        or assets != []
    ):
        raise ProviderPreflightError(
            "provider control release is not the exact immutable asset-free release"
        )
    return commit


def verify_control_snapshot(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    manifest_path: Path,
    provider: str,
    workflow: str,
) -> ControlSnapshot:
    """Bind a downstream workflow to its immutable reviewed control commit."""

    manifest = _control_manifest(manifest_path, provider)
    if manifest.workflow != workflow:
        raise ProviderPreflightError(
            "provider workflow differs from the reviewed control manifest"
        )
    commit = _verify_immutable_control_release(
        api, target=target, manifest=manifest
    )
    return ControlSnapshot(
        repository=manifest.repository,
        tag=manifest.tag,
        commit=commit,
        workflow=manifest.workflow,
    )


def verify_embedded_control_snapshot(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    contract_path: Path,
    provider: str,
    workflow: str,
) -> ControlSnapshot:
    """Verify a historical provider from its signed immutable-release contract."""

    contract = load_publication_contract(contract_path)
    control = contract.provider_controls.get(provider)
    if control is None:
        raise ProviderPreflightError("signed provider control is missing")
    if control["repository"] != target.full_name:
        raise ProviderPreflightError(
            "signed provider control repository differs from configuration"
        )
    if control["workflow"] != workflow:
        raise ProviderPreflightError(
            "signed provider workflow differs from configured workflow"
        )
    destination_name = {
        "homebrew": "homebrew",
        "chocolatey": "chocolatey",
        "conan": "conan_control",
    }[provider]
    destination = contract.destinations[destination_name]
    if (
        destination["repository"] != target.full_name
        or destination["repository_id"] != str(target.repository_id)
    ):
        raise ProviderPreflightError(
            "signed provider destination differs from configured repository"
        )
    manifest = ControlManifest(
        repository=target.full_name,
        tag=str(control["control_tag"]),
        workflow=workflow,
    )
    commit = _verify_immutable_control_release(api, target=target, manifest=manifest)
    if commit != control["control_commit"]:
        raise ProviderPreflightError(
            "signed provider control commit differs from immutable release"
        )
    return ControlSnapshot(
        repository=target.full_name,
        tag=manifest.tag,
        commit=commit,
        workflow=workflow,
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    workflow = commands.add_parser("workflow")
    workflow.add_argument("--owner", required=True)
    workflow.add_argument("--repository", required=True)
    workflow.add_argument("--expected-repository-id", type=int, required=True)
    workflow.add_argument("--workflow", required=True)
    workflow.add_argument("--controls", type=Path, required=True)
    workflow.add_argument("--signed-contract", type=Path)
    workflow.add_argument(
        "--policy-source", choices=("current", "embedded"), default="current"
    )
    workflow.add_argument("--provider", choices=sorted(_CONTROL_PROVIDERS), required=True)
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
            if args.policy_source == "embedded" and args.signed_contract is None:
                raise ProviderPreflightError(
                    "provider policy source and signed contract disagree"
                )
            snapshot = (
                verify_embedded_control_snapshot(
                    GhApi(),
                    target=target,
                    contract_path=args.signed_contract,
                    provider=args.provider,
                    workflow=args.workflow,
                )
                if args.policy_source == "embedded"
                else verify_control_snapshot(
                    GhApi(),
                    target=target,
                    manifest_path=args.controls,
                    provider=args.provider,
                    workflow=args.workflow,
                )
            )
            if args.github_output is not None:
                with args.github_output.open("a", encoding="ascii") as output:
                    output.write(f"control_sha={snapshot.commit}\n")
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
