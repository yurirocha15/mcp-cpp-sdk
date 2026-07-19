#!/usr/bin/env python3
"""Select and verify immutable ABI baselines for stable releases.

The module deliberately uses only the Python standard library so the release
workflow can run it in an isolated interpreter before installing project or
third-party dependencies.  It does not perform network access: callers must
obtain release and asset inventories from the repository-scoped GitHub APIs.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import PurePosixPath
import re
from typing import Any, Mapping

from .model import SemVer, ValidationError

DIGEST = re.compile(r"[0-9a-f]{64}")
ASSET_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}")
BUILD_TUPLE = re.compile(r"[a-z0-9][a-z0-9._+-]{0,127}")
RELEASE_FIELDS = frozenset(
    {"databaseId", "isDraft", "isImmutable", "isPrerelease", "tagName"}
)
MANIFEST_FIELDS = frozenset(
    {
        "schema_version",
        "package",
        "version",
        "tag",
        "commit",
        "source_tree_sha256",
        "signers",
        "channel_capabilities",
        "payloads",
        "dependency_closure",
        "conan_requirements",
        "provenance_subjects",
    }
)
CONTROL_ASSETS = frozenset(
    {"release-manifest.json", "release-manifest.json.asc", "SHA256SUMS", "SHA256SUMS.asc"}
)
ABIDIFF_ERROR = 1
ABIDIFF_USAGE_ERROR = 2
ABIDIFF_ABI_CHANGE = 4
ABIDIFF_INCOMPATIBLE_CHANGE = 8
ABIDIFF_KNOWN_STATUS_BITS = (
    ABIDIFF_ERROR | ABIDIFF_USAGE_ERROR | ABIDIFF_ABI_CHANGE | ABIDIFF_INCOMPATIBLE_CHANGE
)


class AbiPolicyError(ValueError):
    """Raised when baseline identity or ABI evidence is incomplete or unsafe."""


def version_from_tag(tag: object, *, stable_only: bool = False) -> SemVer:
    """Parse the canonical project SemVer and expose ABI-policy errors."""

    try:
        if not isinstance(tag, str):
            raise ValidationError("tag must be a string")
        version = SemVer.from_tag(tag)
        if stable_only and version.is_prerelease:
            raise AbiPolicyError("ABI baselines apply only to stable releases")
        return version
    except AbiPolicyError:
        raise
    except ValidationError as error:
        raise AbiPolicyError(str(error)) from error


@dataclass(frozen=True)
class ReleaseIdentity:
    database_id: int
    version: SemVer
    is_draft: bool
    is_immutable: bool
    is_prerelease: bool

    @classmethod
    def from_mapping(cls, value: object) -> "ReleaseIdentity":
        if not isinstance(value, Mapping) or set(value) != RELEASE_FIELDS:
            raise AbiPolicyError("GitHub release metadata fields are not exact")
        database_id = value["databaseId"]
        if type(database_id) is not int or database_id < 1:
            raise AbiPolicyError("GitHub release databaseId must be a positive integer")
        is_draft = value["isDraft"]
        is_immutable = value["isImmutable"]
        is_prerelease = value["isPrerelease"]
        if any(type(flag) is not bool for flag in (is_draft, is_immutable, is_prerelease)):
            raise AbiPolicyError("GitHub release state fields must be booleans")
        version = version_from_tag(value["tagName"])
        if is_prerelease != (version.rc is not None):
            raise AbiPolicyError("GitHub prerelease state disagrees with the canonical tag")
        if is_draft and is_immutable:
            raise AbiPolicyError("a draft GitHub release cannot be an immutable baseline")
        return cls(database_id, version, is_draft, is_immutable, is_prerelease)


@dataclass(frozen=True)
class BaselineSelection:
    current: SemVer
    baseline: ReleaseIdentity | None

    @property
    def has_baseline(self) -> bool:
        return self.baseline is not None

    def to_mapping(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "status": "selected" if self.baseline is not None else "new-abi-line",
            "current_tag": self.current.tag,
            # ``abi_line`` is retained in the serialized schema for workflow
            # compatibility; its value is the ABI comparison series.
            "abi_line": self.current.comparison_series,
            "baseline_tag": self.baseline.version.tag if self.baseline is not None else None,
            "baseline_release_id": self.baseline.database_id if self.baseline is not None else None,
        }

    @classmethod
    def from_mapping(cls, value: object) -> "BaselineSelection":
        expected = {
            "schema_version",
            "status",
            "current_tag",
            "abi_line",
            "baseline_tag",
            "baseline_release_id",
        }
        if not isinstance(value, Mapping) or set(value) != expected or value["schema_version"] != 1:
            raise AbiPolicyError("ABI baseline selection fields are not exact")
        current = version_from_tag(value["current_tag"], stable_only=True)
        if value["abi_line"] != current.comparison_series:
            raise AbiPolicyError("ABI baseline selection has the wrong comparison series")
        if value["status"] == "new-abi-line":
            if value["baseline_tag"] is not None or value["baseline_release_id"] is not None:
                raise AbiPolicyError("a new comparison series must not name a baseline")
            return cls(current, None)
        if value["status"] != "selected":
            raise AbiPolicyError("ABI baseline selection has an unknown status")
        baseline_version = version_from_tag(value["baseline_tag"], stable_only=True)
        database_id = value["baseline_release_id"]
        if type(database_id) is not int or database_id < 1:
            raise AbiPolicyError("selected baseline release ID is malformed")
        if (
            baseline_version.comparison_series != current.comparison_series
            or baseline_version.stable_key >= current.stable_key
        ):
            raise AbiPolicyError(
                "selected baseline is not an earlier stable release in the same comparison series"
            )
        baseline = ReleaseIdentity(database_id, baseline_version, False, True, False)
        return cls(current, baseline)


def _flatten_pages(value: object, *, label: str) -> tuple[Mapping[str, Any], ...]:
    if not isinstance(value, list):
        raise AbiPolicyError(f"{label} must be a JSON list")
    if not value:
        return ()
    if all(isinstance(item, Mapping) for item in value):
        return tuple(value)
    if all(isinstance(page, list) for page in value):
        flattened: list[Mapping[str, Any]] = []
        for page in value:
            if any(not isinstance(item, Mapping) for item in page):
                raise AbiPolicyError(f"{label} contains a malformed page")
            flattened.extend(page)
        return tuple(flattened)
    raise AbiPolicyError(f"{label} mixes paginated and unpaginated records")


def select_baseline(current_tag: str, catalog: object) -> BaselineSelection:
    """Select the latest earlier immutable stable in the comparison series."""
    current = version_from_tag(current_tag, stable_only=True)
    releases = tuple(
        ReleaseIdentity.from_mapping(item)
        for item in _flatten_pages(catalog, label="GitHub release catalog")
    )
    ids = [release.database_id for release in releases]
    tags = [release.version.tag for release in releases]
    if len(ids) != len(set(ids)) or len(tags) != len(set(tags)):
        raise AbiPolicyError("GitHub release catalog contains duplicate IDs or tags")
    if current.tag in tags:
        raise AbiPolicyError("the candidate tag already has a GitHub release")

    eligible: list[ReleaseIdentity] = []
    for release in releases:
        version = release.version
        if (
            version.rc is not None
            or release.is_draft
            or version.comparison_series != current.comparison_series
        ):
            continue
        if version.stable_key >= current.stable_key:
            continue
        if not release.is_immutable:
            raise AbiPolicyError(
                "a published prior stable release in the comparison series is mutable"
            )
        eligible.append(release)
    baseline = max(eligible, key=lambda item: item.version.stable_key, default=None)
    return BaselineSelection(current, baseline)


@dataclass(frozen=True)
class RemoteAsset:
    asset_id: int
    name: str
    size: int
    sha256: str

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "RemoteAsset":
        required = {"id", "name", "size", "state", "digest"}
        if not required <= set(value):
            raise AbiPolicyError("GitHub release asset metadata is incomplete")
        asset_id, name, size = value["id"], value["name"], value["size"]
        if type(asset_id) is not int or asset_id < 1:
            raise AbiPolicyError("GitHub release asset ID must be a positive integer")
        if not isinstance(name, str) or ASSET_NAME.fullmatch(name) is None or PurePosixPath(name).name != name:
            raise AbiPolicyError("GitHub release asset name is unsafe")
        if type(size) is not int or size < 1 or value["state"] != "uploaded":
            raise AbiPolicyError("GitHub release asset is incomplete")
        digest = value["digest"]
        if not isinstance(digest, str) or not digest.startswith("sha256:") or DIGEST.fullmatch(digest[7:]) is None:
            raise AbiPolicyError("GitHub release asset lacks a canonical server-side SHA-256")
        return cls(asset_id, name, size, digest[7:])


def _payload_records(manifest: Mapping[str, Any]) -> tuple[Mapping[str, Any], ...]:
    payloads = manifest.get("payloads")
    if not isinstance(payloads, list):
        raise AbiPolicyError("baseline manifest payload inventory is missing")
    records: list[Mapping[str, Any]] = []
    names: list[str] = []
    for value in payloads:
        if not isinstance(value, Mapping) or not {"name", "size", "sha256", "role"} <= set(value):
            raise AbiPolicyError("baseline manifest contains a malformed payload record")
        if set(value) - {"name", "size", "sha256", "role", "build_tuple"}:
            raise AbiPolicyError("baseline manifest payload record has unknown fields")
        name, size, digest, role = value["name"], value["size"], value["sha256"], value["role"]
        if not isinstance(name, str) or ASSET_NAME.fullmatch(name) is None or PurePosixPath(name).name != name:
            raise AbiPolicyError("baseline manifest payload name is unsafe")
        malformed_identity = (
            type(size) is not int
            or size < 1
            or not isinstance(digest, str)
            or DIGEST.fullmatch(digest) is None
        )
        if malformed_identity:
            raise AbiPolicyError("baseline manifest payload identity is malformed")
        if not isinstance(role, str) or not role or len(role) > 80:
            raise AbiPolicyError("baseline manifest payload role is malformed")
        build_tuple = value.get("build_tuple")
        if build_tuple is not None and (
            not isinstance(build_tuple, str) or BUILD_TUPLE.fullmatch(build_tuple) is None
        ):
            raise AbiPolicyError("baseline manifest build tuple is malformed")
        names.append(name)
        records.append(value)
    if names != sorted(names) or len(names) != len(set(names)):
        raise AbiPolicyError("baseline manifest payloads are not uniquely and canonically ordered")
    return tuple(records)


ABIDIFF_RESULT_FIELDS = frozenset(
    {
        "schema_version",
        "tool",
        "tool_version",
        "baseline_tag",
        "candidate_tag",
        "abi_line",
        "baseline_sha256",
        "candidate_sha256",
        "report_sha256",
        "exit_code",
    }
)
