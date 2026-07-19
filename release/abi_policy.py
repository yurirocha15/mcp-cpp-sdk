#!/usr/bin/env python3
"""Verify immutable ABI baselines and reviewed ABI-change exceptions.

Policy decisions consume GitHub API responses and asset bytes as files, which
keeps them reproducible in offline tests.  The sole provider boundary is the
``download`` subcommand: it asks ``gh`` for reviewed numeric asset IDs and then
checks every byte against the precomputed server-digest plan.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping, Sequence
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import Any

from .abi_baseline import (
    ABIDIFF_ABI_CHANGE,
    ABIDIFF_ERROR,
    ABIDIFF_INCOMPATIBLE_CHANGE,
    ABIDIFF_KNOWN_STATUS_BITS,
    ABIDIFF_RESULT_FIELDS,
    ABIDIFF_USAGE_ERROR,
    ASSET_NAME,
    AbiPolicyError,
    BaselineSelection,
    CONTROL_ASSETS,
    DIGEST,
    MANIFEST_FIELDS,
    RemoteAsset,
    Version,
    _flatten_pages,
    _payload_records,
    select_baseline,
)
from .abi_build import (
    ABI_BUILD_TUPLE,
    ABI_IDENTITY_NAME,
    AbiBuildError,
    require_compatible_environments,
    sha256_file,
    validate_build_identity,
)
from .artifacts import STABLE_CHANNEL_CAPABILITIES
from .verify_gnupg_status import validate_status


_POLICY_FIELDS = frozenset({"schema_version", "build_tuple", "initial_baselines", "exceptions"})
_INITIAL_FIELDS = frozenset({"abi_line", "tag", "rationale"})
_EXCEPTION_FIELDS = frozenset(
    {
        "id",
        "baseline_tag",
        "candidate_tag",
        "abi_line",
        "baseline_corpus_sha256",
        "candidate_corpus_sha256",
        "baseline_build_identity_sha256",
        "candidate_build_identity_sha256",
        "report_sha256",
        "abidiff_version",
        "exit_code",
        "rationale",
    }
)
_FINGERPRINT = re.compile(r"[0-9A-F]{40}|[0-9A-F]{64}")
_EXCEPTION_ID = re.compile(r"[a-z0-9][a-z0-9-]{2,63}")
_RATIONALE = re.compile(r"[ -~]{20,500}")
_CHECKSUM_LINE = re.compile(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]{0,199})")


class AbiReleasePolicyError(ValueError):
    """Raised when an ABI baseline or exception is not safely authorized."""


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode(
        "ascii"
    )


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _exact_mapping(value: object, fields: frozenset[str], label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or set(value) != fields:
        raise AbiReleasePolicyError(f"{label} fields are not exact")
    return value


def _bounded_rationale(value: object) -> str:
    if not isinstance(value, str) or _RATIONALE.fullmatch(value) is None or value != value.strip():
        raise AbiReleasePolicyError("ABI policy rationale is malformed")
    return value


def load_policy(value: object) -> dict[str, object]:
    """Validate the checked-in first-baseline and exception policy."""

    policy = _exact_mapping(value, _POLICY_FIELDS, "ABI policy")
    if policy["schema_version"] != 1 or policy["build_tuple"] != ABI_BUILD_TUPLE:
        raise AbiReleasePolicyError("ABI policy schema or build tuple is not canonical")
    raw_initial = policy["initial_baselines"]
    raw_exceptions = policy["exceptions"]
    if not isinstance(raw_initial, list) or not isinstance(raw_exceptions, list):
        raise AbiReleasePolicyError("ABI policy entries must be lists")
    initial: list[dict[str, str]] = []
    for raw in raw_initial:
        entry = _exact_mapping(raw, _INITIAL_FIELDS, "initial ABI baseline")
        version = Version.from_tag(entry["tag"], stable_only=True)
        if entry["abi_line"] != version.comparison_series:
            raise AbiReleasePolicyError(
                "initial ABI baseline tag and comparison series disagree"
            )
        initial.append(
            {
                "abi_line": version.comparison_series,
                "tag": version.tag,
                "rationale": _bounded_rationale(entry["rationale"]),
            }
        )
    if initial != sorted(initial, key=lambda entry: entry["abi_line"]):
        raise AbiReleasePolicyError("initial ABI baselines are not canonically ordered")
    if len({entry["abi_line"] for entry in initial}) != len(initial):
        raise AbiReleasePolicyError("initial ABI baseline lines are duplicated")

    exceptions: list[dict[str, object]] = []
    for raw in raw_exceptions:
        entry = _exact_mapping(raw, _EXCEPTION_FIELDS, "ABI exception")
        identifier = entry["id"]
        if not isinstance(identifier, str) or _EXCEPTION_ID.fullmatch(identifier) is None:
            raise AbiReleasePolicyError("ABI exception ID is malformed")
        baseline = Version.from_tag(entry["baseline_tag"], stable_only=True)
        candidate = Version.from_tag(entry["candidate_tag"], stable_only=True)
        if (
            entry["abi_line"] != baseline.comparison_series
            or candidate.comparison_series != baseline.comparison_series
            or baseline.stable_key >= candidate.stable_key
        ):
            raise AbiReleasePolicyError("ABI exception version range is invalid")
        for field in (
            "baseline_corpus_sha256",
            "candidate_corpus_sha256",
            "baseline_build_identity_sha256",
            "candidate_build_identity_sha256",
            "report_sha256",
        ):
            if not isinstance(entry[field], str) or DIGEST.fullmatch(entry[field]) is None:
                raise AbiReleasePolicyError(f"ABI exception {field} is malformed")
        permitted_exit_codes = {ABIDIFF_ABI_CHANGE}
        if baseline.major == 0:
            permitted_exit_codes.add(ABIDIFF_ABI_CHANGE | ABIDIFF_INCOMPATIBLE_CHANGE)
        if (
            entry["abidiff_version"] != "2.4"
            or type(entry["exit_code"]) is not int
            or entry["exit_code"] not in permitted_exit_codes
        ):
            raise AbiReleasePolicyError(
                "ABI exceptions may authorize reviewed libabigail status 4, or status 12 "
                "only within a pre-1.0 comparison series"
            )
        exceptions.append(
            {
                **{field: entry[field] for field in _EXCEPTION_FIELDS - {"rationale"}},
                "rationale": _bounded_rationale(entry["rationale"]),
            }
        )
    if exceptions != sorted(exceptions, key=lambda entry: str(entry["id"])):
        raise AbiReleasePolicyError("ABI exceptions are not canonically ordered")
    if len({entry["id"] for entry in exceptions}) != len(exceptions):
        raise AbiReleasePolicyError("ABI exception IDs are duplicated")
    return {
        "schema_version": 1,
        "build_tuple": ABI_BUILD_TUPLE,
        "initial_baselines": initial,
        "exceptions": exceptions,
    }


def authorize_first_baseline(selection: BaselineSelection, policy: object) -> dict[str, str]:
    """Require an explicit entry before creating a comparison series."""

    normalized = load_policy(policy)
    if selection.baseline is not None:
        raise AbiReleasePolicyError(
            "an existing comparison series cannot use first-baseline authorization"
        )
    matches = [
        entry
        for entry in normalized["initial_baselines"]
        if (
            entry["abi_line"] == selection.current.comparison_series
            and entry["tag"] == selection.current.tag
        )
    ]
    if len(matches) != 1:
        raise AbiReleasePolicyError(
            "new ABI comparison series is not explicitly authorized by the checked-in policy"
        )
    return matches[0]


def normalize_release_catalog(value: object) -> list[list[dict[str, object]]]:
    """Normalize paginated GitHub REST release records for baseline selection."""

    if not isinstance(value, list):
        raise AbiReleasePolicyError("GitHub release catalog must be a paginated JSON list")
    pages = value if not value or all(isinstance(page, list) for page in value) else [value]
    normalized: list[list[dict[str, object]]] = []
    required = {"id", "draft", "immutable", "prerelease", "tag_name"}
    for page in pages:
        if not isinstance(page, list):
            raise AbiReleasePolicyError("GitHub release catalog contains a malformed page")
        normalized_page: list[dict[str, object]] = []
        for raw in page:
            if not isinstance(raw, Mapping) or not required <= set(raw):
                raise AbiReleasePolicyError("GitHub REST release metadata is incomplete")
            normalized_page.append(
                {
                    "databaseId": raw["id"],
                    "isDraft": raw["draft"],
                    "isImmutable": raw["immutable"],
                    "isPrerelease": raw["prerelease"],
                    "tagName": raw["tag_name"],
                }
            )
        normalized.append(normalized_page)
    return normalized


def _remote_assets(asset_pages: object) -> tuple[RemoteAsset, ...]:
    assets = tuple(
        RemoteAsset.from_mapping(item)
        for item in _flatten_pages(asset_pages, label="GitHub release asset inventory")
    )
    names = [asset.name for asset in assets]
    ids = [asset.asset_id for asset in assets]
    if len(names) != len(set(names)) or len(ids) != len(set(ids)):
        raise AbiReleasePolicyError("GitHub release asset inventory contains duplicate names or IDs")
    return assets


def baseline_download_plan(selection: BaselineSelection, asset_pages: object) -> dict[str, object]:
    """Return exact asset IDs to download; URLs are deliberately not trusted."""

    if selection.baseline is None:
        raise AbiReleasePolicyError("a new ABI comparison series has no baseline downloads")
    assets = _remote_assets(asset_pages)
    by_name = {asset.name: asset for asset in assets}
    version = selection.baseline.version
    corpus_name = f"mcp-cpp-sdk-{version.core}-{ABI_BUILD_TUPLE}.abi.xml"
    names = sorted(
        {
            "release-manifest.json",
            "release-manifest.json.asc",
            "SHA256SUMS",
            "SHA256SUMS.asc",
            corpus_name,
            ABI_IDENTITY_NAME,
        }
    )
    if any(name not in by_name for name in names):
        raise AbiReleasePolicyError("immutable baseline release lacks a required ABI control asset")
    return {
        "schema_version": 1,
        "release_id": selection.baseline.database_id,
        "tag": version.tag,
        "assets": [
            {
                "id": by_name[name].asset_id,
                "name": name,
                "size": by_name[name].size,
                "sha256": by_name[name].sha256,
            }
            for name in names
        ],
    }


def download_planned_assets(
    plan_value: object,
    *,
    repository: str,
    directory: Path,
    downloader: Callable[[int, Path], None] | None = None,
) -> None:
    """Download only reviewed asset IDs from the repository-scoped GitHub API."""

    plan = _exact_mapping(
        plan_value,
        frozenset({"schema_version", "release_id", "tag", "assets"}),
        "ABI baseline download plan",
    )
    if repository != "yurirocha15/mcp-cpp-sdk":
        raise AbiReleasePolicyError("ABI baseline repository identity is unexpected")
    if plan["schema_version"] != 1 or type(plan["release_id"]) is not int or plan["release_id"] < 1:
        raise AbiReleasePolicyError("ABI baseline download plan identity is malformed")
    Version.from_tag(plan["tag"], stable_only=True)
    raw_assets = plan["assets"]
    if not isinstance(raw_assets, list) or not raw_assets:
        raise AbiReleasePolicyError("ABI baseline download plan has no assets")
    assets: list[dict[str, object]] = []
    for raw in raw_assets:
        asset = _exact_mapping(
            raw,
            frozenset({"id", "name", "size", "sha256"}),
            "ABI baseline planned asset",
        )
        if type(asset["id"]) is not int or asset["id"] < 1:
            raise AbiReleasePolicyError("ABI baseline planned asset ID is malformed")
        if type(asset["size"]) is not int or asset["size"] < 1:
            raise AbiReleasePolicyError("ABI baseline planned asset size is malformed")
        if not isinstance(asset["name"], str) or ASSET_NAME.fullmatch(asset["name"]) is None:
            raise AbiReleasePolicyError("ABI baseline planned asset name is unsafe")
        if not isinstance(asset["sha256"], str) or DIGEST.fullmatch(asset["sha256"]) is None:
            raise AbiReleasePolicyError("ABI baseline planned asset digest is malformed")
        assets.append(dict(asset))
    if assets != sorted(assets, key=lambda asset: str(asset["name"])):
        raise AbiReleasePolicyError("ABI baseline planned assets are not canonically ordered")
    if len({asset["id"] for asset in assets}) != len(assets) or len(
        {asset["name"] for asset in assets}
    ) != len(assets):
        raise AbiReleasePolicyError("ABI baseline planned assets are duplicated")
    if directory.exists():
        raise AbiReleasePolicyError("ABI baseline download directory must be new")
    directory.mkdir(parents=True)

    def github_download(asset_id: int, output: Path) -> None:
        with output.open("xb") as stream:
            subprocess.run(
                [
                    "gh",
                    "api",
                    "--method",
                    "GET",
                    "-H",
                    "Accept: application/octet-stream",
                    f"repos/{repository}/releases/assets/{asset_id}",
                ],
                check=True,
                stdout=stream,
                stderr=subprocess.PIPE,
                timeout=600,
            )

    fetch = downloader or github_download
    for asset in assets:
        output = directory / str(asset["name"])
        fetch(int(asset["id"]), output)
        if output.is_symlink() or not output.is_file():
            raise AbiReleasePolicyError("GitHub asset download did not produce a regular file")
        if output.stat().st_size != asset["size"] or sha256_file(output) != asset["sha256"]:
            raise AbiReleasePolicyError("GitHub asset download differs from its reviewed plan")


def _parse_checksums(value: bytes) -> dict[str, str]:
    try:
        lines = value.decode("ascii").splitlines()
    except UnicodeError as error:
        raise AbiReleasePolicyError("baseline SHA256SUMS is not ASCII") from error
    checksums: dict[str, str] = {}
    for line in lines:
        match = _CHECKSUM_LINE.fullmatch(line)
        if match is None or match.group(2) in checksums:
            raise AbiReleasePolicyError("baseline SHA256SUMS is malformed or duplicated")
        checksums[match.group(2)] = match.group(1)
    if not checksums or list(checksums) != sorted(checksums):
        raise AbiReleasePolicyError("baseline SHA256SUMS is empty or not canonically ordered")
    return checksums


def _read_exact_files(directory: Path, expected: set[str]) -> dict[str, bytes]:
    if not directory.is_dir() or directory.is_symlink():
        raise AbiReleasePolicyError("baseline download directory is unsafe")
    actual = {path.name for path in directory.iterdir() if path.is_file() and not path.is_symlink()}
    if actual != expected:
        raise AbiReleasePolicyError("baseline download file inventory is not exact")
    return {name: (directory / name).read_bytes() for name in sorted(expected)}


def verify_baseline_bundle(
    selection: BaselineSelection,
    asset_pages: object,
    directory: Path,
    *,
    signature_verifier: Callable[[Path, Path], None],
    expected_signers: Mapping[str, str],
) -> dict[str, object]:
    """Verify server digests, signed checksums, manifest, corpus and identity."""

    plan = baseline_download_plan(selection, asset_pages)
    expected_names = {asset["name"] for asset in plan["assets"]}
    files = _read_exact_files(directory, expected_names)
    assets = _remote_assets(asset_pages)
    by_name = {asset.name: asset for asset in assets}
    for name, data in files.items():
        remote = by_name[name]
        if remote.size != len(data) or remote.sha256 != _sha256(data):
            raise AbiReleasePolicyError(f"downloaded baseline asset differs from GitHub digest: {name}")

    signature_verifier(directory / "release-manifest.json", directory / "release-manifest.json.asc")
    signature_verifier(directory / "SHA256SUMS", directory / "SHA256SUMS.asc")
    checksums = _parse_checksums(files["SHA256SUMS"])
    expected_checksum_names = set(by_name) - {"SHA256SUMS", "SHA256SUMS.asc"}
    if set(checksums) != expected_checksum_names:
        raise AbiReleasePolicyError("signed baseline checksums do not cover the exact immutable asset set")
    for name, digest in checksums.items():
        if digest != by_name[name].sha256:
            raise AbiReleasePolicyError("signed baseline checksum differs from GitHub's server digest")
    for name, data in files.items():
        if name not in {"SHA256SUMS", "SHA256SUMS.asc"} and checksums[name] != _sha256(data):
            raise AbiReleasePolicyError(f"signed baseline checksum does not match downloaded asset: {name}")

    try:
        manifest = json.loads(files["release-manifest.json"].decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AbiReleasePolicyError("baseline manifest is not UTF-8 JSON") from error
    version = selection.baseline.version
    signer_fields = {
        "primary_fingerprint",
        "tag_subkey_fingerprint",
        "artifact_subkey_fingerprint",
    }
    if (
        set(expected_signers) != signer_fields
        or len(set(expected_signers.values())) != 3
        or any(
            not isinstance(fingerprint, str) or _FINGERPRINT.fullmatch(fingerprint) is None
            for fingerprint in expected_signers.values()
        )
    ):
        raise AbiReleasePolicyError("expected baseline signer roles are malformed")
    if (
        not isinstance(manifest, Mapping)
        or set(manifest) != MANIFEST_FIELDS
        or manifest.get("schema_version") != 2
        or manifest.get("package") != "mcp-cpp-sdk"
        or manifest.get("version") != version.core
        or manifest.get("tag") != version.tag
        or manifest.get("signers") != dict(expected_signers)
        or manifest.get("channel_capabilities") != list(STABLE_CHANNEL_CAPABILITIES)
    ):
        raise AbiReleasePolicyError("baseline manifest release identity is not exact")
    if _canonical_json(manifest) != files["release-manifest.json"]:
        raise AbiReleasePolicyError("baseline manifest JSON is not canonical")
    records = _payload_records(manifest)
    payload_names = {record["name"] for record in records}
    detached = {f"{name}.asc" for name in payload_names if name.endswith((".tar.gz", ".zip"))}
    if set(by_name) != payload_names | set(CONTROL_ASSETS) | detached:
        raise AbiReleasePolicyError("baseline manifest does not cover the exact immutable asset set")
    for record in records:
        remote = by_name.get(record["name"])
        if remote is None or remote.size != record["size"] or remote.sha256 != record["sha256"]:
            raise AbiReleasePolicyError("baseline manifest payload differs from GitHub's server digest")

    corpus_name = f"mcp-cpp-sdk-{version.core}-{ABI_BUILD_TUPLE}.abi.xml"
    corpus_records = [
        record
        for record in records
        if record["name"] == corpus_name
        and record["role"] == "abi-corpus"
        and record.get("build_tuple") == ABI_BUILD_TUPLE
    ]
    identity_records = [
        record
        for record in records
        if record["name"] == ABI_IDENTITY_NAME
        and record["role"] == "abi-build-identity"
        and record.get("build_tuple") == ABI_BUILD_TUPLE
    ]
    if len(corpus_records) != 1 or len(identity_records) != 1:
        raise AbiReleasePolicyError("baseline manifest lacks one canonical ABI corpus and build identity")
    try:
        raw_identity = json.loads(files[ABI_IDENTITY_NAME].decode("ascii"))
        identity = validate_build_identity(raw_identity)
    except (UnicodeError, json.JSONDecodeError, AbiBuildError) as error:
        raise AbiReleasePolicyError(f"baseline ABI build identity is invalid: {error}") from error
    if _canonical_json(identity) != files[ABI_IDENTITY_NAME]:
        raise AbiReleasePolicyError("baseline ABI build identity JSON is not canonical")
    if identity["source"] != {"tag": version.tag, "commit": manifest["commit"]}:
        raise AbiReleasePolicyError("baseline ABI build identity source differs from its release")
    if identity["outputs"]["corpus_name"] != corpus_name:
        raise AbiReleasePolicyError("baseline ABI build identity names a different corpus")
    corpus_sha256 = _sha256(files[corpus_name])
    if identity["outputs"]["corpus_sha256"] != corpus_sha256:
        raise AbiReleasePolicyError("baseline ABI build identity does not bind the downloaded corpus")
    return {
        "schema_version": 1,
        "release_id": selection.baseline.database_id,
        "tag": version.tag,
        "abi_line": version.comparison_series,
        "build_tuple": ABI_BUILD_TUPLE,
        "manifest_asset_id": by_name["release-manifest.json"].asset_id,
        "manifest_sha256": by_name["release-manifest.json"].sha256,
        "corpus_asset_id": by_name[corpus_name].asset_id,
        "corpus_name": corpus_name,
        "corpus_sha256": corpus_sha256,
        "identity_asset_id": by_name[ABI_IDENTITY_NAME].asset_id,
        "identity_name": ABI_IDENTITY_NAME,
        "identity_sha256": _sha256(files[ABI_IDENTITY_NAME]),
        "environment_sha256": identity["environment_sha256"],
    }


def _validate_diff_binding(
    value: object,
    selection: BaselineSelection,
    *,
    baseline_corpus_sha256: str,
    candidate_corpus_sha256: str,
    report_sha256: str,
) -> int:
    if selection.baseline is None:
        raise AbiReleasePolicyError("abidiff cannot run for a first comparison-series baseline")
    result = _exact_mapping(value, ABIDIFF_RESULT_FIELDS, "abidiff result")
    expected = {
        "schema_version": 1,
        "tool": "abidiff",
        "tool_version": "2.4",
        "baseline_tag": selection.baseline.version.tag,
        "candidate_tag": selection.current.tag,
        "abi_line": selection.current.comparison_series,
        "baseline_sha256": baseline_corpus_sha256,
        "candidate_sha256": candidate_corpus_sha256,
        "report_sha256": report_sha256,
    }
    if any(not isinstance(digest, str) or DIGEST.fullmatch(digest) is None for digest in (
        baseline_corpus_sha256,
        candidate_corpus_sha256,
        report_sha256,
    )):
        raise AbiReleasePolicyError("abidiff evidence digest is malformed")
    if any(result[field] != expected[field] for field in expected):
        raise AbiReleasePolicyError("abidiff result is not bound to exact baseline evidence")
    exit_code = result["exit_code"]
    if type(exit_code) is not int or not 0 <= exit_code <= 255:
        raise AbiReleasePolicyError("abidiff exit code is malformed")
    if exit_code & ~ABIDIFF_KNOWN_STATUS_BITS:
        raise AbiReleasePolicyError("abidiff returned unknown status bits")
    if exit_code & (ABIDIFF_ERROR | ABIDIFF_USAGE_ERROR):
        raise AbiReleasePolicyError("abidiff failed or reported a usage error")
    if exit_code & ABIDIFF_INCOMPATIBLE_CHANGE:
        if not exit_code & ABIDIFF_ABI_CHANGE:
            raise AbiReleasePolicyError(
                "abidiff returned an internally inconsistent incompatible-change status"
            )
        if selection.current.major != 0:
            raise AbiReleasePolicyError(
                "an incompatible ABI change cannot be excepted after 1.0"
            )
    return exit_code


def authorize_abidiff(
    result: object,
    selection: BaselineSelection,
    *,
    baseline_identity_bytes: bytes,
    candidate_identity_bytes: bytes,
    report_bytes: bytes,
    policy: object,
) -> str | None:
    """Accept no change, or one exact checked-in reviewed exception."""

    if selection.baseline is None:
        raise AbiReleasePolicyError(
            "a first comparison-series baseline cannot authorize an abidiff result"
        )
    normalized_policy = load_policy(policy)
    try:
        baseline_raw = json.loads(baseline_identity_bytes.decode("ascii"))
        candidate_raw = json.loads(candidate_identity_bytes.decode("ascii"))
        baseline, candidate = require_compatible_environments(baseline_raw, candidate_raw)
    except (UnicodeError, json.JSONDecodeError, AbiBuildError) as error:
        raise AbiReleasePolicyError(f"ABI build identities are not comparable: {error}") from error
    if _canonical_json(baseline) != baseline_identity_bytes or _canonical_json(candidate) != candidate_identity_bytes:
        raise AbiReleasePolicyError("ABI build identity JSON is not canonical")
    if baseline["source"]["tag"] != selection.baseline.version.tag:
        raise AbiReleasePolicyError("baseline identity tag differs from selected release")
    if candidate["source"]["tag"] != selection.current.tag:
        raise AbiReleasePolicyError("candidate identity tag differs from candidate release")
    baseline_corpus = baseline["outputs"]["corpus_sha256"]
    candidate_corpus = candidate["outputs"]["corpus_sha256"]
    report_digest = _sha256(report_bytes)
    exit_code = _validate_diff_binding(
        result,
        selection,
        baseline_corpus_sha256=baseline_corpus,
        candidate_corpus_sha256=candidate_corpus,
        report_sha256=report_digest,
    )
    if exit_code == 0:
        return None
    if exit_code not in {
        ABIDIFF_ABI_CHANGE,
        ABIDIFF_ABI_CHANGE | ABIDIFF_INCOMPATIBLE_CHANGE,
    }:
        raise AbiReleasePolicyError("abidiff returned an unsupported nonzero status")
    expected_exception = {
        "baseline_tag": selection.baseline.version.tag,
        "candidate_tag": selection.current.tag,
        "abi_line": selection.current.comparison_series,
        "baseline_corpus_sha256": baseline_corpus,
        "candidate_corpus_sha256": candidate_corpus,
        "baseline_build_identity_sha256": _sha256(baseline_identity_bytes),
        "candidate_build_identity_sha256": _sha256(candidate_identity_bytes),
        "report_sha256": report_digest,
        "abidiff_version": "2.4",
        "exit_code": exit_code,
    }
    matches = [
        entry
        for entry in normalized_policy["exceptions"]
        if all(entry[field] == value for field, value in expected_exception.items())
    ]
    if len(matches) != 1:
        raise AbiReleasePolicyError(
            "an ABI change requires one exact checked-in reviewed exception"
        )
    return str(matches[0]["id"])


def compare_corpora(
    selection: BaselineSelection,
    *,
    baseline_corpus: Path,
    candidate_corpus: Path,
    baseline_identity: Path,
    candidate_identity: Path,
    report: Path,
    result_path: Path,
    policy: object,
) -> str | None:
    """Run the exact conservative abidiff command and authorize its result."""

    if selection.baseline is None:
        raise AbiReleasePolicyError("a first comparison-series baseline must not run abidiff")
    baseline_identity_bytes = baseline_identity.read_bytes()
    candidate_identity_bytes = candidate_identity.read_bytes()
    try:
        baseline_value = validate_build_identity(json.loads(baseline_identity_bytes.decode("ascii")))
        candidate_value = validate_build_identity(json.loads(candidate_identity_bytes.decode("ascii")))
        require_compatible_environments(baseline_value, candidate_value)
    except (UnicodeError, json.JSONDecodeError, AbiBuildError) as error:
        raise AbiReleasePolicyError(f"ABI identities cannot be compared: {error}") from error
    if sha256_file(baseline_corpus) != baseline_value["outputs"]["corpus_sha256"]:
        raise AbiReleasePolicyError("baseline corpus differs from its build identity")
    if sha256_file(candidate_corpus) != candidate_value["outputs"]["corpus_sha256"]:
        raise AbiReleasePolicyError("candidate corpus differs from its build identity")
    executable_name = shutil.which("abidiff")
    if executable_name is None:
        raise AbiReleasePolicyError("libabigail abidiff is unavailable")
    executable = Path(executable_name).resolve()
    abigail = next(
        component for component in candidate_value["components"] if component["name"] == "libabigail"
    )
    binary_records = [
        artifact for artifact in abigail["artifacts"] if artifact["name"] == "abidiff"
    ]
    if len(binary_records) != 1:
        raise AbiReleasePolicyError("candidate build identity lacks one exact abidiff binary")
    expected_binary = binary_records[0]["sha256"]
    if sha256_file(executable) != expected_binary:
        raise AbiReleasePolicyError("abidiff executable differs from the candidate build identity")
    version_output = subprocess.run(
        [str(executable), "--version"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    ).stdout
    if re.search(r"(?<![0-9])2\.4(?![0-9])", version_output) is None:
        raise AbiReleasePolicyError("abidiff is not canonical libabigail 2.4")
    with tempfile.TemporaryDirectory() as temporary:
        comparison = Path(temporary)
        (comparison / "baseline.abi.xml").write_bytes(baseline_corpus.read_bytes())
        (comparison / "candidate.abi.xml").write_bytes(candidate_corpus.read_bytes())
        completed = subprocess.run(
            [
                str(executable),
                "--no-default-suppression",
                "baseline.abi.xml",
                "candidate.abi.xml",
            ],
            cwd=comparison,
            env={**os.environ, "LANG": "C", "LC_ALL": "C", "TZ": "UTC"},
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=1800,
        )
    report.write_bytes(completed.stdout)
    result = {
        "schema_version": 1,
        "tool": "abidiff",
        "tool_version": "2.4",
        "baseline_tag": selection.baseline.version.tag,
        "candidate_tag": selection.current.tag,
        "abi_line": selection.current.comparison_series,
        "baseline_sha256": baseline_value["outputs"]["corpus_sha256"],
        "candidate_sha256": candidate_value["outputs"]["corpus_sha256"],
        "report_sha256": _sha256(completed.stdout),
        "exit_code": completed.returncode,
    }
    _write_json(result_path, result)
    return authorize_abidiff(
        result,
        selection,
        baseline_identity_bytes=baseline_identity_bytes,
        candidate_identity_bytes=candidate_identity_bytes,
        report_bytes=completed.stdout,
        policy=policy,
    )


def verify_gpg_signatures(
    directory: Path,
    *,
    public_key: Path,
    trusted_fingerprints: Sequence[str],
    primary_fingerprint: str,
    artifact_fingerprint: str,
) -> None:
    """Verify the two baseline control signatures with the approved subkey."""

    fingerprints = tuple(trusted_fingerprints)
    if (
        len(fingerprints) != 3
        or len(set(fingerprints)) != 3
        or any(_FINGERPRINT.fullmatch(value) is None for value in fingerprints)
        or primary_fingerprint not in fingerprints
        or artifact_fingerprint not in fingerprints
        or primary_fingerprint == artifact_fingerprint
    ):
        raise AbiReleasePolicyError("trusted GPG fingerprint roles are malformed")
    with tempfile.TemporaryDirectory() as temporary:
        home = Path(temporary)
        environment = {**os.environ, "GNUPGHOME": str(home)}
        shown = subprocess.run(
            [
                "gpg",
                "--batch",
                "--with-colons",
                "--import-options",
                "show-only",
                "--import",
                str(public_key),
            ],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=30,
        ).stdout.splitlines()
        actual = {line.split(":")[9] for line in shown if line.startswith("fpr:")}
        if actual != set(fingerprints):
            raise AbiReleasePolicyError("baseline verification key fingerprint set is not exact")
        subprocess.run(
            ["gpg", "--batch", "--import", str(public_key)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=30,
        )
        for signed_name in ("release-manifest.json", "SHA256SUMS"):
            signed = directory / signed_name
            signature = directory / f"{signed_name}.asc"
            result = subprocess.run(
                [
                    "gpg",
                    "--batch",
                    "--no-auto-key-retrieve",
                    "--status-fd",
                    "1",
                    "--verify",
                    str(signature),
                    str(signed),
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
                timeout=30,
            )
            if result.returncode:
                raise AbiReleasePolicyError(f"baseline signature verification failed: {signed_name}")
            try:
                validate_status(
                    result.stdout,
                    signing_fingerprint=artifact_fingerprint,
                    primary_fingerprint=primary_fingerprint,
                )
            except ValueError as error:
                raise AbiReleasePolicyError(
                    f"baseline signature identity is invalid: {signed_name}: {error}"
                ) from error


def _read_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, value: object) -> None:
    path.write_bytes(_canonical_json(value))


def _write_selection_outputs(path: Path, selection: BaselineSelection) -> None:
    baseline = selection.baseline
    values = {
        "has_baseline": "true" if baseline is not None else "false",
        "abi_line": selection.current.comparison_series,
        "baseline_tag": baseline.version.tag if baseline is not None else "",
        "baseline_release_id": str(baseline.database_id) if baseline is not None else "",
    }
    with path.open("a", encoding="ascii") as stream:
        for name, value in values.items():
            stream.write(f"{name}={value}\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    select = commands.add_parser("select")
    select.add_argument("--tag", required=True)
    select.add_argument("--catalog", type=Path, required=True)
    select.add_argument("--policy", type=Path, required=True)
    select.add_argument("--selection", type=Path, required=True)
    select.add_argument("--decision", type=Path, required=True)
    select.add_argument("--github-output", type=Path)

    downloads = commands.add_parser("plan-downloads")
    downloads.add_argument("--selection", type=Path, required=True)
    downloads.add_argument("--assets", type=Path, required=True)
    downloads.add_argument("--output", type=Path, required=True)

    bundle = commands.add_parser("verify-bundle")
    bundle.add_argument("--selection", type=Path, required=True)
    bundle.add_argument("--assets", type=Path, required=True)
    bundle.add_argument("--directory", type=Path, required=True)
    bundle.add_argument("--public-key", type=Path, required=True)
    bundle.add_argument("--primary-fingerprint", required=True)
    bundle.add_argument("--tag-fingerprint", required=True)
    bundle.add_argument("--artifact-fingerprint", required=True)
    bundle.add_argument("--output", type=Path, required=True)

    download = commands.add_parser("download")
    download.add_argument("--plan", type=Path, required=True)
    download.add_argument("--repository", required=True)
    download.add_argument("--directory", type=Path, required=True)

    diff = commands.add_parser("authorize-diff")
    diff.add_argument("--selection", type=Path, required=True)
    diff.add_argument("--result", type=Path, required=True)
    diff.add_argument("--report", type=Path, required=True)
    diff.add_argument("--baseline-identity", type=Path, required=True)
    diff.add_argument("--candidate-identity", type=Path, required=True)
    diff.add_argument("--policy", type=Path, required=True)
    diff.add_argument("--output", type=Path, required=True)

    compare = commands.add_parser("compare")
    compare.add_argument("--selection", type=Path, required=True)
    compare.add_argument("--baseline-corpus", type=Path, required=True)
    compare.add_argument("--candidate-corpus", type=Path, required=True)
    compare.add_argument("--baseline-identity", type=Path, required=True)
    compare.add_argument("--candidate-identity", type=Path, required=True)
    compare.add_argument("--report", type=Path, required=True)
    compare.add_argument("--result", type=Path, required=True)
    compare.add_argument("--policy", type=Path, required=True)
    compare.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.command == "select":
            selection = select_baseline(
                args.tag,
                normalize_release_catalog(_read_json(args.catalog)),
            )
            policy = _read_json(args.policy)
            _write_json(args.selection, selection.to_mapping())
            if args.github_output is not None:
                _write_selection_outputs(args.github_output, selection)
            if selection.baseline is None:
                authorization = authorize_first_baseline(selection, policy)
                _write_json(
                    args.decision,
                    {
                        "schema_version": 1,
                        "status": "first-baseline",
                        "tag": selection.current.tag,
                        "abi_line": selection.current.comparison_series,
                        "authorization": authorization,
                        "assets": [],
                    },
                )
            else:
                _write_json(
                    args.decision,
                    {
                        "schema_version": 1,
                        "status": "selected",
                        "tag": selection.current.tag,
                        "abi_line": selection.current.comparison_series,
                        "baseline_tag": selection.baseline.version.tag,
                        "baseline_release_id": selection.baseline.database_id,
                    },
                )
        elif args.command == "plan-downloads":
            selection = BaselineSelection.from_mapping(_read_json(args.selection))
            _write_json(args.output, baseline_download_plan(selection, _read_json(args.assets)))
        elif args.command == "download":
            download_planned_assets(
                _read_json(args.plan),
                repository=args.repository,
                directory=args.directory,
            )
        elif args.command == "verify-bundle":
            selection = BaselineSelection.from_mapping(_read_json(args.selection))
            verify_gpg_signatures(
                args.directory,
                public_key=args.public_key,
                trusted_fingerprints=(
                    args.primary_fingerprint,
                    args.tag_fingerprint,
                    args.artifact_fingerprint,
                ),
                primary_fingerprint=args.primary_fingerprint,
                artifact_fingerprint=args.artifact_fingerprint,
            )
            # GPG authenticates the exact local files first.  The pure verifier
            # then binds those bytes to signed checksums and GitHub server
            # digests before any evidence is written.
            evidence = verify_baseline_bundle(
                selection,
                _read_json(args.assets),
                args.directory,
                signature_verifier=lambda _signed, _signature: None,
                expected_signers={
                    "primary_fingerprint": args.primary_fingerprint,
                    "tag_subkey_fingerprint": args.tag_fingerprint,
                    "artifact_subkey_fingerprint": args.artifact_fingerprint,
                },
            )
            _write_json(args.output, evidence)
        elif args.command == "authorize-diff":
            selection = BaselineSelection.from_mapping(_read_json(args.selection))
            exception_id = authorize_abidiff(
                _read_json(args.result),
                selection,
                baseline_identity_bytes=args.baseline_identity.read_bytes(),
                candidate_identity_bytes=args.candidate_identity.read_bytes(),
                report_bytes=args.report.read_bytes(),
                policy=_read_json(args.policy),
            )
            _write_json(
                args.output,
                {
                    "schema_version": 1,
                    "status": "no-change" if exception_id is None else "reviewed-exception",
                    "exception_id": exception_id,
                },
            )
        else:
            selection = BaselineSelection.from_mapping(_read_json(args.selection))
            exception_id = compare_corpora(
                selection,
                baseline_corpus=args.baseline_corpus,
                candidate_corpus=args.candidate_corpus,
                baseline_identity=args.baseline_identity,
                candidate_identity=args.candidate_identity,
                report=args.report,
                result_path=args.result,
                policy=_read_json(args.policy),
            )
            _write_json(
                args.output,
                {
                    "schema_version": 1,
                    "status": "no-change" if exception_id is None else "reviewed-exception",
                    "exception_id": exception_id,
                },
            )
    except (
        AbiPolicyError,
        AbiBuildError,
        AbiReleasePolicyError,
        OSError,
        UnicodeError,
        json.JSONDecodeError,
        subprocess.CalledProcessError,
    ) as error:
        raise SystemExit(f"abi-policy: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
