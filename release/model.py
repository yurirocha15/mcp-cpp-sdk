"""Strict, provider-independent release data models."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import re
from typing import Any, Mapping
from uuid import UUID


class ValidationError(ValueError):
    """Raised when release input is not canonical or violates policy."""


_SEMVER_RE = re.compile(
    r"(?P<major>0|[1-9][0-9]*)\."
    r"(?P<minor>0|[1-9][0-9]*)\."
    r"(?P<patch>0|[1-9][0-9]*)"
    r"(?:-rc\.(?P<rc>[1-9][0-9]*))?"
)
_SHA_RE = re.compile(r"[0-9a-f]{40}")
_DIGEST_RE = re.compile(r"[0-9a-f]{64}")
_DECIMAL_RE = re.compile(r"0|[1-9][0-9]*")


@dataclass(frozen=True)
class SemVer:
    """The project's deliberately narrow canonical SemVer profile."""

    major: int
    minor: int
    patch: int
    rc: int | None = None

    def __post_init__(self) -> None:
        if min(self.major, self.minor, self.patch) < 0 or (self.rc is not None and self.rc < 1):
            raise ValidationError("version components must be non-negative and RC numbers must be positive")

    @classmethod
    def parse(cls, value: str, *, stable_only: bool = False) -> "SemVer":
        if not isinstance(value, str) or len(value) > 64:
            raise ValidationError("version must be a string of at most 64 characters")
        match = _SEMVER_RE.fullmatch(value)
        if match is None:
            raise ValidationError("version must be MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-rc.N")
        version = cls(
            major=int(match.group("major")),
            minor=int(match.group("minor")),
            patch=int(match.group("patch")),
            rc=int(match.group("rc")) if match.group("rc") else None,
        )
        if stable_only and version.rc is not None:
            raise ValidationError("a stable version is required")
        return version

    @classmethod
    def from_tag(cls, tag: str, *, stable_only: bool = False) -> "SemVer":
        if not isinstance(tag, str) or not tag.startswith("v"):
            raise ValidationError("tag must start with v")
        return cls.parse(tag[1:], stable_only=stable_only)

    @property
    def is_prerelease(self) -> bool:
        return self.rc is not None

    @property
    def core(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    @property
    def tag(self) -> str:
        return f"v{self}"

    @property
    def debian_version(self) -> str:
        suffix = f"~rc.{self.rc}" if self.rc is not None else ""
        return f"{self.core}{suffix}-1"

    @property
    def rpm_version_release(self) -> tuple[str, str]:
        release = f"0.{self.rc}.rc.{self.rc}" if self.rc is not None else "1"
        return self.core, release

    @property
    def arch_pkgver(self) -> str:
        suffix = f"rc{self.rc}" if self.rc is not None else ""
        return f"{self.core}{suffix}"

    def __str__(self) -> str:
        suffix = f"-rc.{self.rc}" if self.rc is not None else ""
        return f"{self.core}{suffix}"


@dataclass(frozen=True)
class DispatchRequest:
    """Validated inputs accepted by the Conan release-control broker."""

    source_tag: str
    source_commit_sha: str
    github_release_id: str
    source_workflow_run_id: str
    release_manifest_sha256: str
    fork_branch: str
    fork_head_sha: str
    recipe_tree_sha256: str
    request_uuid: str

    _FIELDS = (
        "source_tag",
        "source_commit_sha",
        "github_release_id",
        "source_workflow_run_id",
        "release_manifest_sha256",
        "fork_branch",
        "fork_head_sha",
        "recipe_tree_sha256",
        "request_uuid",
    )

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "DispatchRequest":
        if not isinstance(value, Mapping):
            raise ValidationError("dispatch request must be an object")
        if set(value) != set(cls._FIELDS):
            missing = sorted(set(cls._FIELDS) - set(value))
            extra = sorted(set(value) - set(cls._FIELDS))
            raise ValidationError(f"dispatch fields mismatch; missing={missing}, extra={extra}")
        for field in cls._FIELDS:
            item = value[field]
            if not isinstance(item, str) or not item or len(item) > 160:
                raise ValidationError(f"{field} must be a non-empty bounded string")
            if any(character.isspace() or ord(character) < 0x20 for character in item):
                raise ValidationError(f"{field} contains whitespace or control characters")

        version = SemVer.from_tag(value["source_tag"], stable_only=True)
        cls._require_match("source_commit_sha", value["source_commit_sha"], _SHA_RE)
        cls._require_match("fork_head_sha", value["fork_head_sha"], _SHA_RE)
        cls._require_match("release_manifest_sha256", value["release_manifest_sha256"], _DIGEST_RE)
        cls._require_match("recipe_tree_sha256", value["recipe_tree_sha256"], _DIGEST_RE)
        cls._require_match("github_release_id", value["github_release_id"], _DECIMAL_RE)
        cls._require_match("source_workflow_run_id", value["source_workflow_run_id"], _DECIMAL_RE)

        expected_branch = f"package/mcp-cpp-sdk-{version}"
        recovery_re = re.compile(re.escape(expected_branch) + r"-r[1-9][0-9]*")
        if value["fork_branch"] != expected_branch and recovery_re.fullmatch(value["fork_branch"]) is None:
            raise ValidationError("fork_branch does not match the source tag")

        try:
            parsed_uuid = UUID(value["request_uuid"])
        except ValueError as error:
            raise ValidationError("request_uuid is not a UUID") from error
        if str(parsed_uuid) != value["request_uuid"]:
            raise ValidationError("request_uuid must use canonical lowercase form")
        return cls(**{field: value[field] for field in cls._FIELDS})

    @staticmethod
    def _require_match(field: str, value: str, pattern: re.Pattern[str]) -> None:
        if pattern.fullmatch(value) is None:
            raise ValidationError(f"{field} is not canonical")


class DestinationResult(str, Enum):
    PUBLISHED = "PUBLISHED"
    SUBMITTED_PENDING_REVIEW = "SUBMITTED_PENDING_REVIEW"
    SUBMITTED_PENDING_MODERATION = "SUBMITTED_PENDING_MODERATION"
    BLOCKED_MANUAL_ACTION = "BLOCKED_MANUAL_ACTION"
    FAILED = "FAILED"
    SKIPPED_ALREADY_IDENTICAL = "SKIPPED_ALREADY_IDENTICAL"
    FIRST_USE_UNPROVEN = "FIRST_USE_UNPROVEN"
    LIVE = "LIVE"


RELEASE_DESTINATIONS = (
    "github",
    "conan2",
    "deb_apt",
    "rpm",
    "arch_aur",
    "homebrew",
    "chocolatey",
)


@dataclass(frozen=True)
class ReleaseLedger:
    """A strict snapshot of the externally visible release state."""

    version: SemVer
    tag: str
    source_commit_sha: str
    release_manifest_sha256: str
    results: Mapping[str, DestinationResult]

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "ReleaseLedger":
        expected = {"schema_version", "version", "tag", "source_commit_sha", "release_manifest_sha256", "results"}
        if not isinstance(value, Mapping) or set(value) != expected:
            raise ValidationError("ledger fields do not match schema version 1")
        if value["schema_version"] != 1:
            raise ValidationError("unsupported ledger schema version")
        version = SemVer.parse(value["version"])
        if value["tag"] != version.tag:
            raise ValidationError("ledger tag and version disagree")
        DispatchRequest._require_match("source_commit_sha", value["source_commit_sha"], _SHA_RE)
        DispatchRequest._require_match("release_manifest_sha256", value["release_manifest_sha256"], _DIGEST_RE)
        raw_results = value["results"]
        if not isinstance(raw_results, Mapping) or set(raw_results) != set(RELEASE_DESTINATIONS):
            raise ValidationError("ledger must contain every release destination exactly once")
        try:
            results = {name: DestinationResult(raw_results[name]) for name in RELEASE_DESTINATIONS}
        except (TypeError, ValueError) as error:
            raise ValidationError("ledger contains an unknown destination result") from error
        if version.is_prerelease:
            downstream = [name for name in RELEASE_DESTINATIONS if name != "github"]
            forbidden = {
                DestinationResult.PUBLISHED,
                DestinationResult.SUBMITTED_PENDING_REVIEW,
                DestinationResult.SUBMITTED_PENDING_MODERATION,
                DestinationResult.SKIPPED_ALREADY_IDENTICAL,
                DestinationResult.LIVE,
            }
            if any(results[name] in forbidden for name in downstream):
                raise ValidationError("prereleases cannot be published to downstream channels")
        public_anchor = {
            DestinationResult.PUBLISHED,
            DestinationResult.SKIPPED_ALREADY_IDENTICAL,
            DestinationResult.LIVE,
        }
        downstream_public = public_anchor | {
            DestinationResult.SUBMITTED_PENDING_REVIEW,
            DestinationResult.SUBMITTED_PENDING_MODERATION,
        }
        if results["github"] not in public_anchor and any(
            results[name] in downstream_public for name in RELEASE_DESTINATIONS if name != "github"
        ):
            raise ValidationError("downstream publication requires an immutable GitHub release anchor")
        return cls(version, value["tag"], value["source_commit_sha"], value["release_manifest_sha256"], results)

    def to_mapping(self) -> dict[str, Any]:
        return {
            "schema_version": 1,
            "version": str(self.version),
            "tag": self.tag,
            "source_commit_sha": self.source_commit_sha,
            "release_manifest_sha256": self.release_manifest_sha256,
            "results": {name: self.results[name].value for name in RELEASE_DESTINATIONS},
        }
