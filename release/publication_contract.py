"""Build and validate the signed, self-contained publication contract."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
from typing import Mapping, Sequence

from .artifacts import (
    CONAN_REFERENCE_RE,
    canonical_json_bytes,
    load_conan_requirements,
)
from .model import ValidationError
from .native_builder_lock import load_builder_lock
from .native_targets import NativeTarget, load_native_targets
from .strict_json import load_strict


CONTRACT_ASSET = "release-publication-contract.json"
SUPPORTED_SCHEMA_VERSIONS = frozenset({1})
_OCI_DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
_REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
_CLOUDSMITH_COORDINATE = re.compile(r"[a-z0-9][a-z0-9-]{0,63}")
_GPG_FINGERPRINT = re.compile(r"[0-9A-F]{40}")
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
@dataclass(frozen=True)
class PublicationContract:
    """Normalized schema-v1 policy retained inside an immutable release."""

    native_targets: tuple[NativeTarget, ...]
    native_builders: tuple[Mapping[str, str], ...]
    channel_targets: Mapping[str, object]
    destinations: Mapping[str, Mapping[str, str]]
    conan_requirements: tuple[str, ...]


def _read_json(path: Path) -> object:
    return load_strict(path, label=f"publication input {path.name}")


def build_publication_contract(
    *,
    root: Path,
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
        "native_targets": [
            target.to_mapping()
            for target in load_native_targets(root / "packaging/targets.json")
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


def parse_publication_contract(value: object) -> PublicationContract:
    """Parse one supported immutable contract without consulting current catalogs."""

    fields = {
        "schema_version", "native_targets", "channel_targets", "destinations",
        "native_builders", "conan_requirements",
    }
    # Contracts signed while providers ran from control tags also carry
    # ``provider_controls``. Providers now run from their protected ``main``,
    # so that historical field is accepted and not read.
    if not isinstance(value, Mapping) or set(value) - {"provider_controls"} != fields:
        raise ValidationError("publication contract schema is not exact")
    if value.get("schema_version") not in SUPPORTED_SCHEMA_VERSIONS:
        raise ValidationError("publication contract version is unsupported")

    target_values = value.get("native_targets")
    if not isinstance(target_values, list):
        raise ValidationError("publication contract native target inventory is missing")
    targets = tuple(NativeTarget.from_mapping(item) for item in target_values)
    if (
        [target.to_mapping() for target in targets] != target_values
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
            {"namespace", "repository", "repository_gpg_fingerprint"}
        ),
        "homebrew": frozenset({"repository", "repository_id"}),
        "chocolatey": frozenset({"repository", "repository_id"}),
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
                and (
                    any(
                        _CLOUDSMITH_COORDINATE.fullmatch(destination[field]) is None
                        for field in {"namespace", "repository"}
                    )
                    or _GPG_FINGERPRINT.fullmatch(
                        destination["repository_gpg_fingerprint"]
                    )
                    is None
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

    return PublicationContract(
        native_targets=targets,
        native_builders=tuple(builders),
        channel_targets=dict(channel_targets),
        destinations=normalized_destinations,
        conan_requirements=requirements,
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
    destinations: Mapping[str, Mapping[str, str]],
) -> None:
    path.write_bytes(
        canonical_json_bytes(
            build_publication_contract(
                root=root,
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
            "aur", "cloudsmith", "homebrew", "chocolatey", "conan_control",
        ),
        required=True,
    )
    parser.add_argument("--namespace")
    parser.add_argument("--repository", required=True)
    parser.add_argument("--repository-id")
    parser.add_argument("--repository-gpg-fingerprint")
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
            "repository_gpg_fingerprint": args.repository_gpg_fingerprint,
        }
    elif args.name == "homebrew":
        observed = {
            "repository": args.repository,
            "repository_id": args.repository_id,
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
