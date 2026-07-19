#!/usr/bin/env python3
"""Select and verify immutable ABI baselines for stable releases.

The module deliberately uses only the Python standard library so the release
workflow can run it in an isolated interpreter before installing project or
third-party dependencies.  It does not perform network access: callers must
obtain release and asset inventories from the repository-scoped GitHub APIs.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any, Mapping, Sequence


SEMVER = re.compile(
    r"(?P<major>0|[1-9][0-9]*)\."
    r"(?P<minor>0|[1-9][0-9]*)\."
    r"(?P<patch>0|[1-9][0-9]*)"
    r"(?:-rc\.(?P<rc>[1-9][0-9]*))?"
)
DIGEST = re.compile(r"[0-9a-f]{64}")
ASSET_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}")
BUILD_TUPLE = re.compile(r"[a-z0-9][a-z0-9._+-]{0,127}")
TOOL_VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+~-]{0,63}")
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
        "release_ledger",
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


@dataclass(frozen=True)
class Version:
    major: int
    minor: int
    patch: int
    rc: int | None = None

    @classmethod
    def from_tag(cls, tag: object, *, stable_only: bool = False) -> "Version":
        if not isinstance(tag, str) or len(tag) > 65 or not tag.startswith("v"):
            raise AbiPolicyError("release tag must be canonical v-prefixed SemVer")
        match = SEMVER.fullmatch(tag[1:])
        if match is None:
            raise AbiPolicyError("release tag must be vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-rc.N")
        version = cls(
            int(match.group("major")),
            int(match.group("minor")),
            int(match.group("patch")),
            int(match.group("rc")) if match.group("rc") else None,
        )
        if stable_only and version.rc is not None:
            raise AbiPolicyError("ABI baselines apply only to stable releases")
        return version

    @property
    def tag(self) -> str:
        suffix = f"-rc.{self.rc}" if self.rc is not None else ""
        return f"v{self.major}.{self.minor}.{self.patch}{suffix}"

    @property
    def core(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    @property
    def loader_identity(self) -> str:
        """Return the shared-library loader/package identity.

        Pre-1.0 releases deliberately use the complete stable version so that
        no 0.x update silently claims loader compatibility.  This identity is
        intentionally independent from the broader comparison series used to
        choose ABI evidence.
        """

        return self.core if self.major == 0 else str(self.major)

    @property
    def comparison_series(self) -> str:
        """Return the stable-release series used for ABI comparisons."""

        return f"0.{self.minor}" if self.major == 0 else str(self.major)

    @property
    def stable_key(self) -> tuple[int, int, int]:
        if self.rc is not None:
            raise AbiPolicyError("a release candidate has no stable ordering key")
        return self.major, self.minor, self.patch


@dataclass(frozen=True)
class ReleaseIdentity:
    database_id: int
    version: Version
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
        version = Version.from_tag(value["tagName"])
        if is_prerelease != (version.rc is not None):
            raise AbiPolicyError("GitHub prerelease state disagrees with the canonical tag")
        if is_draft and is_immutable:
            raise AbiPolicyError("a draft GitHub release cannot be an immutable baseline")
        return cls(database_id, version, is_draft, is_immutable, is_prerelease)


@dataclass(frozen=True)
class BaselineSelection:
    current: Version
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
        current = Version.from_tag(value["current_tag"], stable_only=True)
        if value["abi_line"] != current.comparison_series:
            raise AbiPolicyError("ABI baseline selection has the wrong comparison series")
        if value["status"] == "new-abi-line":
            if value["baseline_tag"] is not None or value["baseline_release_id"] is not None:
                raise AbiPolicyError("a new comparison series must not name a baseline")
            return cls(current, None)
        if value["status"] != "selected":
            raise AbiPolicyError("ABI baseline selection has an unknown status")
        baseline_version = Version.from_tag(value["baseline_tag"], stable_only=True)
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
    current = Version.from_tag(current_tag, stable_only=True)
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


@dataclass(frozen=True)
class BaselineArtifact:
    release_id: int
    tag: str
    comparison_series: str
    manifest_asset_id: int
    manifest_sha256: str
    corpus_asset_id: int
    corpus_name: str
    corpus_sha256: str
    build_tuple: str

    def to_mapping(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "release_id": self.release_id,
            "tag": self.tag,
            "abi_line": self.comparison_series,
            "manifest_asset_id": self.manifest_asset_id,
            "manifest_sha256": self.manifest_sha256,
            "corpus_asset_id": self.corpus_asset_id,
            "corpus_name": self.corpus_name,
            "corpus_sha256": self.corpus_sha256,
            "build_tuple": self.build_tuple,
        }


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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


def verify_baseline_artifacts(
    selection: BaselineSelection,
    asset_pages: object,
    manifest_bytes: bytes,
    corpus_bytes: bytes,
    *,
    expected_build_tuple: str,
) -> BaselineArtifact:
    """Bind a downloaded ABI corpus to one immutable release and its manifest."""
    if selection.baseline is None:
        raise AbiPolicyError("a new comparison series has no baseline artifacts to verify")
    if BUILD_TUPLE.fullmatch(expected_build_tuple) is None:
        raise AbiPolicyError("expected ABI build tuple is malformed")
    assets = tuple(
        RemoteAsset.from_mapping(item)
        for item in _flatten_pages(asset_pages, label="GitHub release asset inventory")
    )
    names = [asset.name for asset in assets]
    ids = [asset.asset_id for asset in assets]
    if len(names) != len(set(names)) or len(ids) != len(set(ids)):
        raise AbiPolicyError("GitHub release asset inventory contains duplicates")
    by_name = {asset.name: asset for asset in assets}
    manifest_asset = by_name.get("release-manifest.json")
    if manifest_asset is None:
        raise AbiPolicyError("immutable baseline release has no manifest asset")
    if manifest_asset.size != len(manifest_bytes) or manifest_asset.sha256 != _sha256(manifest_bytes):
        raise AbiPolicyError("downloaded baseline manifest differs from GitHub's immutable asset digest")
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AbiPolicyError("baseline manifest is not canonical UTF-8 JSON") from error
    version = selection.baseline.version
    if (
        not isinstance(manifest, Mapping)
        or set(manifest) != MANIFEST_FIELDS
        or manifest.get("schema_version") != 2
        or manifest.get("package") != "mcp-cpp-sdk"
        or manifest.get("version") != version.core
        or manifest.get("tag") != version.tag
    ):
        raise AbiPolicyError("baseline manifest identity is not exact")
    records = _payload_records(manifest)
    payload_names = {record["name"] for record in records}
    detached = {f"{name}.asc" for name in payload_names if name.endswith((".tar.gz", ".zip"))}
    if set(by_name) != payload_names | set(CONTROL_ASSETS) | detached:
        raise AbiPolicyError("immutable baseline asset set is not exactly covered by its manifest")
    for record in records:
        remote = by_name[record["name"]]
        if remote.size != record["size"] or remote.sha256 != record["sha256"]:
            raise AbiPolicyError("baseline manifest payload identity differs from GitHub's immutable asset digest")
    corpus_records = [
        record
        for record in records
        if record["role"] == "abi-corpus" and record.get("build_tuple") == expected_build_tuple
    ]
    if len(corpus_records) != 1:
        raise AbiPolicyError("baseline manifest does not contain exactly one ABI corpus for the build tuple")
    corpus = corpus_records[0]
    expected_name = f"mcp-cpp-sdk-{version.core}-{expected_build_tuple}.abi.xml"
    if corpus["name"] != expected_name:
        raise AbiPolicyError("baseline ABI corpus name is not canonical")
    remote_corpus = by_name[expected_name]
    corpus_digest = _sha256(corpus_bytes)
    if remote_corpus.size != len(corpus_bytes) or remote_corpus.sha256 != corpus_digest:
        raise AbiPolicyError("downloaded ABI corpus differs from GitHub's immutable asset digest")
    return BaselineArtifact(
        selection.baseline.database_id,
        version.tag,
        version.comparison_series,
        manifest_asset.asset_id,
        manifest_asset.sha256,
        remote_corpus.asset_id,
        remote_corpus.name,
        corpus_digest,
        expected_build_tuple,
    )


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


def verify_abidiff_result(
    value: object,
    selection: BaselineSelection,
    *,
    baseline_sha256: str,
    candidate_sha256: str,
    report_bytes: bytes,
    expected_tool_version: str,
) -> None:
    """Accept only a bound, successful, no-change ``abidiff`` result.

    Libabigail documents status 4 as a change that might or might not be ABI
    compatible.  It therefore requires review and is rejected here along with
    explicitly incompatible changes and tool errors.
    """
    if selection.baseline is None:
        raise AbiPolicyError("abidiff must not run without a selected baseline")
    if not isinstance(value, Mapping) or set(value) != ABIDIFF_RESULT_FIELDS:
        raise AbiPolicyError("abidiff result fields are not exact")
    expected = {
        "schema_version": 1,
        "tool": "abidiff",
        "tool_version": expected_tool_version,
        "baseline_tag": selection.baseline.version.tag,
        "candidate_tag": selection.current.tag,
        "abi_line": selection.current.comparison_series,
        "baseline_sha256": baseline_sha256,
        "candidate_sha256": candidate_sha256,
        "report_sha256": _sha256(report_bytes),
    }
    if TOOL_VERSION.fullmatch(expected_tool_version) is None:
        raise AbiPolicyError("expected abidiff version is malformed")
    corpus_digests = (baseline_sha256, candidate_sha256)
    if any(not isinstance(digest, str) or DIGEST.fullmatch(digest) is None for digest in corpus_digests):
        raise AbiPolicyError("ABI corpus digest is malformed")
    if any(value[field] != expected[field] for field in expected):
        raise AbiPolicyError("abidiff result is not bound to the selected baseline and candidate")
    exit_code = value["exit_code"]
    if type(exit_code) is not int or exit_code < 0 or exit_code > 255:
        raise AbiPolicyError("abidiff exit code is malformed")
    if exit_code & ~ABIDIFF_KNOWN_STATUS_BITS:
        raise AbiPolicyError("abidiff exit code contains unknown status bits")
    if exit_code & ABIDIFF_USAGE_ERROR:
        raise AbiPolicyError("abidiff reported a usage error")
    if exit_code & ABIDIFF_ERROR:
        raise AbiPolicyError("abidiff failed to compare the ABI corpora")
    if exit_code & ABIDIFF_INCOMPATIBLE_CHANGE and not exit_code & ABIDIFF_ABI_CHANGE:
        raise AbiPolicyError("abidiff returned an internally inconsistent incompatible-change status")
    if exit_code & ABIDIFF_INCOMPATIBLE_CHANGE:
        raise AbiPolicyError("abidiff found an incompatible ABI change")
    if exit_code & ABIDIFF_ABI_CHANGE:
        raise AbiPolicyError("abidiff found an ABI change that requires explicit review")


def _read_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path | None, value: Mapping[str, object]) -> None:
    text = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    if path is None:
        sys.stdout.write(text)
    else:
        path.write_text(text, encoding="ascii")


def _write_github_output(path: Path, selection: BaselineSelection) -> None:
    baseline = selection.baseline
    values = {
        "abi_line": selection.current.comparison_series,
        "has_baseline": "true" if baseline is not None else "false",
        "baseline_tag": baseline.version.tag if baseline is not None else "",
        "baseline_release_id": str(baseline.database_id) if baseline is not None else "",
    }
    with path.open("a", encoding="ascii") as output:
        for name, value in values.items():
            output.write(f"{name}={value}\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)

    select = commands.add_parser("select")
    select.add_argument("--current-tag", required=True)
    select.add_argument("--catalog", type=Path, required=True)
    select.add_argument("--output", type=Path)
    select.add_argument("--github-output", type=Path)

    artifacts = commands.add_parser("verify-baseline")
    artifacts.add_argument("--selection", type=Path, required=True)
    artifacts.add_argument("--assets", type=Path, required=True)
    artifacts.add_argument("--manifest", type=Path, required=True)
    artifacts.add_argument("--corpus", type=Path, required=True)
    artifacts.add_argument("--build-tuple", required=True)
    artifacts.add_argument("--output", type=Path)

    result = commands.add_parser("verify-result")
    result.add_argument("--selection", type=Path, required=True)
    result.add_argument("--result", type=Path, required=True)
    result.add_argument("--report", type=Path, required=True)
    result.add_argument("--baseline-sha256", required=True)
    result.add_argument("--candidate-sha256", required=True)
    result.add_argument("--tool-version", required=True)

    args = parser.parse_args(argv)
    try:
        if args.command == "select":
            selection = select_baseline(args.current_tag, _read_json(args.catalog))
            _write_json(args.output, selection.to_mapping())
            if args.github_output is not None:
                _write_github_output(args.github_output, selection)
        elif args.command == "verify-baseline":
            selection = BaselineSelection.from_mapping(_read_json(args.selection))
            artifact = verify_baseline_artifacts(
                selection,
                _read_json(args.assets),
                args.manifest.read_bytes(),
                args.corpus.read_bytes(),
                expected_build_tuple=args.build_tuple,
            )
            _write_json(args.output, artifact.to_mapping())
        else:
            selection = BaselineSelection.from_mapping(_read_json(args.selection))
            verify_abidiff_result(
                _read_json(args.result),
                selection,
                baseline_sha256=args.baseline_sha256,
                candidate_sha256=args.candidate_sha256,
                report_bytes=args.report.read_bytes(),
                expected_tool_version=args.tool_version,
            )
    except (AbiPolicyError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"abi-baseline: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
