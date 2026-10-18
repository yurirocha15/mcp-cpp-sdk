#!/usr/bin/env python3
"""Strict GitHub API adapter for isolated downstream release publishers.

The adapter deliberately accepts no credential argument.  The ``gh`` process
obtains its short-lived GitHub App token from ``GH_TOKEN`` in the job
environment, keeping credentials out of command lines, JSON payloads, and
test fixtures.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys
from typing import Mapping, Sequence
from urllib.parse import quote
from uuid import NAMESPACE_URL, uuid5

from .github_repository import (
    ApiRequest,
    GhApi,
    GitHubProviderError,
    RepositoryTarget,
    mapping,
    validate_repository,
    validate_workflow,
)
from .model import SemVer, ValidationError
from .publication_contract import load_publication_contract


_SHA1 = re.compile(r"[0-9a-f]{40}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POSITIVE_DECIMAL = re.compile(r"[1-9][0-9]*")
_CONTROL_TAG = re.compile(r"release-control-v[1-9][0-9]*")
_CHANNELS = frozenset({"homebrew", "chocolatey", "conan2"})
_CONTRACT_CHANNELS = {
    "homebrew": ("homebrew", "homebrew"),
    "chocolatey": ("chocolatey", "chocolatey"),
    "conan2": ("conan", "conan_control"),
}


@dataclass(frozen=True)
class DispatchReceipt:
    run_id: str
    run_url: str
    html_url: str


def _positive_decimal(value: str, label: str) -> str:
    if not isinstance(value, str) or _POSITIVE_DECIMAL.fullmatch(value) is None:
        raise GitHubProviderError(f"{label} is not a positive canonical decimal")
    return value


def _full_sha(value: str, label: str) -> str:
    if not isinstance(value, str) or _SHA1.fullmatch(value) is None:
        raise GitHubProviderError(f"{label} is not a lowercase full commit SHA")
    return value


def _sha256(value: str, label: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise GitHubProviderError(f"{label} is not a lowercase SHA-256")
    return value


def deterministic_request_uuid(
    *, source_repository: str, source_tag: str, manifest_sha256: str, channel: str
) -> str:
    if channel not in _CHANNELS:
        raise GitHubProviderError("unsupported publisher channel")
    parts = source_repository.split("/")
    if len(parts) != 2:
        raise GitHubProviderError("source repository is not canonical")
    RepositoryTarget(parts[0], parts[1], 1)
    SemVer.from_tag(source_tag, stable_only=True)
    _sha256(manifest_sha256, "release manifest digest")
    return str(
        uuid5(
            NAMESPACE_URL,
            f"{source_repository}:{source_tag}:{manifest_sha256}:{channel}",
        )
    )


def build_dispatch_inputs(
    *,
    channel: str,
    source_repository: str,
    source_tag: str,
    source_commit_sha: str,
    source_workflow_head_sha: str,
    provider_control_sha: str,
    github_release_id: str,
    source_workflow_run_id: str,
    release_manifest_sha256: str,
) -> dict[str, str]:
    if channel not in _CHANNELS:
        raise GitHubProviderError("unsupported publisher channel")
    SemVer.from_tag(source_tag, stable_only=True)
    common = {
        "source_tag": source_tag,
        "source_commit_sha": _full_sha(source_commit_sha, "source commit"),
        "source_workflow_head_sha": _full_sha(
            source_workflow_head_sha, "source workflow head"
        ),
        "provider_control_sha": _full_sha(
            provider_control_sha, "provider control commit"
        ),
        "github_release_id": _positive_decimal(github_release_id, "GitHub release ID"),
        "source_workflow_run_id": _positive_decimal(
            source_workflow_run_id, "source workflow run ID"
        ),
        "release_manifest_sha256": _sha256(
            release_manifest_sha256, "release manifest digest"
        ),
        "request_uuid": deterministic_request_uuid(
            source_repository=source_repository,
            source_tag=source_tag,
            manifest_sha256=release_manifest_sha256,
            channel=channel,
        ),
    }
    return common


def control_from_contract(
    path: Path,
    *,
    channel: str,
    target: RepositoryTarget,
    workflow: str,
) -> tuple[str, str]:
    """Bind a dispatch to the control identity signed into the GitHub anchor."""

    try:
        provider, destination_name = _CONTRACT_CHANNELS[channel]
    except KeyError as error:
        raise GitHubProviderError("unsupported publisher channel") from error
    contract = load_publication_contract(path)
    control = contract.provider_controls[provider]
    destination = contract.destinations[destination_name]
    if (
        control["repository"] != target.full_name
        or control["workflow"] != workflow
        or destination["repository"] != target.full_name
        or destination["repository_id"] != str(target.repository_id)
    ):
        raise GitHubProviderError(
            "publisher configuration differs from the signed release contract"
        )
    tag = control["control_tag"]
    commit = control["control_commit"]
    if not isinstance(tag, str) or not isinstance(commit, str):
        raise GitHubProviderError("signed provider control identity is malformed")
    return tag, commit


def dispatch_workflow(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    workflow: str,
    control_tag: str,
    control_sha: str,
    inputs: Mapping[str, str],
) -> DispatchReceipt:
    validate_repository(api, target)
    validate_workflow(api, target, workflow)
    if not isinstance(control_tag, str) or _CONTROL_TAG.fullmatch(control_tag) is None:
        raise GitHubProviderError("provider control tag is not canonical")
    expected_control_sha = _full_sha(control_sha, "provider control commit")
    if not isinstance(inputs, Mapping) or not inputs:
        raise GitHubProviderError("publisher dispatch inputs are missing")
    if any(
        not isinstance(key, str)
        or not key
        or len(key) > 80
        or re.fullmatch(r"[a-z][a-z0-9_]*", key) is None
        or not isinstance(value, str)
        or not value
        or len(value) > 160
        or any(character.isspace() or ord(character) < 0x20 for character in value)
        for key, value in inputs.items()
    ):
        raise GitHubProviderError("publisher dispatch inputs are not canonical strings")
    if inputs.get("provider_control_sha") != expected_control_sha:
        raise GitHubProviderError(
            "publisher dispatch does not carry the preflighted control commit"
        )
    ref = mapping(
        api("GET", f"repos/{target.full_name}/git/ref/tags/{control_tag}"),
        "provider control tag",
    )
    ref_object = mapping(ref.get("object"), "provider control tag object")
    if (
        ref.get("ref") != f"refs/tags/{control_tag}"
        or ref_object.get("type") != "commit"
        or ref_object.get("sha") != expected_control_sha
    ):
        raise GitHubProviderError(
            "provider control tag changed after its reviewed preflight"
        )
    response = api(
        "POST",
        f"repos/{target.full_name}/actions/workflows/{quote(workflow, safe='')}/dispatches",
        {"ref": control_tag, "inputs": dict(inputs)},
    )
    value = mapping(response, "workflow dispatch response")
    if set(value) != {"workflow_run_id", "run_url", "html_url"}:
        raise GitHubProviderError("workflow dispatch response schema is not exact")
    run_id = value.get("workflow_run_id")
    if type(run_id) is not int or run_id < 1:
        raise GitHubProviderError("workflow dispatch run ID is not positive")
    if value.get("run_url") != (
        f"https://api.github.com/repos/{target.full_name}/actions/runs/{run_id}"
    ) or value.get("html_url") != (
        f"https://github.com/{target.full_name}/actions/runs/{run_id}"
    ):
        raise GitHubProviderError("workflow dispatch response URLs are not canonical")
    return DispatchReceipt(
        run_id=str(run_id),
        run_url=str(value["run_url"]),
        html_url=str(value["html_url"]),
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("channel", choices=sorted(_CHANNELS))
    parser.add_argument("--owner", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--expected-repository-id", type=int, required=True)
    parser.add_argument("--workflow", required=True)
    parser.add_argument("--source-repository", required=True)
    parser.add_argument("--source-tag", required=True)
    parser.add_argument("--source-commit-sha", required=True)
    parser.add_argument("--source-workflow-head-sha", required=True)
    parser.add_argument("--contract", type=Path, required=True)
    parser.add_argument("--github-release-id", required=True)
    parser.add_argument("--source-workflow-run-id", required=True)
    parser.add_argument("--release-manifest-sha256", required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        target = RepositoryTarget(
            args.owner, args.repository, args.expected_repository_id
        )
        control_tag, control_sha = control_from_contract(
            args.contract,
            channel=args.channel,
            target=target,
            workflow=args.workflow,
        )
        inputs = build_dispatch_inputs(
            channel=args.channel,
            source_repository=args.source_repository,
            source_tag=args.source_tag,
            source_commit_sha=args.source_commit_sha,
            source_workflow_head_sha=args.source_workflow_head_sha,
            provider_control_sha=control_sha,
            github_release_id=args.github_release_id,
            source_workflow_run_id=args.source_workflow_run_id,
            release_manifest_sha256=args.release_manifest_sha256,
        )
        receipt = dispatch_workflow(
            GhApi(),
            target=target,
            workflow=args.workflow,
            control_tag=control_tag,
            control_sha=control_sha,
            inputs=inputs,
        )
        with args.github_output.open("a", encoding="ascii") as output:
            output.write(f"downstream_run_id={receipt.run_id}\n")
            output.write(f"downstream_html_url={receipt.html_url}\n")
    except (
        GitHubProviderError,
        ValidationError,
        OSError,
        UnicodeError,
        subprocess.SubprocessError,
    ) as error:
        print(f"github-provider: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
