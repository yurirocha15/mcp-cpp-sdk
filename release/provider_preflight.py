#!/usr/bin/env python3
"""Read-only preflights for GitHub App-backed release providers."""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from pathlib import PurePosixPath
import re
import subprocess
import sys
from typing import Mapping, Sequence
from urllib.parse import quote, urlencode

from .artifacts import load_conan_requirements
from .github_provider import (
    ApiRequest,
    GhApi,
    GitHubProviderError,
    RepositoryTarget,
    validate_repository,
    validate_workflow,
)
from .model import ValidationError
from .publication_contract import load_publication_contract, require_destination


_SHA1 = re.compile(r"[0-9a-f]{40}")
_CONTROL_TAG = re.compile(r"release-control-v[1-9][0-9]*")
_SAFE_FOLDER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+-]{0,99}")
_CONAN_UPSTREAM = "conan-io/conan-center-index"
_CONAN_BRANCH = "master"
_MAX_CONFIG_BYTES = 2 * 1024 * 1024
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
    files: dict[str, Path]
    exact_directories: tuple[str, ...]


@dataclass(frozen=True)
class ControlManifest:
    """Reviewed local inventory and immutable release tag for one provider."""

    repository: str
    tag: str
    files: dict[str, Path]
    exact_directories: tuple[str, ...]


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


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ProviderPreflightError(
                f"provider control manifest contains duplicate field: {key}"
            )
        result[key] = value
    return result


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


def _branch_head(api: ApiRequest, repository: str, branch: str) -> str:
    ref = _mapping(
        api("GET", f"repos/{repository}/git/ref/heads/{branch}"),
        f"{repository} default branch ref",
    )
    ref_object = _mapping(ref.get("object"), f"{repository} default branch ref object")
    if (
        ref.get("ref") != f"refs/heads/{branch}"
        or ref_object.get("type") != "commit"
    ):
        raise ProviderPreflightError(f"{repository} default branch is not an exact commit ref")
    return _sha(ref_object.get("sha"), f"{repository} default branch head")


def _decode_contents(value: object, *, expected_path: str) -> str:
    item = _mapping(value, f"ConanCenter content {expected_path}")
    if (
        item.get("type") != "file"
        or item.get("path") != expected_path
        or item.get("encoding") != "base64"
    ):
        raise ProviderPreflightError("ConanCenter dependency config identity is malformed")
    content = item.get("content")
    if not isinstance(content, str):
        raise ProviderPreflightError("ConanCenter dependency config has no encoded content")
    try:
        raw = base64.b64decode("".join(content.splitlines()), validate=True)
        text = raw.decode("utf-8")
    except (ValueError, UnicodeDecodeError) as error:
        raise ProviderPreflightError(
            "ConanCenter dependency config is not canonical base64 UTF-8"
        ) from error
    if not raw or len(raw) > _MAX_CONFIG_BYTES or "\r" in text or "\0" in text:
        raise ProviderPreflightError("ConanCenter dependency config size or encoding is unsafe")
    return text


def _git_blob_sha(content: bytes) -> str:
    header = f"blob {len(content)}\0".encode("ascii")
    return hashlib.sha1(header + content, usedforsecurity=False).hexdigest()


def _control_manifest(path: Path, provider: str) -> ControlManifest:
    if provider not in _CONTROL_PROVIDERS:
        raise ProviderPreflightError("unknown downstream control provider")
    try:
        root = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_strict_pairs
        )
    except json.JSONDecodeError as error:
        raise ProviderPreflightError("provider control manifest is malformed JSON") from error
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
        "repository",
        "control_tag",
        "files",
        "exact_directories",
    }:
        raise ProviderPreflightError("provider control entry schema is not exact")
    repository = entry.get("repository")
    control_tag = entry.get("control_tag")
    if (
        not isinstance(repository, str)
        or len(repository.split("/")) != 2
        or any(_SAFE_FOLDER.fullmatch(part) is None for part in repository.split("/"))
        or not isinstance(control_tag, str)
        or _CONTROL_TAG.fullmatch(control_tag) is None
    ):
        raise ProviderPreflightError("provider control repository or tag is not canonical")
    files = entry.get("files")
    directories = entry.get("exact_directories")
    if not isinstance(files, dict) or not files or not isinstance(directories, list):
        raise ProviderPreflightError("provider control inventory is missing")
    if list(files) != sorted(files) or len(set(files.values())) != len(files):
        raise ProviderPreflightError("provider control file inventory is noncanonical")
    resolved: dict[str, Path] = {}
    source_root = path.resolve().parents[1]
    for remote_path, local_path in files.items():
        if not isinstance(remote_path, str) or not isinstance(local_path, str):
            raise ProviderPreflightError("provider control path is malformed")
        remote = PurePosixPath(remote_path)
        local = PurePosixPath(local_path)
        if (
            remote.is_absolute()
            or local.is_absolute()
            or ".." in remote.parts
            or ".." in local.parts
            or str(remote) != remote_path
            or str(local) != local_path
        ):
            raise ProviderPreflightError("provider control path is unsafe")
        source = source_root.joinpath(*local.parts)
        if source.is_symlink() or not source.is_file() or source.stat().st_size > _MAX_CONFIG_BYTES:
            raise ProviderPreflightError(f"provider control source is missing or unsafe: {local_path}")
        resolved[remote_path] = source
    if any(not isinstance(item, str) for item in directories):
        raise ProviderPreflightError("provider exact-directory inventory is noncanonical")
    normalized_directories = tuple(PurePosixPath(item) for item in directories)
    if (
        not directories
        or len(set(directories)) != len(directories)
        or tuple(sorted(directories)) != tuple(directories)
        or any(
            directory.is_absolute()
            or (not directory.parts and directory != PurePosixPath("."))
            or ".." in directory.parts
            or str(directory) != original
            for directory, original in zip(normalized_directories, directories, strict=True)
        )
        or any(
            not any(PurePosixPath(remote).parent == directory for remote in resolved)
            for directory in normalized_directories
        )
        or any(
            PurePosixPath(remote).parent not in normalized_directories
            for remote in resolved
        )
    ):
        raise ProviderPreflightError("provider exact-directory inventory is noncanonical")
    return ControlManifest(
        repository=repository,
        tag=control_tag,
        files=resolved,
        exact_directories=tuple(directories),
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
) -> ControlSnapshot:
    """Bind every executable downstream control file to the reviewed source snapshot."""

    manifest = _control_manifest(manifest_path, provider)
    commit = _verify_immutable_control_release(
        api, target=target, manifest=manifest
    )
    for directory in manifest.exact_directories:
        endpoint_directory = "" if directory == "." else directory
        listing = api(
            "GET",
            f"repos/{target.full_name}/contents/{endpoint_directory}?"
            + urlencode({"ref": commit}),
        )
        if not isinstance(listing, list) or any(not isinstance(item, Mapping) for item in listing):
            raise ProviderPreflightError(f"downstream control directory is malformed: {directory}")
        actual = sorted(
            item.get("path")
            for item in listing
            if item.get("type") == "file" and isinstance(item.get("path"), str)
        )
        expected = sorted(
            remote_path
            for remote_path in manifest.files
            if str(PurePosixPath(remote_path).parent) == directory
        )
        if actual != expected:
            raise ProviderPreflightError(
                f"downstream control directory differs from reviewed snapshot: {directory}"
            )
    for remote_path, source in sorted(manifest.files.items()):
        value = _mapping(
            api(
                "GET",
                f"repos/{target.full_name}/contents/{remote_path}?"
                + urlencode({"ref": commit}),
            ),
            f"downstream control {remote_path}",
        )
        content = value.get("content")
        if (
            value.get("type") != "file"
            or value.get("path") != remote_path
            or value.get("encoding") != "base64"
            or not isinstance(content, str)
        ):
            raise ProviderPreflightError(f"downstream control identity is malformed: {remote_path}")
        try:
            remote = base64.b64decode("".join(content.splitlines()), validate=True)
        except ValueError as error:
            raise ProviderPreflightError(
                f"downstream control is not canonical base64: {remote_path}"
            ) from error
        local = source.read_bytes()
        if remote != local or value.get("sha") != _git_blob_sha(local):
            raise ProviderPreflightError(
                f"downstream control differs from reviewed snapshot: {remote_path}"
            )
    return ControlSnapshot(
        repository=manifest.repository,
        tag=manifest.tag,
        commit=commit,
        files=manifest.files,
        exact_directories=manifest.exact_directories,
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
        files={},
        exact_directories=tuple(control["exact_directories"]),  # type: ignore[arg-type]
    )
    commit = _verify_immutable_control_release(api, target=target, manifest=manifest)
    if commit != control["control_commit"]:
        raise ProviderPreflightError(
            "signed provider control commit differs from immutable release"
        )
    signed_files = control["files"]
    if not isinstance(signed_files, Mapping):
        raise ProviderPreflightError("signed provider control file inventory is malformed")
    for directory in manifest.exact_directories:
        endpoint_directory = "" if directory == "." else directory
        listing = api(
            "GET", f"repos/{target.full_name}/contents/{endpoint_directory}?" + urlencode({"ref": commit})
        )
        if not isinstance(listing, list) or any(not isinstance(item, Mapping) for item in listing):
            raise ProviderPreflightError(
                f"signed provider control directory is malformed: {directory}"
            )
        actual = sorted(
            item.get("path")
            for item in listing
            if item.get("type") == "file" and isinstance(item.get("path"), str)
        )
        expected = sorted(
            path
            for path in signed_files
            if str(PurePosixPath(path).parent) == directory
        )
        if actual != expected:
            raise ProviderPreflightError(
                f"signed provider control directory differs from anchor: {directory}"
            )
    for remote_path, expected_sha in sorted(signed_files.items()):
        value = _mapping(
            api(
                "GET",
                f"repos/{target.full_name}/contents/{remote_path}?"
                + urlencode({"ref": commit}),
            ),
            f"signed provider control {remote_path}",
        )
        if (
            value.get("type") != "file"
            or value.get("path") != remote_path
            or value.get("sha") != expected_sha
        ):
            raise ProviderPreflightError(
                f"signed provider control differs from anchor: {remote_path}"
            )
    return ControlSnapshot(
        repository=target.full_name,
        tag=manifest.tag,
        commit=commit,
        files={},
        exact_directories=manifest.exact_directories,
    )


def _dependency_folder(config: str, *, package: str, version: str) -> str:
    pattern = re.compile(
        rf'(?m)^  "{re.escape(version)}":\n    folder: '
        r"([A-Za-z0-9][A-Za-z0-9_.+-]{0,99})$"
    )
    matches = pattern.findall(config)
    if len(matches) != 1 or _SAFE_FOLDER.fullmatch(matches[0]) is None:
        raise ProviderPreflightError(
            f"ConanCenter does not expose exactly one {package}/{version} recipe folder"
        )
    return matches[0]


def preflight_conan_fork(
    api: ApiRequest,
    *,
    owner: str,
    repository: str,
    expected_repository_id: int,
    requirements_path: Path | None = None,
    requirements: Sequence[str] | None = None,
) -> str:
    """Prove the reviewed Conan fork and all pinned recipe dependencies exist."""

    coordinate = RepositoryTarget(owner, repository, expected_repository_id).full_name
    fork = _mapping(api("GET", f"repos/{coordinate}"), "Conan fork repository")
    parent = _mapping(fork.get("parent"), "Conan fork parent")
    parent_id = parent.get("id")
    if (
        fork.get("id") != expected_repository_id
        or fork.get("full_name") != coordinate
        or fork.get("default_branch") != _CONAN_BRANCH
        or fork.get("fork") is not True
        or fork.get("archived") is not False
        or fork.get("disabled") is not False
        or type(parent_id) is not int
        or parent_id < 1
        or parent.get("full_name") != _CONAN_UPSTREAM
    ):
        raise ProviderPreflightError("Conan fork identity differs from reviewed configuration")
    upstream = _mapping(
        api("GET", f"repos/{_CONAN_UPSTREAM}"), "ConanCenter upstream repository"
    )
    if (
        upstream.get("id") != parent_id
        or upstream.get("full_name") != _CONAN_UPSTREAM
        or upstream.get("default_branch") != _CONAN_BRANCH
        or upstream.get("fork") is not False
        or upstream.get("archived") is not False
        or upstream.get("disabled") is not False
    ):
        raise ProviderPreflightError("ConanCenter upstream identity is unavailable or changed")
    fork_head = _branch_head(api, coordinate, _CONAN_BRANCH)
    upstream_head = _branch_head(api, _CONAN_UPSTREAM, _CONAN_BRANCH)
    if fork_head != upstream_head:
        raise ProviderPreflightError("Conan recipe fork is not synchronized to upstream master")

    if (requirements_path is None) == (requirements is None):
        raise ProviderPreflightError("exactly one Conan requirement source is required")
    references = (
        load_conan_requirements(requirements_path)
        if requirements_path is not None
        else tuple(requirements or ())
    )
    for reference in references:
        package, version = reference.split("/", 1)
        config_path = f"recipes/{package}/config.yml"
        endpoint = (
            f"repos/{_CONAN_UPSTREAM}/contents/{config_path}?"
            + urlencode({"ref": upstream_head})
        )
        config = _decode_contents(api("GET", endpoint), expected_path=config_path)
        folder = _dependency_folder(config, package=package, version=version)
        folder_path = f"recipes/{package}/{folder}"
        listing = api(
            "GET",
            f"repos/{_CONAN_UPSTREAM}/contents/{folder_path}?"
            + urlencode({"ref": upstream_head}),
        )
        if (
            not isinstance(listing, list)
            or not listing
            or any(not isinstance(item, Mapping) for item in listing)
        ):
            raise ProviderPreflightError(
                f"ConanCenter dependency recipe folder is unavailable: {reference}"
            )
    return upstream_head


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
    conan = commands.add_parser("conan-fork")
    conan.add_argument("--owner", required=True)
    conan.add_argument("--repository", required=True)
    conan.add_argument("--expected-repository-id", type=int, required=True)
    conan.add_argument("--requirements", type=Path, required=True)
    conan.add_argument("--signed-contract", type=Path)
    conan.add_argument(
        "--policy-source", choices=("current", "embedded"), default="current"
    )
    gate = commands.add_parser("gate")
    for provider in (
        "cloudsmith", "aur", "homebrew", "chocolatey", "conan_fork", "conan_broker"
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
                )
            )
            if args.github_output is not None:
                with args.github_output.open("a", encoding="ascii") as output:
                    output.write(f"control_tag={snapshot.tag}\n")
                    output.write(f"control_sha={snapshot.commit}\n")
        elif args.command == "conan-fork":
            if args.policy_source == "embedded" and args.signed_contract is None:
                raise ProviderPreflightError(
                    "Conan policy source and signed contract disagree"
                )
            embedded_contract = (
                load_publication_contract(args.signed_contract)
                if args.policy_source == "embedded"
                else None
            )
            if embedded_contract is not None:
                require_destination(
                    args.signed_contract,
                    name="conan_fork",
                    observed={
                        "repository": f"{args.owner}/{args.repository}",
                        "repository_id": str(args.expected_repository_id),
                        "upstream": _CONAN_UPSTREAM,
                    },
                )
            preflight_conan_fork(
                GhApi(),
                owner=args.owner,
                repository=args.repository,
                expected_repository_id=args.expected_repository_id,
                requirements_path=(
                    args.requirements if args.policy_source == "current" else None
                ),
                requirements=(
                    embedded_contract.conan_requirements
                    if embedded_contract is not None
                    else None
                ),
            )
        else:
            providers = (
                "cloudsmith", "aur", "homebrew", "chocolatey", "conan_fork", "conan_broker"
            )
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
