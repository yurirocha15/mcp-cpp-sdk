#!/usr/bin/env python3
"""Verify a complete signed release candidate before any GitHub publication."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from release.artifacts import (  # noqa: E402
    ABI_BUILD_IDENTITY_NAME,
    ABI_BUILD_TUPLE,
    CONTROL_ASSET_NAMES,
    STABLE_CHANNEL_CAPABILITIES,
    ArtifactRecord,
    NativeRoute,
    load_native_targets,
    load_conan_requirements,
    public_dependency_closure,
    validate_candidate_inventory,
)
from release.abi_build import AbiBuildError, validate_candidate_pair  # noqa: E402
from release.model import SemVer, ValidationError  # noqa: E402
from release.publication_contract import (  # noqa: E402
    CONTRACT_ASSET,
    load_publication_contract,
)
from release.build_identity import (  # noqa: E402
    BuildIdentityError,
    load_and_validate_target_projection,
    validate_apt_build_identity,
    validate_rpm_build_identity,
    validate_windows_build_identity,
)
from release.native_builder import (  # noqa: E402
    load_builder_lock,
    validate_container_identity,
)


DIGEST = re.compile(r"[0-9a-f]{64}")
FINGERPRINT = re.compile(r"[0-9A-F]{40}|[0-9A-F]{64}")
MANIFEST_FIELDS = {
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
DEPENDENCY_CLOSURE = public_dependency_closure()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_dependency_closure(value: object) -> None:
    if value != DEPENDENCY_CLOSURE:
        raise ValidationError("release manifest dependency closure is not exact")


def validate_embedded_dependency_closure(value: object) -> None:
    """Validate retained schema-v2 closure without comparing today's dependencies."""

    if not isinstance(value, list) or not value:
        raise ValidationError("release manifest dependency closure is malformed")
    normalized: list[dict[str, str]] = []
    for item in value:
        if (
            not isinstance(item, dict)
            or set(item) != {"name", "minimum"}
            or any(not isinstance(item[field], str) or not item[field] for field in item)
        ):
            raise ValidationError("release manifest dependency closure is malformed")
        normalized.append({"name": item["name"], "minimum": item["minimum"]})
    if normalized != sorted(normalized, key=lambda item: item["name"]) or len(
        {item["name"] for item in normalized}
    ) != len(normalized):
        raise ValidationError("release manifest dependency closure is noncanonical")


def server_asset_digests(pages: object) -> dict[str, str]:
    """Flatten every paginated GitHub release asset into an exact digest map."""
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise ValidationError("GitHub release asset pages are malformed")
    assets: dict[str, str] = {}
    for page in pages:
        for asset in page:
            if not isinstance(asset, dict):
                raise ValidationError("GitHub release asset record is malformed")
            name = asset.get("name")
            digest = asset.get("digest")
            if not isinstance(name, str) or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}", name) is None:
                raise ValidationError("GitHub release contains an unsafe asset name")
            if not isinstance(digest, str) or re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is None:
                raise ValidationError("GitHub release asset lacks a canonical server-side digest")
            if name in assets:
                raise ValidationError("GitHub release contains duplicate asset names")
            assets[name] = digest.removeprefix("sha256:")
    return assets


def _parse_checksums(text: str) -> dict[str, str]:
    checksums: dict[str, str] = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]{0,199})", line)
        if match is None or match.group(2) in checksums:
            raise ValidationError("SHA256SUMS contains a malformed or duplicate record")
        checksums[match.group(2)] = match.group(1)
    if not checksums:
        raise ValidationError("SHA256SUMS is empty")
    return checksums


def _verify_signature(
    *, home: Path, signed: Path, signature: Path, artifact_fingerprint: str, primary_fingerprint: str
) -> None:
    result = subprocess.run(
        [
            "gpg",
            "--batch",
            "--homedir",
            str(home),
            "--no-auto-key-retrieve",
            "--status-fd",
            "1",
            "--verify",
            str(signature),
            str(signed),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    signatures = [line.split() for line in result.stdout.splitlines() if line.startswith("[GNUPG:] VALIDSIG ")]
    if result.returncode or len(signatures) != 1 or len(signatures[0]) < 12:
        raise ValidationError(f"signature verification failed: {signed.name}")
    if signatures[0][2] != artifact_fingerprint or signatures[0][-1] != primary_fingerprint:
        raise ValidationError(f"signature identity mismatch: {signed.name}")


def verify_candidate(
    directory: Path,
    *,
    tag: str,
    commit: str,
    ledger_issue: str,
    repository: str,
    primary_fingerprint: str,
    tag_fingerprint: str,
    artifact_fingerprint: str,
    public_key: Path,
    targets_path: Path,
    target_catalog_path: Path,
    native_builder_lock_path: Path,
    conan_requirements_path: Path,
    include_release_notes: bool,
    policy_source: str = "current",
) -> str:
    version = SemVer.from_tag(tag)
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise ValidationError("candidate commit is malformed")
    if not ledger_issue.isdecimal() or ledger_issue.startswith("0"):
        raise ValidationError("candidate ledger issue is malformed")
    if repository != "yurirocha15/mcp-cpp-sdk":
        raise ValidationError("candidate repository identity is unexpected")
    fingerprints = (primary_fingerprint, tag_fingerprint, artifact_fingerprint)
    if any(FINGERPRINT.fullmatch(value) is None for value in fingerprints) or len(set(fingerprints)) != 3:
        raise ValidationError("candidate signer fingerprints are malformed or duplicated")

    if policy_source not in {"current", "embedded"}:
        raise ValidationError("candidate policy source is unsupported")
    manifest_path = directory / "release-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    contract = None
    if not version.is_prerelease:
        contract = load_publication_contract(directory / CONTRACT_ASSET)
    expected_capabilities = ["github"] if version.is_prerelease else list(STABLE_CHANNEL_CAPABILITIES)
    expected_signers = {
        "primary_fingerprint": primary_fingerprint,
        "tag_subkey_fingerprint": tag_fingerprint,
        "artifact_subkey_fingerprint": artifact_fingerprint,
    }
    expected_ledger = {
        "issue_id": ledger_issue,
        "issue_url": f"https://github.com/{repository}/issues/{ledger_issue}",
    }
    if (
        not isinstance(manifest, dict)
        or set(manifest) != MANIFEST_FIELDS
        or manifest.get("schema_version") != 2
        or manifest.get("package") != "mcp-cpp-sdk"
        or manifest.get("version") != str(version)
        or manifest.get("tag") != tag
        or manifest.get("commit") != commit
        or not isinstance(manifest.get("source_tree_sha256"), str)
        or DIGEST.fullmatch(manifest["source_tree_sha256"]) is None
        or manifest.get("release_ledger") != expected_ledger
        or manifest.get("signers") != expected_signers
        or manifest.get("channel_capabilities") != expected_capabilities
        or (
            contract is not None
            and manifest.get("conan_requirements") != list(contract.conan_requirements)
        )
    ):
        raise ValidationError("release manifest identity or policy fields are not exact")
    if policy_source == "current" and manifest.get("conan_requirements") != list(
        load_conan_requirements(conan_requirements_path)
    ):
        raise ValidationError("release manifest differs from current Conan requirements")
    if policy_source == "current":
        validate_dependency_closure(manifest.get("dependency_closure"))
    else:
        validate_embedded_dependency_closure(manifest.get("dependency_closure"))
    payload_mappings = manifest.get("payloads")
    if not isinstance(payload_mappings, list):
        raise ValidationError("release manifest payload list is missing")
    payloads = tuple(ArtifactRecord.from_mapping(item) for item in payload_mappings)
    if list(payload_mappings) != [record.to_mapping() for record in sorted(payloads, key=lambda item: item.name)]:
        raise ValidationError("release manifest payload records are not canonically ordered")

    route_path = directory / "cloudsmith-routes.json"
    route_mappings = [] if version.is_prerelease else json.loads(route_path.read_text(encoding="utf-8"))
    if not isinstance(route_mappings, list):
        raise ValidationError("native route inventory is malformed")
    routes = tuple(NativeRoute.from_mapping(item) for item in route_mappings)
    if contract is None:
        targets = () if policy_source == "embedded" else load_native_targets(targets_path)
    else:
        targets = contract.native_targets
        if policy_source == "current":
            load_and_validate_target_projection(targets_path, target_catalog_path)
            current_targets = load_native_targets(targets_path)
            if tuple(target.to_mapping() for target in targets) != tuple(
                target.to_mapping() for target in current_targets
            ):
                raise ValidationError(
                    "signed publication contract differs from current native targets"
                )
            targets = current_targets
    if contract is None:
        builder_records = ()
    elif policy_source == "embedded":
        builder_records = contract.native_builders
    else:
        current_builders = load_builder_lock(
            native_builder_lock_path, require_resolved=True
        )
        builder_records = tuple(
            {
                "id": builder["id"],
                "image": builder["image"],
                "image_digest": builder["image_digest"],
            }
            for builder in current_builders
            if builder["id"] in {target.id for target in targets}
        )
        if builder_records != contract.native_builders:
            raise ValidationError(
                "signed publication contract differs from current native builders"
            )
    builders = {record["id"]: record for record in builder_records}
    validate_candidate_inventory(
        version=version,
        payloads=payloads,
        routes=routes,
        targets=targets,
        provenance_subjects=manifest.get("provenance_subjects"),
    )
    if not version.is_prerelease:
        for target in targets:
            identity_path = directory / f"build-identity-{target.id}.json"
            identity = json.loads(identity_path.read_text(encoding="ascii"))
            evidence = identity.get("evidence") if isinstance(identity, dict) else None
            facts = evidence.get("platform") if isinstance(evidence, dict) else None
            platform_identity = (
                validate_apt_build_identity(target.id, facts)
                if target.format == "apt"
                else validate_rpm_build_identity(target.id, facts)
            )
            builder = builders.get(target.id)
            if builder is None:
                raise ValidationError(f"signed build identity has no reviewed builder: {target.id}")
            validate_container_identity(
                identity,
                expected_platform_identity=platform_identity,
                expected_image=f"{builder['image']}@{builder['image_digest']}",
            )
        windows_identity = json.loads(
            (directory / "build-identity-windows-x64-v143-md.json").read_text(encoding="ascii")
        )
        windows_facts = windows_identity.get("evidence") if isinstance(windows_identity, dict) else None
        if isinstance(windows_facts, dict):
            windows_facts = dict(windows_facts)
            if windows_facts.pop("abi_version", None) != version.abi_version:
                raise ValidationError("signed Windows ABI version is not exact")
        if windows_identity != validate_windows_build_identity(
            windows_facts,
            expected_abi_version=version.abi_version,
        ):
            raise ValidationError("signed Windows build identity is not exact")
        try:
            validate_candidate_pair(
                directory / ABI_BUILD_IDENTITY_NAME,
                directory / f"mcp-cpp-sdk-{version}-{ABI_BUILD_TUPLE}.abi.xml",
                tag=tag,
                commit=commit,
            )
        except AbiBuildError as error:
            raise ValidationError(f"signed ABI build identity is not exact: {error}") from error

    if (directory / "release-signing-key.asc").read_bytes() != public_key.read_bytes():
        raise ValidationError("candidate verification key differs from the tagged public key")
    payload_names = {record.name for record in payloads}
    detached = {f"{name}.asc" for name in payload_names if name.endswith((".tar.gz", ".zip"))}
    expected_files = payload_names | CONTROL_ASSET_NAMES | detached
    if include_release_notes:
        expected_files.add("RELEASE_NOTES.md")
    actual_files = {path.name for path in directory.iterdir() if path.is_file()}
    if actual_files != expected_files:
        raise ValidationError("signed candidate file inventory is not exact")
    for record in payloads:
        path = directory / record.name
        if path.stat().st_size != record.size or sha256(path) != record.sha256:
            raise ValidationError(f"candidate payload identity mismatch: {record.name}")
    if include_release_notes and not (directory / "RELEASE_NOTES.md").read_text(encoding="utf-8").strip():
        raise ValidationError("release notes are empty")

    checksums = _parse_checksums((directory / "SHA256SUMS").read_text(encoding="ascii"))
    expected_checksum_files = actual_files - {"RELEASE_NOTES.md", "SHA256SUMS", "SHA256SUMS.asc"}
    if set(checksums) != expected_checksum_files:
        raise ValidationError("SHA256SUMS does not cover the exact signed candidate")
    for name, digest in checksums.items():
        if sha256(directory / name) != digest:
            raise ValidationError(f"candidate checksum mismatch: {name}")

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
            raise ValidationError("candidate public key import failed")
        signed_assets = [(directory / "SHA256SUMS", directory / "SHA256SUMS.asc")]
        signed_assets.extend(
            (directory / name, directory / f"{name}.asc")
            for name in sorted(payload_names | {"release-manifest.json"})
            if name == "release-manifest.json" or name.endswith((".tar.gz", ".zip"))
        )
        for signed, signature in signed_assets:
            _verify_signature(
                home=home,
                signed=signed,
                signature=signature,
                artifact_fingerprint=artifact_fingerprint,
                primary_fingerprint=primary_fingerprint,
            )
    return sha256(manifest_path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--ledger-issue", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    parser.add_argument("--tag-fingerprint", required=True)
    parser.add_argument("--artifact-fingerprint", required=True)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--targets", type=Path, required=True)
    parser.add_argument("--target-catalog", type=Path, required=True)
    parser.add_argument("--native-builder-lock", type=Path, required=True)
    parser.add_argument("--conan-requirements", type=Path, required=True)
    parser.add_argument("--release-notes", choices=("included", "excluded"), required=True)
    parser.add_argument("--policy-source", choices=("current", "embedded"), default="current")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    try:
        manifest = verify_candidate(
            args.directory,
            tag=args.tag,
            commit=args.commit,
            ledger_issue=args.ledger_issue,
            repository=args.repository,
            primary_fingerprint=args.primary_fingerprint,
            tag_fingerprint=args.tag_fingerprint,
            artifact_fingerprint=args.artifact_fingerprint,
            public_key=args.public_key,
            targets_path=args.targets,
            target_catalog_path=args.target_catalog,
            native_builder_lock_path=args.native_builder_lock,
            conan_requirements_path=args.conan_requirements,
            include_release_notes=args.release_notes == "included",
            policy_source=args.policy_source,
        )
        if args.github_output is not None:
            with args.github_output.open("a", encoding="utf-8") as output:
                output.write(f"manifest_sha256={manifest}\n")
    except (
        OSError, UnicodeError, json.JSONDecodeError, subprocess.SubprocessError,
        BuildIdentityError, ValidationError,
    ) as error:
        print(f"verify-candidate: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
