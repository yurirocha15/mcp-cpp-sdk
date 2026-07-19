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
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Callable, Mapping, Protocol, Sequence
from urllib.parse import quote
from uuid import NAMESPACE_URL, uuid5

from .model import SemVer, ValidationError


_REPOSITORY_PART = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}")
_WORKFLOW_FILE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,94}\.ya?ml")
_SHA1 = re.compile(r"[0-9a-f]{40}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POSITIVE_DECIMAL = re.compile(r"[1-9][0-9]*")
_CONTROL_TAG = re.compile(r"release-control-v[1-9][0-9]*")
_CHANNELS = frozenset({"homebrew", "chocolatey", "conan2"})


class GitHubProviderError(ValueError):
    """Raised when live GitHub state does not prove the intended operation."""


class ApiRequest(Protocol):
    """Small injectable boundary used by provider orchestration and tests."""

    def __call__(
        self, method: str, endpoint: str, payload: Mapping[str, object] | None = None
    ) -> object | None: ...


CommandRunner = Callable[[Sequence[str], bytes | None], bytes]


def _run_command(arguments: Sequence[str], stdin: bytes | None) -> bytes:
    return subprocess.run(
        list(arguments),
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        timeout=120,
    ).stdout


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise GitHubProviderError(f"GitHub JSON contains duplicate field: {key}")
        result[key] = value
    return result


class GhApi:
    """Call ``gh api`` without a shell and parse duplicate-free JSON."""

    def __init__(self, runner: CommandRunner = _run_command) -> None:
        self._runner = runner

    def __call__(
        self, method: str, endpoint: str, payload: Mapping[str, object] | None = None
    ) -> object | None:
        if method not in {"GET", "POST"} or not endpoint or endpoint.startswith("/"):
            raise GitHubProviderError("invalid internal GitHub API request")
        arguments = [
            "gh",
            "api",
            "--method",
            method,
            "-H",
            "X-GitHub-Api-Version: 2026-03-10",
            endpoint,
        ]
        stdin = None
        if payload is not None:
            arguments.extend(("--input", "-"))
            stdin = (
                json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n"
            ).encode("utf-8")
        raw = self._runner(arguments, stdin)
        if not raw.strip():
            return None
        try:
            return json.loads(raw.decode("utf-8"), object_pairs_hook=_strict_pairs)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise GitHubProviderError("GitHub returned malformed JSON") from error


@dataclass(frozen=True)
class RepositoryTarget:
    owner: str
    repository: str
    repository_id: int
    default_branch: str = "main"

    def __post_init__(self) -> None:
        for label, value in (("owner", self.owner), ("repository", self.repository)):
            if (
                not isinstance(value, str)
                or _REPOSITORY_PART.fullmatch(value) is None
                or ".." in value
            ):
                raise GitHubProviderError(f"GitHub {label} is not canonical")
        if type(self.repository_id) is not int or self.repository_id < 1:
            raise GitHubProviderError("expected repository ID must be positive")
        if self.default_branch != "main":
            raise GitHubProviderError("publisher workflow must use protected main")

    @property
    def full_name(self) -> str:
        return f"{self.owner}/{self.repository}"


def _mapping(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise GitHubProviderError(f"{label} is not a JSON object")
    return value


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


def validate_repository(api: ApiRequest, target: RepositoryTarget) -> None:
    repository = _mapping(api("GET", f"repos/{target.full_name}"), "repository")
    if repository.get("id") != target.repository_id:
        raise GitHubProviderError("repository numeric identity differs from configuration")
    if repository.get("full_name") != target.full_name:
        raise GitHubProviderError("repository full name differs from configuration")
    if repository.get("default_branch") != target.default_branch:
        raise GitHubProviderError("repository default branch is not protected main")
    if repository.get("archived") is not False or repository.get("disabled") is not False:
        raise GitHubProviderError("publisher repository is archived, disabled, or ambiguous")


def validate_workflow(api: ApiRequest, target: RepositoryTarget, workflow: str) -> int:
    if (
        not isinstance(workflow, str)
        or _WORKFLOW_FILE.fullmatch(workflow) is None
        or ".." in workflow
    ):
        raise GitHubProviderError("publisher workflow filename is not canonical")
    value = _mapping(
        api(
            "GET",
            f"repos/{target.full_name}/actions/workflows/{quote(workflow, safe='')}",
        ),
        "publisher workflow",
    )
    workflow_id = value.get("id")
    if type(workflow_id) is not int or workflow_id < 1:
        raise GitHubProviderError("publisher workflow has no stable numeric identity")
    if value.get("path") != f".github/workflows/{workflow}":
        raise GitHubProviderError("publisher workflow path differs from configuration")
    if value.get("state") != "active":
        raise GitHubProviderError("publisher workflow is not active")
    return workflow_id


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
    formula_pr_number: str | None = None,
    formula_branch: str | None = None,
    formula_head_sha: str | None = None,
    fork_branch: str | None = None,
    fork_head_sha: str | None = None,
    recipe_tree_sha256: str | None = None,
) -> dict[str, str]:
    optional = {
        "formula_pr_number": formula_pr_number,
        "formula_branch": formula_branch,
        "formula_head_sha": formula_head_sha,
        "fork_branch": fork_branch,
        "fork_head_sha": fork_head_sha,
        "recipe_tree_sha256": recipe_tree_sha256,
    }
    allowed_optional = {
        "homebrew": {"formula_pr_number", "formula_branch", "formula_head_sha"},
        "chocolatey": set(),
        "conan2": {"fork_branch", "fork_head_sha", "recipe_tree_sha256"},
    }
    if channel not in allowed_optional:
        raise GitHubProviderError("unsupported publisher channel")
    unexpected = sorted(
        key
        for key, value in optional.items()
        if value is not None and key not in allowed_optional[channel]
    )
    if unexpected:
        raise GitHubProviderError(
            f"unexpected {channel} dispatch fields: {', '.join(unexpected)}"
        )
    version = SemVer.from_tag(source_tag, stable_only=True)
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
    if channel == "homebrew":
        expected_branch = f"release/mcp-cpp-sdk-{source_tag}"
        if formula_branch != expected_branch:
            raise GitHubProviderError("formula branch does not match the source tag")
        common.update(
            {
                "formula_pr_number": _positive_decimal(
                    formula_pr_number or "", "formula pull request number"
                ),
                "formula_branch": formula_branch,
                "formula_head_sha": _full_sha(
                    formula_head_sha or "", "formula head commit"
                ),
            }
        )
    elif channel == "conan2":
        expected = f"package/mcp-cpp-sdk-{version}"
        recovery = re.compile(re.escape(expected) + r"-r[1-9][0-9]*")
        if (
            not isinstance(fork_branch, str)
            or (fork_branch != expected and recovery.fullmatch(fork_branch) is None)
        ):
            raise GitHubProviderError("Conan fork branch does not match the source tag")
        common.update(
            {
                "fork_branch": fork_branch,
                "fork_head_sha": _full_sha(fork_head_sha or "", "Conan fork head"),
                "recipe_tree_sha256": _sha256(
                    recipe_tree_sha256 or "", "Conan recipe tree digest"
                ),
            }
        )
    elif channel != "chocolatey":
        raise GitHubProviderError("unsupported publisher channel")
    return common


def dispatch_workflow(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    workflow: str,
    control_tag: str,
    control_sha: str,
    inputs: Mapping[str, str],
) -> str | None:
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
    ref = _mapping(
        api("GET", f"repos/{target.full_name}/git/ref/tags/{control_tag}"),
        "provider control tag",
    )
    ref_object = _mapping(ref.get("object"), "provider control tag object")
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
    if response is None:
        # GitHub historically returned 204 with no body. Accept that legacy
        # success shape because the mutation has already happened, but expose
        # no downstream run identity.
        return None
    value = _mapping(response, "workflow dispatch response")
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
    return str(run_id)


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
    parser.add_argument("--provider-control-tag", required=True)
    parser.add_argument("--provider-control-sha", required=True)
    parser.add_argument("--github-release-id", required=True)
    parser.add_argument("--source-workflow-run-id", required=True)
    parser.add_argument("--release-manifest-sha256", required=True)
    parser.add_argument("--formula-pr-number")
    parser.add_argument("--formula-branch")
    parser.add_argument("--formula-head-sha")
    parser.add_argument("--fork-branch")
    parser.add_argument("--fork-head-sha")
    parser.add_argument("--recipe-tree-sha256")
    parser.add_argument("--github-output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        inputs = build_dispatch_inputs(
            channel=args.channel,
            source_repository=args.source_repository,
            source_tag=args.source_tag,
            source_commit_sha=args.source_commit_sha,
            source_workflow_head_sha=args.source_workflow_head_sha,
            provider_control_sha=args.provider_control_sha,
            github_release_id=args.github_release_id,
            source_workflow_run_id=args.source_workflow_run_id,
            release_manifest_sha256=args.release_manifest_sha256,
            formula_pr_number=args.formula_pr_number,
            formula_branch=args.formula_branch,
            formula_head_sha=args.formula_head_sha,
            fork_branch=args.fork_branch,
            fork_head_sha=args.fork_head_sha,
            recipe_tree_sha256=args.recipe_tree_sha256,
        )
        downstream_run_id = dispatch_workflow(
            GhApi(),
            target=RepositoryTarget(
                args.owner, args.repository, args.expected_repository_id
            ),
            workflow=args.workflow,
            control_tag=args.provider_control_tag,
            control_sha=args.provider_control_sha,
            inputs=inputs,
        )
        if args.github_output is not None:
            with args.github_output.open("a", encoding="ascii") as output:
                output.write(f"request_uuid={inputs['request_uuid']}\n")
                if downstream_run_id is not None:
                    output.write(f"downstream_workflow_run_id={downstream_run_id}\n")
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
