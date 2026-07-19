"""Retained validators for immutable release-manifest v2 anchors."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
from typing import Mapping

from .model import SemVer, ValidationError
from .publication_contract import CONTRACT_ASSET, load_publication_contract
from .verify_candidate import _verify_signature


_DIGEST = re.compile(r"[0-9a-f]{64}")
_FINGERPRINT = re.compile(r"[0-9A-F]{40}|[0-9A-F]{64}")
_ASSET_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}")
_MANIFEST_V2_FIELDS = frozenset(
    {
        "schema_version", "package", "version", "tag", "commit",
        "source_tree_sha256", "release_ledger", "signers",
        "channel_capabilities", "payloads", "dependency_closure",
        "conan_requirements", "provenance_subjects",
    }
)
_PAYLOAD_V2_FIELDS = frozenset({"name", "size", "sha256", "role"})
_PAYLOAD_V2_OPTIONAL_FIELDS = frozenset({"build_tuple"})
_ROUTE_V1_FIELDS = frozenset(
    {
        "asset", "format", "route_id", "distribution", "release",
        "target_architecture", "package_name", "package_version",
        "package_architecture", "build_tuple", "identity_asset",
    }
)
_CONTROL_ASSETS_V2 = frozenset(
    {"release-manifest.json", "release-manifest.json.asc", "SHA256SUMS", "SHA256SUMS.asc"}
)
_STABLE_CAPABILITIES_V2 = (
    "github", "conan2", "apt", "rpm", "aur", "homebrew", "chocolatey"
)


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValidationError(f"historical release JSON contains duplicate field: {key}")
        value[key] = item
    return value


def _json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_strict_pairs)
    except (json.JSONDecodeError, UnicodeError) as error:
        raise ValidationError(f"historical release JSON is malformed: {path.name}") from error


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _payloads_v2(value: object) -> dict[str, Mapping[str, object]]:
    if not isinstance(value, list) or not value:
        raise ValidationError("historical release payload inventory is missing")
    records: dict[str, Mapping[str, object]] = {}
    for record in value:
        if (
            not isinstance(record, Mapping)
            or not _PAYLOAD_V2_FIELDS <= set(record)
            or set(record) - _PAYLOAD_V2_FIELDS - _PAYLOAD_V2_OPTIONAL_FIELDS
        ):
            raise ValidationError("historical release payload record is malformed")
        name = record.get("name")
        size = record.get("size")
        digest = record.get("sha256")
        role = record.get("role")
        build_tuple = record.get("build_tuple")
        if (
            not isinstance(name, str)
            or _ASSET_NAME.fullmatch(name) is None
            or PurePosixPath(name).name != name
            or name in records
            or type(size) is not int
            or size < 1
            or not isinstance(digest, str)
            or _DIGEST.fullmatch(digest) is None
            or not isinstance(role, str)
            or not role
            or (build_tuple is not None and not isinstance(build_tuple, str))
        ):
            raise ValidationError("historical release payload identity is malformed")
        records[name] = record
    if list(records) != sorted(records):
        raise ValidationError("historical release payload records are noncanonical")
    return records


def _validate_routes(directory: Path, records: Mapping[str, Mapping[str, object]], contract) -> None:
    routes = _json(directory / "cloudsmith-routes.json")
    if not isinstance(routes, list) or not routes:
        raise ValidationError("historical native route inventory is malformed")
    target_ids = {target.id for target in contract.native_targets}
    seen_assets: set[str] = set()
    packages: dict[str, int] = {target_id: 0 for target_id in target_ids}
    for route in routes:
        if (
            not isinstance(route, Mapping)
            or set(route) != _ROUTE_V1_FIELDS
            or any(not isinstance(route[field], str) or not route[field] for field in route)
            or route["route_id"] not in target_ids
            or route["asset"] not in records
            or route["identity_asset"] not in records
            or route["asset"] in seen_assets
        ):
            raise ValidationError("historical native route record is malformed")
        seen_assets.add(route["asset"])
        packages[str(route["route_id"])] += 1
    for target in contract.native_targets:
        expected = 3 if target.format == "apt" or target.architecture != "x86_64" else 4
        if packages[target.id] != expected:
            raise ValidationError("historical native route package inventory is incomplete")


def _validate_checksums(directory: Path, actual_files: set[str]) -> None:
    checksums: dict[str, str] = {}
    for line in (directory / "SHA256SUMS").read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]{0,199})", line)
        if match is None or match.group(2) in checksums:
            raise ValidationError("historical SHA256SUMS record is malformed")
        checksums[match.group(2)] = match.group(1)
    expected = actual_files - {"SHA256SUMS", "SHA256SUMS.asc"}
    if set(checksums) != expected:
        raise ValidationError("historical SHA256SUMS inventory is incomplete")
    for name, digest in checksums.items():
        if _sha256(directory / name) != digest:
            raise ValidationError(f"historical checksum differs: {name}")


def verify_historical_candidate_v2(
    directory: Path,
    *,
    tag: str,
    commit: str,
    ledger_issue: str,
    repository: str,
    signers: Mapping[str, str],
    public_key: Path,
) -> str:
    """Verify a v2 anchor using only retained schemas and signed release data."""

    version = SemVer.from_tag(tag)
    manifest_path = directory / "release-manifest.json"
    manifest = _json(manifest_path)
    capabilities = ["github"] if version.is_prerelease else list(_STABLE_CAPABILITIES_V2)
    expected_ledger = {
        "issue_id": ledger_issue,
        "issue_url": f"https://github.com/{repository}/issues/{ledger_issue}",
    }
    if (
        not isinstance(manifest, Mapping)
        or set(manifest) != _MANIFEST_V2_FIELDS
        or manifest.get("schema_version") != 2
        or manifest.get("package") != "mcp-cpp-sdk"
        or manifest.get("version") != str(version)
        or manifest.get("tag") != tag
        or manifest.get("commit") != commit
        or not isinstance(manifest.get("source_tree_sha256"), str)
        or _DIGEST.fullmatch(str(manifest["source_tree_sha256"])) is None
        or manifest.get("release_ledger") != expected_ledger
        or manifest.get("signers") != dict(signers)
        or manifest.get("channel_capabilities") != capabilities
    ):
        raise ValidationError("historical release manifest v2 identity is not exact")
    if any(
        not isinstance(value, str) or _FINGERPRINT.fullmatch(value) is None
        for value in signers.values()
    ):
        raise ValidationError("historical signer fingerprints are malformed")
    records = _payloads_v2(manifest.get("payloads"))
    provenance = manifest.get("provenance_subjects")
    expected_provenance = [
        {"name": name, "digest": {"sha256": records[name]["sha256"]}}
        for name in sorted(records)
    ]
    if provenance != expected_provenance:
        raise ValidationError("historical provenance subjects are not exact")

    detached = {f"{name}.asc" for name in records if name.endswith((".tar.gz", ".zip"))}
    expected_files = set(records) | _CONTROL_ASSETS_V2 | detached
    actual_paths = tuple(directory.iterdir())
    if any(path.is_symlink() or not path.is_file() for path in actual_paths):
        raise ValidationError("historical release contains a non-regular asset")
    actual_files = {path.name for path in actual_paths}
    if actual_files != expected_files:
        raise ValidationError("historical release asset inventory is not exact")
    for name, record in records.items():
        path = directory / name
        if path.stat().st_size != record["size"] or _sha256(path) != record["sha256"]:
            raise ValidationError(f"historical release payload differs: {name}")
    if (directory / "release-signing-key.asc").read_bytes() != public_key.read_bytes():
        raise ValidationError("historical release key differs from its trusted key")
    _validate_checksums(directory, actual_files)

    if not version.is_prerelease:
        contract = load_publication_contract(directory / CONTRACT_ASSET)
        if manifest.get("conan_requirements") != list(contract.conan_requirements):
            raise ValidationError("historical Conan requirements differ from signed contract")
        required = {CONTRACT_ASSET, "release-signing-key.asc"}
        required.update(asset for assets in contract.channel_assets.values() for asset in assets)
        if not required <= set(records):
            raise ValidationError("historical publication contract assets are incomplete")
        _validate_routes(directory, records, contract)

    with tempfile.TemporaryDirectory() as temporary:
        home = Path(temporary)
        imported = subprocess.run(
            ["gpg", "--batch", "--homedir", str(home), "--import", str(public_key)],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if imported.returncode:
            raise ValidationError("historical release public key import failed")
        signed_assets = [(directory / "SHA256SUMS", directory / "SHA256SUMS.asc")]
        signed_assets.extend(
            (directory / name, directory / f"{name}.asc")
            for name in sorted(set(records) | {"release-manifest.json"})
            if name == "release-manifest.json" or name.endswith((".tar.gz", ".zip"))
        )
        for signed, signature in signed_assets:
            _verify_signature(
                home=home,
                signed=signed,
                signature=signature,
                artifact_fingerprint=signers["artifact_subkey_fingerprint"],
                primary_fingerprint=signers["primary_fingerprint"],
            )
    return _sha256(manifest_path)
