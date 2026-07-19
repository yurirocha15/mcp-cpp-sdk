"""Assemble downloaded release parts into one validated unsigned boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
from typing import Sequence

from .artifacts import (
    ABI_BUILD_IDENTITY_NAME,
    ABI_BUILD_TUPLE,
    STABLE_CHANNEL_CAPABILITIES,
    WINDOWS_BUILD_TUPLE,
    ArtifactRecord,
    NativeRoute,
    build_release_manifest,
    build_sha256sums,
    canonical_json_bytes,
    load_conan_requirements,
    load_native_targets,
    public_dependency_closure,
    validate_candidate_inventory,
    write_atomic,
)
from .abi_build import AbiBuildError, validate_candidate_pair
from .model import SemVer, ValidationError


_DIGEST_RE = re.compile(r"[0-9a-f]{64}")
_COMMIT_RE = re.compile(r"[0-9a-f]{40}")
_METADATA_FIELDS = frozenset(
    {"commit", "source_date_epoch", "source_tree_sha256", "source_sha256"}
)
def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValidationError(f"JSON object contains duplicate field: {key!r}")
        value[key] = item
    return value


def _reject_constant(value: str) -> None:
    raise ValidationError(f"JSON document contains non-finite number: {value}")


def _read_json(path: Path) -> object:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
        )
    except (json.JSONDecodeError, UnicodeError) as error:
        raise ValidationError(f"malformed JSON file: {path.name}") from error


def _reset_directory(path: Path) -> None:
    if path.exists():
        if path.is_symlink() or not path.is_dir():
            raise ValidationError("unsigned output must be a real directory when it exists")
        shutil.rmtree(path)
    path.mkdir(parents=True)


def _part_files(parts: Path) -> tuple[Path, ...]:
    if parts.is_symlink() or not parts.is_dir():
        raise ValidationError("release parts must be a real directory")
    entries = tuple(sorted(parts.iterdir(), key=lambda path: path.name))
    if not entries:
        raise ValidationError("release parts directory is empty")
    for entry in entries:
        if entry.is_symlink() or not entry.is_file():
            raise ValidationError(f"release part is not a regular file: {entry.name}")
    return entries


def _require_separate_directories(parts: Path, output: Path) -> None:
    parts_root = parts.resolve()
    output_root = output.resolve()
    if (
        parts_root == output_root
        or parts_root in output_root.parents
        or output_root in parts_root.parents
    ):
        raise ValidationError("release parts and unsigned output must be separate directories")


def _load_metadata(path: Path, *, commit: str, source_archive: Path) -> dict[str, object]:
    value = _read_json(path)
    if not isinstance(value, dict) or set(value) != _METADATA_FIELDS:
        raise ValidationError("core metadata fields are incomplete or unexpected")
    if value["commit"] != commit or _COMMIT_RE.fullmatch(commit) is None:
        raise ValidationError("core metadata commit differs from the selected release commit")
    if (
        not isinstance(value["source_date_epoch"], int)
        or isinstance(value["source_date_epoch"], bool)
        or value["source_date_epoch"] < 1
    ):
        raise ValidationError("core metadata source date epoch is invalid")
    if (
        not isinstance(value["source_tree_sha256"], str)
        or _DIGEST_RE.fullmatch(value["source_tree_sha256"]) is None
    ):
        raise ValidationError("core metadata source tree digest is invalid")
    if not source_archive.is_file() or source_archive.is_symlink():
        raise ValidationError("canonical source archive is missing")
    source_digest = hashlib.sha256(source_archive.read_bytes()).hexdigest()
    if value["source_sha256"] != source_digest:
        raise ValidationError("core metadata source archive digest does not match the release part")
    return value


def _load_routes(paths: Sequence[Path]) -> tuple[NativeRoute, ...]:
    routes: list[NativeRoute] = []
    for path in paths:
        value = _read_json(path)
        if not isinstance(value, list) or not value:
            raise ValidationError("route fragment must be a non-empty list")
        routes.extend(NativeRoute.from_mapping(item) for item in value)
    return tuple(
        sorted(
            routes,
            key=lambda route: (route.route_id, route.package_name, route.asset),
        )
    )


def _artifact_role(path: Path) -> str:
    if path.name.endswith((".tar.gz", ".zip")):
        return "source-or-binary-archive"
    if path.name.endswith((".deb", ".rpm", ".nupkg")):
        return "native-package"
    if path.name.endswith((".spdx.json", ".cdx.json")):
        return "sbom"
    if path.name.endswith(".abi.xml"):
        return "abi-corpus"
    if path.name == ABI_BUILD_IDENTITY_NAME:
        return "abi-build-identity"
    if path.name.startswith("build-identity-") and path.name.endswith(".json"):
        return "build-identity"
    return "publisher-input"


def assemble_unsigned(
    *,
    root: Path,
    parts: Path,
    output: Path,
    version_text: str,
    tag: str,
    commit: str,
    primary_fingerprint: str,
    tag_subkey_fingerprint: str,
    artifact_subkey_fingerprint: str,
) -> None:
    """Validate flat release parts and construct the canonical unsigned bundle."""

    version = SemVer.parse(version_text)
    if tag != version.tag:
        raise ValidationError("tag does not match the release version")
    entries = _part_files(parts)
    metadata_path = parts / "core-metadata.json"
    if metadata_path not in entries:
        raise ValidationError("core metadata release part is missing")
    route_paths = tuple(
        path
        for path in entries
        if path.name.startswith("route-") and path.suffix == ".json"
    )
    routes = _load_routes(route_paths) if route_paths else ()

    source_archive = parts / f"mcp-cpp-sdk-{version}.tar.gz"
    metadata = _load_metadata(metadata_path, commit=commit, source_archive=source_archive)
    internal_names = {"core-metadata.json", *(path.name for path in route_paths)}
    _require_separate_directories(parts, output)
    _reset_directory(output)
    for path in entries:
        if path.name in internal_names:
            continue
        destination = output / path.name
        if destination.exists():
            raise ValidationError(f"duplicate release asset name: {path.name}")
        shutil.copyfile(path, destination)

    route_mappings = [route.to_mapping() for route in routes]
    if routes:
        write_atomic(output / "cloudsmith-routes.json", canonical_json_bytes(route_mappings))
    route_tuples = {route.asset: route.build_tuple for route in routes}
    identity_tuples = {route.identity_asset: route.build_tuple for route in routes}
    if len(route_tuples) != len(routes) or len(identity_tuples) != len(
        {route.route_id for route in routes}
    ):
        raise ValidationError("native route build identity inventory is ambiguous")

    payloads: list[ArtifactRecord] = []
    for path in sorted(output.iterdir(), key=lambda item: item.name):
        if path.name == "RELEASE_NOTES.md":
            continue
        build_tuple = route_tuples.get(path.name) or identity_tuples.get(path.name)
        if path.name in {
            f"mcp-cpp-sdk-{version}-windows-x64-v143-md.zip",
            f"mcp-cpp-sdk.{version}.nupkg",
            "build-identity-windows-x64-v143-md.json",
        }:
            build_tuple = WINDOWS_BUILD_TUPLE
        if path.name == f"mcp-cpp-sdk-{version}-{ABI_BUILD_TUPLE}.abi.xml":
            build_tuple = ABI_BUILD_TUPLE
        if path.name == ABI_BUILD_IDENTITY_NAME:
            build_tuple = ABI_BUILD_TUPLE
        payloads.append(
            ArtifactRecord.from_path(
                path,
                role=_artifact_role(path),
                build_tuple=build_tuple,
            )
        )

    if not version.is_prerelease:
        abi_identity_path = output / ABI_BUILD_IDENTITY_NAME
        abi_corpus_path = output / f"mcp-cpp-sdk-{version}-{ABI_BUILD_TUPLE}.abi.xml"
        try:
            validate_candidate_pair(
                abi_identity_path,
                abi_corpus_path,
                tag=tag,
                commit=commit,
            )
        except AbiBuildError as error:
            raise ValidationError(f"ABI build identity is invalid: {error}") from error

    provenance_subjects = [
        {"name": record.name, "digest": {"sha256": record.sha256}}
        for record in sorted(payloads, key=lambda record: record.name)
    ]
    targets = load_native_targets(root / "packaging/targets.json")
    validate_candidate_inventory(
        version=version,
        payloads=payloads,
        routes=routes,
        targets=targets,
        provenance_subjects=provenance_subjects,
    )
    manifest = build_release_manifest(
        version=version,
        tag=tag,
        commit=commit,
        source_tree_sha256=str(metadata["source_tree_sha256"]),
        primary_fingerprint=primary_fingerprint,
        tag_subkey_fingerprint=tag_subkey_fingerprint,
        artifact_subkey_fingerprint=artifact_subkey_fingerprint,
        channel_capabilities=(
            ["github"] if version.is_prerelease else list(STABLE_CHANNEL_CAPABILITIES)
        ),
        payloads=payloads,
        dependency_closure=public_dependency_closure(),
        conan_requirements=list(
            load_conan_requirements(root / "packaging/conan-center/requirements.json")
        ),
        provenance_subjects=provenance_subjects,
    )
    write_atomic(output / "release-manifest.json", canonical_json_bytes(manifest))
    checksum_inputs = [
        path for path in output.iterdir() if path.is_file() and path.name != "RELEASE_NOTES.md"
    ]
    write_atomic(output / "UNSIGNED-SHA256SUMS", build_sha256sums(checksum_inputs))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--parts", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    parser.add_argument("--tag-subkey-fingerprint", required=True)
    parser.add_argument("--artifact-subkey-fingerprint", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        assemble_unsigned(
            root=args.root,
            parts=args.parts,
            output=args.output,
            version_text=args.version,
            tag=args.tag,
            commit=args.commit,
            primary_fingerprint=args.primary_fingerprint,
            tag_subkey_fingerprint=args.tag_subkey_fingerprint,
            artifact_subkey_fingerprint=args.artifact_subkey_fingerprint,
        )
    except (OSError, ValidationError) as error:
        raise SystemExit(f"assemble-unsigned: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
