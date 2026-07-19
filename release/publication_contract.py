"""Build and validate the signed, self-contained publication contract."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
from typing import Any, Mapping, Sequence

from .artifacts import (
    CONAN_REFERENCE_RE,
    canonical_json_bytes,
    load_conan_requirements,
    load_native_targets,
)
from .model import ValidationError
from .native_builder import load_builder_lock


CONTRACT_ASSET = "release-publication-contract.json"
SUPPORTED_SCHEMA_VERSIONS = frozenset({1})
_SHA1 = re.compile(r"[0-9a-f]{40}")
_OCI_DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
_CONTROL_TAG = re.compile(r"release-control-v[1-9][0-9]*")
_REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
_CLOUDSMITH_COORDINATE = re.compile(r"[a-z0-9][a-z0-9-]{0,63}")
_WORKFLOWS = {
    "homebrew": "publish-bottles.yml",
    "chocolatey": "publish.yml",
    "conan": "conan-center-pr.yml",
}
_V1_NATIVE_TARGET_IDS = (
    "ubuntu-jammy-amd64", "ubuntu-jammy-arm64",
    "ubuntu-noble-amd64", "ubuntu-noble-arm64",
    "ubuntu-resolute-amd64", "ubuntu-resolute-arm64",
    "debian-bookworm-amd64", "debian-bookworm-arm64",
    "debian-trixie-amd64", "debian-trixie-arm64",
    "fedora-43-x86_64", "fedora-43-aarch64",
    "fedora-44-x86_64", "fedora-44-aarch64",
    "el-9-x86_64", "el-9-aarch64", "el-10-x86_64", "el-10-aarch64",
)
_V1_BUILDER_FIELDS = frozenset({"id", "image", "image_digest"})
_V1_TARGET_BASE_FIELDS = frozenset(
    {"id", "format", "distribution", "release", "architecture", "runner"}
)
_V1_TARGET_APT_FIELDS = _V1_TARGET_BASE_FIELDS | frozenset(
    {
        "builder_os_id", "builder_os_version_id", "builder_os_version_codename",
        "builder_dpkg_architecture", "builder_uname_machine",
    }
)
_V1_TARGET_RPM_FIELDS = _V1_TARGET_BASE_FIELDS | frozenset(
    {
        "builder_os_id", "builder_os_version_id", "builder_rpm_fedora",
        "builder_rpm_rhel", "builder_rpm_dist", "builder_rpm_architecture",
        "builder_uname_machine",
    }
)
_RECIPE_ASSETS = (
    "conan-recipe-config-entry.json",
    "conan-recipe-conandata-entry.json",
    "conan-recipe-conanfile.py",
    "conan-recipe-test-CMakeLists.txt",
    "conan-recipe-test-conanfile.py",
    "conan-recipe-test-test_package.cpp",
    "conan-source.json",
)
_CHANNEL_ASSETS = {
    "apt": ("cloudsmith-routes.json",),
    "rpm": ("cloudsmith-routes.json",),
    "aur": ("aur-PKGBUILD", "aur-SRCINFO"),
    "homebrew": ("homebrew-mcp-cpp-sdk.rb",),
    "chocolatey": (),
    "conan2": _RECIPE_ASSETS,
}
_DISPATCH_INPUTS = {
    "homebrew": (
        "source_tag", "source_commit_sha", "source_workflow_head_sha",
        "provider_control_sha", "github_release_id", "source_workflow_run_id",
        "release_manifest_sha256", "request_uuid", "formula_pr_number",
        "formula_branch", "formula_head_sha",
    ),
    "chocolatey": (
        "source_tag", "source_commit_sha", "source_workflow_head_sha",
        "provider_control_sha", "github_release_id", "source_workflow_run_id",
        "release_manifest_sha256", "request_uuid",
    ),
    "conan2": (
        "source_tag", "source_commit_sha", "source_workflow_head_sha",
        "provider_control_sha", "github_release_id", "source_workflow_run_id",
        "release_manifest_sha256", "request_uuid", "fork_branch",
        "fork_head_sha", "recipe_tree_sha256",
    ),
}


@dataclass(frozen=True)
class PublicationNativeTargetV1:
    """Frozen native target value object for publication-contract schema v1."""

    id: str
    format: str
    distribution: str
    release: str
    architecture: str
    runner: str
    builder_os_id: str
    builder_os_version_id: str
    builder_uname_machine: str
    builder_os_version_codename: str | None = None
    builder_dpkg_architecture: str | None = None
    builder_rpm_fedora: str | None = None
    builder_rpm_rhel: str | None = None
    builder_rpm_dist: str | None = None
    builder_rpm_architecture: str | None = None

    def to_mapping(self) -> dict[str, str]:
        return _native_target_mapping_v1(self)


@dataclass(frozen=True)
class PublicationContract:
    """Normalized schema-v1 policy retained inside an immutable release."""

    native_targets: tuple[PublicationNativeTargetV1, ...]
    native_builders: tuple[Mapping[str, str], ...]
    channel_targets: Mapping[str, object]
    destinations: Mapping[str, Mapping[str, str]]
    conan_requirements: tuple[str, ...]
    provider_controls: Mapping[str, Mapping[str, object]]
    channel_assets: Mapping[str, tuple[str, ...]]
    dispatch_inputs: Mapping[str, tuple[str, ...]]


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValidationError(f"publication contract contains duplicate field: {key}")
        value[key] = item
    return value


def _read_json(path: Path) -> object:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_strict_pairs
        )
    except (json.JSONDecodeError, UnicodeError) as error:
        raise ValidationError(f"publication input is malformed JSON: {path.name}") from error


def _git_blob_sha(content: bytes) -> str:
    header = f"blob {len(content)}\0".encode("ascii")
    return hashlib.sha1(header + content, usedforsecurity=False).hexdigest()


def _provider_snapshots(
    root: Path, controls_path: Path, control_commits: Mapping[str, str]
) -> dict[str, object]:
    controls = _read_json(controls_path)
    if not isinstance(controls, Mapping) or set(controls) != {"schema_version", "providers"}:
        raise ValidationError("provider control source schema is not exact")
    providers = controls.get("providers")
    if controls.get("schema_version") != 1 or not isinstance(providers, Mapping):
        raise ValidationError("provider control source version is unsupported")
    if set(providers) != set(_WORKFLOWS) or set(control_commits) != set(_WORKFLOWS):
        raise ValidationError("provider control source inventory is not exact")
    snapshots: dict[str, object] = {}
    for provider in sorted(_WORKFLOWS):
        entry = providers[provider]
        if not isinstance(entry, Mapping) or set(entry) != {
            "repository", "control_tag", "files", "exact_directories"
        }:
            raise ValidationError("provider control source entry is malformed")
        repository = entry.get("repository")
        control_tag = entry.get("control_tag")
        commit = control_commits[provider]
        files = entry.get("files")
        directories = entry.get("exact_directories")
        if (
            not isinstance(repository, str)
            or _REPOSITORY.fullmatch(repository) is None
            or not isinstance(control_tag, str)
            or _CONTROL_TAG.fullmatch(control_tag) is None
            or _SHA1.fullmatch(commit) is None
            or not isinstance(files, Mapping)
            or not files
            or not isinstance(directories, list)
        ):
            raise ValidationError("provider control source identity is malformed")
        hashes: dict[str, str] = {}
        for remote_path, local_path in files.items():
            if not isinstance(remote_path, str) or not isinstance(local_path, str):
                raise ValidationError("provider control source path is malformed")
            remote = PurePosixPath(remote_path)
            local = PurePosixPath(local_path)
            if (
                remote.is_absolute() or local.is_absolute()
                or ".." in remote.parts or ".." in local.parts
                or str(remote) != remote_path or str(local) != local_path
            ):
                raise ValidationError("provider control source path is unsafe")
            source = root.joinpath(*local.parts)
            if source.is_symlink() or not source.is_file():
                raise ValidationError("provider control source file is missing or unsafe")
            hashes[remote_path] = _git_blob_sha(source.read_bytes())
        if list(hashes) != sorted(hashes) or any(not isinstance(item, str) for item in directories):
            raise ValidationError("provider control source inventory is noncanonical")
        snapshots[provider] = {
            "repository": repository,
            "control_tag": control_tag,
            "control_commit": commit,
            "workflow": _WORKFLOWS[provider],
            "files": hashes,
            "exact_directories": directories,
        }
    return snapshots


def build_publication_contract(
    *,
    root: Path,
    control_commits: Mapping[str, str],
    destinations: Mapping[str, Mapping[str, str]],
) -> dict[str, object]:
    """Capture all versioned publication inputs needed after the anchor is immutable."""

    targets_document = _read_json(root / "packaging/targets.json")
    if not isinstance(targets_document, Mapping):
        raise ValidationError("package target catalog is malformed")
    channel_targets = {
        name: targets_document.get(name)
        for name in ("aur", "homebrew", "chocolatey")
    }
    contract = {
        "schema_version": 1,
        "route_schema_version": 1,
        "recipe_schema_version": 1,
        "dispatch_schema_version": 1,
        "native_targets": [
            target.to_mapping()
            for target in load_native_targets(root / "packaging/native-targets.json")
        ],
        "native_builders": [
            {
                "id": builder["id"],
                "image": builder["image"],
                "image_digest": builder["image_digest"],
            }
            for builder in load_builder_lock(
                root / "release/native-builders/lock.json", require_resolved=True
            )
            if builder["id"] in _V1_NATIVE_TARGET_IDS
        ],
        "channel_targets": channel_targets,
        "destinations": {name: dict(value) for name, value in destinations.items()},
        "conan_requirements": list(
            load_conan_requirements(root / "packaging/conan-center/requirements.json")
        ),
        "provider_controls": _provider_snapshots(
            root, root / "packaging/provider-controls.json", control_commits
        ),
        "channel_assets": {
            channel: sorted(assets) for channel, assets in _CHANNEL_ASSETS.items()
        },
        "dispatch_inputs": {
            channel: sorted(fields) for channel, fields in _DISPATCH_INPUTS.items()
        },
    }
    # Validate what we emit so construction and consumption cannot drift.
    parse_publication_contract(contract)
    return contract


def _canonical_string_list(name: str, value: object) -> tuple[str, ...]:
    if (
        not isinstance(value, list)
        or any(not isinstance(item, str) or not item for item in value)
        or value != sorted(value)
        or len(set(value)) != len(value)
    ):
        raise ValidationError(f"publication contract {name} is noncanonical")
    return tuple(value)


def _parse_native_target_v1(value: object) -> PublicationNativeTargetV1:
    """Parse the frozen v1 target shape independently of current catalogs."""

    if not isinstance(value, Mapping) or value.get("format") not in {"apt", "rpm"}:
        raise ValidationError("publication contract native target is malformed")
    fields = _V1_TARGET_APT_FIELDS if value["format"] == "apt" else _V1_TARGET_RPM_FIELDS
    if set(value) != fields or any(
        not isinstance(value[field], str)
        or (not value[field] and field not in {"builder_rpm_fedora", "builder_rpm_rhel"})
        for field in fields
    ):
        raise ValidationError("publication contract native target is malformed")
    return PublicationNativeTargetV1(
        **{field: value[field] for field in _V1_TARGET_BASE_FIELDS},
        builder_os_id=value["builder_os_id"],
        builder_os_version_id=value["builder_os_version_id"],
        builder_uname_machine=value["builder_uname_machine"],
        builder_os_version_codename=value.get("builder_os_version_codename"),
        builder_dpkg_architecture=value.get("builder_dpkg_architecture"),
        builder_rpm_fedora=value.get("builder_rpm_fedora"),
        builder_rpm_rhel=value.get("builder_rpm_rhel"),
        builder_rpm_dist=value.get("builder_rpm_dist"),
        builder_rpm_architecture=value.get("builder_rpm_architecture"),
    )


def _native_target_mapping_v1(target: PublicationNativeTargetV1) -> dict[str, str]:
    fields = (
        _V1_TARGET_APT_FIELDS if target.format == "apt" else _V1_TARGET_RPM_FIELDS
    )
    return {
        field: getattr(target, field) or ""
        for field in sorted(fields)
    }


def parse_publication_contract(value: object) -> PublicationContract:
    """Parse one supported immutable contract without consulting current catalogs."""

    fields = {
        "schema_version", "route_schema_version", "recipe_schema_version",
        "dispatch_schema_version", "native_targets", "channel_targets", "destinations",
        "native_builders", "conan_requirements", "provider_controls", "channel_assets",
        "dispatch_inputs",
    }
    if not isinstance(value, Mapping) or set(value) != fields:
        raise ValidationError("publication contract schema is not exact")
    if value.get("schema_version") not in SUPPORTED_SCHEMA_VERSIONS or any(
        value.get(field) != 1
        for field in ("route_schema_version", "recipe_schema_version", "dispatch_schema_version")
    ):
        raise ValidationError("publication contract version is unsupported")

    target_values = value.get("native_targets")
    if not isinstance(target_values, list):
        raise ValidationError("publication contract native target inventory is missing")
    targets = tuple(_parse_native_target_v1(item) for item in target_values)
    if (
        [_native_target_mapping_v1(target) for target in targets] != target_values
        or tuple(target.id for target in targets) != _V1_NATIVE_TARGET_IDS
    ):
        raise ValidationError("publication contract native target inventory is noncanonical")

    builder_values = value.get("native_builders")
    if not isinstance(builder_values, list):
        raise ValidationError("publication contract native builder inventory is missing")
    builders: list[Mapping[str, str]] = []
    for builder in builder_values:
        if (
            not isinstance(builder, Mapping)
            or set(builder) != _V1_BUILDER_FIELDS
            or any(not isinstance(builder[field], str) for field in _V1_BUILDER_FIELDS)
            or _OCI_DIGEST.fullmatch(builder["image_digest"]) is None
            or builder["image"]
            != "ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders"
        ):
            raise ValidationError("publication contract native builder is malformed")
        builders.append(dict(builder))
    if tuple(builder["id"] for builder in builders) != _V1_NATIVE_TARGET_IDS:
        raise ValidationError("publication contract native builder inventory is noncanonical")

    channel_targets = value.get("channel_targets")
    if not isinstance(channel_targets, Mapping) or set(channel_targets) != {
        "aur", "homebrew", "chocolatey"
    }:
        raise ValidationError("publication contract channel targets are incomplete")
    expected_channel_targets = {
        "aur": {"package_base": "mcp-cpp-sdk", "architectures": ["x86_64"]},
        "homebrew": {
            "tap": "yurirocha15/homebrew-mcp-cpp-sdk",
            "bottle_tags": ["sequoia", "arm64_sequoia", "x86_64_linux", "arm64_linux"],
        },
        "chocolatey": {
            "package_id": "mcp-cpp-sdk",
            "architecture": "x86_64",
            "toolset": "v143",
            "runtime": "MD",
        },
    }
    if channel_targets != expected_channel_targets:
        raise ValidationError("publication contract channel target schema is not exact")

    destination_fields = {
        "cloudsmith": frozenset(
            {"namespace", "repository", "preflight_username", "publish_username"}
        ),
        "homebrew": frozenset({"repository", "repository_id", "bot_login"}),
        "chocolatey": frozenset({"repository", "repository_id"}),
        "conan_fork": frozenset({"repository", "repository_id", "upstream"}),
        "conan_control": frozenset({"repository", "repository_id"}),
    }
    destination_values = value.get("destinations")
    if not isinstance(destination_values, Mapping) or set(destination_values) != set(destination_fields):
        raise ValidationError("publication contract destination inventory is incomplete")
    normalized_destinations: dict[str, Mapping[str, str]] = {}
    for name, fields in destination_fields.items():
        destination = destination_values[name]
        if (
            not isinstance(destination, Mapping)
            or set(destination) != fields
            or any(
                not isinstance(destination[field], str)
                or not destination[field]
                or destination[field] != destination[field].strip()
                for field in fields
            )
            or (
                "repository_id" in fields
                and re.fullmatch(r"[1-9][0-9]*", destination["repository_id"]) is None
            )
            or (
                name == "cloudsmith"
                and any(
                    _CLOUDSMITH_COORDINATE.fullmatch(destination[field]) is None
                    for field in fields
                )
            )
            or (
                name != "cloudsmith"
                and any(
                    _REPOSITORY.fullmatch(destination[field]) is None
                    for field in fields & {"repository", "upstream"}
                )
            )
        ):
            raise ValidationError("publication contract destination identity is malformed")
        normalized_destinations[name] = dict(destination)

    requirements = _canonical_string_list("Conan requirements", value.get("conan_requirements"))
    if any(CONAN_REFERENCE_RE.fullmatch(reference) is None for reference in requirements):
        raise ValidationError("publication contract Conan requirement is malformed")

    controls = value.get("provider_controls")
    if not isinstance(controls, Mapping) or set(controls) != set(_WORKFLOWS):
        raise ValidationError("publication contract provider controls are incomplete")
    normalized_controls: dict[str, Mapping[str, object]] = {}
    for provider in sorted(_WORKFLOWS):
        control = controls[provider]
        if not isinstance(control, Mapping) or set(control) != {
            "repository", "control_tag", "control_commit", "workflow", "files",
            "exact_directories",
        }:
            raise ValidationError("publication contract provider control is malformed")
        files = control.get("files")
        directories = control.get("exact_directories")
        if (
            not isinstance(control.get("repository"), str)
            or _REPOSITORY.fullmatch(control["repository"]) is None
            or not isinstance(control.get("control_tag"), str)
            or _CONTROL_TAG.fullmatch(control["control_tag"]) is None
            or not isinstance(control.get("control_commit"), str)
            or _SHA1.fullmatch(control["control_commit"]) is None
            or control.get("workflow") != _WORKFLOWS[provider]
            or not isinstance(files, Mapping)
            or not files
            or list(files) != sorted(files)
            or any(
                not isinstance(path, str)
                or not isinstance(digest, str)
                or _SHA1.fullmatch(digest) is None
                for path, digest in files.items()
            )
            or not isinstance(directories, list)
            or directories != sorted(directories)
            or len(set(directories)) != len(directories)
        ):
            raise ValidationError("publication contract provider control identity is malformed")
        normalized_directories = tuple(PurePosixPath(item) for item in directories)
        if (
            any(
                not isinstance(item, str)
                or directory.is_absolute()
                or (not directory.parts and directory != PurePosixPath("."))
                or ".." in directory.parts
                or str(directory) != item
                for item, directory in zip(directories, normalized_directories, strict=True)
            )
            or any(
                PurePosixPath(path).is_absolute()
                or ".." in PurePosixPath(path).parts
                or str(PurePosixPath(path)) != path
                for path in files
            )
            or any(
                not any(PurePosixPath(path).parent == directory for path in files)
                for directory in normalized_directories
            )
            or any(
                PurePosixPath(path).parent not in normalized_directories
                for path in files
            )
        ):
            raise ValidationError("publication contract provider control path is unsafe")
        normalized_controls[provider] = control
    for provider, destination in (
        ("homebrew", "homebrew"),
        ("chocolatey", "chocolatey"),
        ("conan", "conan_control"),
    ):
        if normalized_controls[provider]["repository"] != normalized_destinations[destination]["repository"]:
            raise ValidationError(
                "publication contract provider and destination repositories disagree"
            )

    assets = value.get("channel_assets")
    if not isinstance(assets, Mapping) or set(assets) != set(_CHANNEL_ASSETS):
        raise ValidationError("publication contract channel assets are incomplete")
    normalized_assets = {
        channel: _canonical_string_list(f"{channel} assets", assets[channel])
        for channel in sorted(assets)
    }
    if normalized_assets != {
        channel: tuple(sorted(expected)) for channel, expected in sorted(_CHANNEL_ASSETS.items())
    }:
        raise ValidationError("publication contract channel asset protocol differs from schema v1")

    dispatch = value.get("dispatch_inputs")
    if not isinstance(dispatch, Mapping) or set(dispatch) != set(_DISPATCH_INPUTS):
        raise ValidationError("publication contract dispatch inventory is incomplete")
    normalized_dispatch = {
        channel: _canonical_string_list(f"{channel} dispatch inputs", dispatch[channel])
        for channel in sorted(dispatch)
    }
    if normalized_dispatch != {
        channel: tuple(sorted(expected)) for channel, expected in sorted(_DISPATCH_INPUTS.items())
    }:
        raise ValidationError("publication contract dispatch protocol differs from schema v1")
    return PublicationContract(
        native_targets=targets,
        native_builders=tuple(builders),
        channel_targets=dict(channel_targets),
        destinations=normalized_destinations,
        conan_requirements=requirements,
        provider_controls=normalized_controls,
        channel_assets=normalized_assets,
        dispatch_inputs=normalized_dispatch,
    )


def load_publication_contract(path: Path) -> PublicationContract:
    return parse_publication_contract(_read_json(path))


def require_destination(
    path: Path, *, name: str, observed: Mapping[str, str]
) -> None:
    """Require current provider coordinates to equal the signed release contract."""

    contract = load_publication_contract(path)
    expected = contract.destinations.get(name)
    if expected is None or dict(observed) != dict(expected):
        raise ValidationError(
            f"current {name} destination differs from the signed publication contract"
        )


def require_channel_target(
    path: Path, *, name: str, observed: Mapping[str, object]
) -> None:
    """Require a direct package-channel target to equal the signed contract."""

    contract = load_publication_contract(path)
    target = contract.channel_targets.get(name)
    if not isinstance(target, Mapping):
        raise ValidationError(f"signed publication contract has no {name} target")
    expected: dict[str, object] = dict(target)
    if name == "aur":
        package_base = expected.get("package_base")
        expected["repository"] = (
            f"ssh://aur@aur.archlinux.org/{package_base}.git"
        )
    if dict(observed) != expected:
        raise ValidationError(
            f"current {name} target differs from the signed publication contract"
        )


def write_publication_contract(
    path: Path,
    *,
    root: Path,
    control_commits: Mapping[str, str],
    destinations: Mapping[str, Mapping[str, str]],
) -> None:
    path.write_bytes(
        canonical_json_bytes(
            build_publication_contract(
                root=root,
                control_commits=control_commits,
                destinations=destinations,
            )
        )
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", type=Path, required=True)
    parser.add_argument(
        "--name",
        choices=(
            "aur", "cloudsmith", "homebrew", "chocolatey", "conan_fork",
            "conan_control",
        ),
        required=True,
    )
    parser.add_argument("--namespace")
    parser.add_argument("--repository", required=True)
    parser.add_argument("--repository-id")
    parser.add_argument("--preflight-username")
    parser.add_argument("--publish-username")
    parser.add_argument("--bot-login")
    parser.add_argument("--upstream")
    parser.add_argument("--package-base")
    parser.add_argument("--architecture", action="append")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.name == "aur":
        observed: Mapping[str, object] = {
            "package_base": args.package_base,
            "architectures": args.architecture,
            "repository": args.repository,
        }
        if any(
            not isinstance(value, (str, list)) or not value
            for value in observed.values()
        ):
            raise SystemExit("publication-contract: AUR target arguments are incomplete")
        try:
            require_channel_target(args.contract, name="aur", observed=observed)
        except (OSError, UnicodeError, ValidationError) as error:
            raise SystemExit(f"publication-contract: {error}") from error
        return 0
    if args.name == "cloudsmith":
        observed = {
            "namespace": args.namespace,
            "repository": args.repository,
            "preflight_username": args.preflight_username,
            "publish_username": args.publish_username,
        }
    elif args.name == "homebrew":
        observed = {
            "repository": args.repository,
            "repository_id": args.repository_id,
            "bot_login": args.bot_login,
        }
    elif args.name == "conan_fork":
        observed = {
            "repository": args.repository,
            "repository_id": args.repository_id,
            "upstream": args.upstream,
        }
    else:
        observed = {
            "repository": args.repository,
            "repository_id": args.repository_id,
        }
    if any(not isinstance(value, str) or not value for value in observed.values()):
        raise SystemExit("publication-contract: destination arguments are incomplete")
    try:
        require_destination(args.contract, name=args.name, observed=observed)
    except (OSError, UnicodeError, ValidationError) as error:
        raise SystemExit(f"publication-contract: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
