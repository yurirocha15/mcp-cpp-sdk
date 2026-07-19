"""Validate downloaded GitHub Release metadata and write the anchor handoff."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from typing import Any, Mapping, Sequence

from .github_publication import (
    PublicationPolicyError,
    _gh_json,
    verify_live_github_tag,
    verify_release_identity,
)
from .model import SemVer, ValidationError
from .historical_anchor import verify_historical_candidate_v2
from .trusted_signers import load_trusted_signers
from .verify_candidate import server_asset_digests, verify_candidate


_GITHUB_API_VERSION = "X-GitHub-Api-Version: 2026-03-10"
_ASSET_RESPONSE_LIMIT = 8 * 1024 * 1024
def _json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _file_digests(directory: Path) -> dict[str, str]:
    return {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in directory.iterdir()
        if path.is_file()
    }


def _write_anchor(
    directory: Path,
    *,
    repository: str,
    release_id: int,
    tag: str,
    commit: str,
    manifest_sha256: str,
) -> None:
    anchor = {
        "schema_version": 1,
        "repository": repository,
        "release_id": str(release_id),
        "tag": tag,
        "commit": commit,
        "manifest_sha256": manifest_sha256,
    }
    (directory / "ANCHOR.json").write_text(
        json.dumps(anchor, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _download_release(*, directory: Path, repository: str, tag: str) -> None:
    if directory.exists() or directory.is_symlink():
        raise ValidationError("release download directory must not already exist")
    completed = subprocess.run(
        [
            "gh",
            "release",
            "download",
            tag,
            "--repo",
            repository,
            "--dir",
            str(directory),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=600,
    )
    if completed.returncode != 0:
        raise ValidationError("GitHub release asset download failed")
    if directory.is_symlink() or not directory.is_dir():
        raise ValidationError("GitHub release download did not create a real directory")


def _release_metadata(*, repository: str, tag: str) -> tuple[object, object, object]:
    graphql = _gh_json(
        [
            "release",
            "view",
            tag,
            "--repo",
            repository,
            "--json",
            "databaseId,isDraft,isImmutable,isPrerelease,tagName",
        ],
        label="release-graphql",
    )
    rest = _gh_json(
        [
            "api",
            "-H",
            _GITHUB_API_VERSION,
            f"repos/{repository}/releases/tags/{tag}",
        ],
        label="release-rest",
    )
    if not isinstance(rest, Mapping):
        raise ValidationError("GitHub REST release response is not an object")
    release_id = rest.get("id")
    if type(release_id) is not int or release_id < 1:
        raise ValidationError("GitHub REST release response lacks a canonical ID")
    assets = _gh_json(
        [
            "api",
            "--paginate",
            "--slurp",
            "-H",
            _GITHUB_API_VERSION,
            f"repos/{repository}/releases/{release_id}/assets?per_page=100",
        ],
        label="release-assets",
        limit=_ASSET_RESPONSE_LIMIT,
    )
    return graphql, rest, assets


def download_and_verify_anchor(args: argparse.Namespace) -> tuple[int, str]:
    """Download and verify one immutable release through a tested orchestration path."""

    version = SemVer.parse(args.version)
    if version.tag != args.tag:
        raise ValidationError("anchor tag and version disagree")
    verify_live_github_tag(
        repository=args.repository,
        tag=args.tag,
        tag_object_sha=args.tag_object_sha,
        commit=args.commit,
    )
    _download_release(directory=args.directory, repository=args.repository, tag=args.tag)
    graphql, rest, assets = _release_metadata(repository=args.repository, tag=args.tag)
    release_id = verify_release_identity(
        graphql,
        rest,
        tag=args.tag,
        prerelease=version.is_prerelease,
    )
    local = _file_digests(args.directory)
    if server_asset_digests(assets) != local:
        raise ValidationError("downloaded release assets do not match server-side SHA-256 digests")
    manifest_sha256 = local.get("release-manifest.json")
    if manifest_sha256 is None:
        raise ValidationError("verified release manifest is missing")
    if args.anchor_existed == "true":
        manifest = _json(args.directory / "release-manifest.json")
        if not isinstance(manifest, Mapping):
            raise ValidationError("release manifest is not an object")
        embedded_key = args.directory / "release-signing-key.asc"
        signer = load_trusted_signers(args.trusted_signers).match(
            manifest.get("signers"), embedded_key.read_bytes()
        )
        verification_key = embedded_key
        primary_fingerprint = signer.primary_fingerprint
        tag_fingerprint = signer.tag_subkey_fingerprint
        artifact_fingerprint = signer.artifact_subkey_fingerprint
        policy_source = "embedded"
    else:
        active = load_trusted_signers(args.trusted_signers).active
        if (
            active.public_key.resolve() != args.public_key.resolve()
            or active.manifest_signers
            != {
                "primary_fingerprint": args.primary_fingerprint,
                "tag_subkey_fingerprint": args.tag_fingerprint,
                "artifact_subkey_fingerprint": args.artifact_fingerprint,
            }
        ):
            raise ValidationError("new anchor signer is not the active trusted signer")
        verification_key = args.public_key
        primary_fingerprint = args.primary_fingerprint
        tag_fingerprint = args.tag_fingerprint
        artifact_fingerprint = args.artifact_fingerprint
        policy_source = "current"
    candidate_manifest = (
        verify_historical_candidate_v2(
            args.directory,
            tag=args.tag,
            commit=args.commit,
            signers=signer.manifest_signers,
            public_key=verification_key,
        )
        if policy_source == "embedded"
        else verify_candidate(
            args.directory,
            tag=args.tag,
            commit=args.commit,
            primary_fingerprint=primary_fingerprint,
            tag_fingerprint=tag_fingerprint,
            artifact_fingerprint=artifact_fingerprint,
            public_key=verification_key,
            targets_path=args.targets,
            native_builder_lock_path=args.native_builder_lock,
            conan_requirements_path=args.conan_requirements,
            include_release_notes=False,
            policy_source=policy_source,
        )
    )
    if candidate_manifest != manifest_sha256:
        raise ValidationError("candidate and anchor manifest digests disagree")
    _write_anchor(
        args.directory,
        repository=args.repository,
        release_id=release_id,
        tag=args.tag,
        commit=args.commit,
        manifest_sha256=manifest_sha256,
    )
    return release_id, manifest_sha256


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("download-and-verify",))
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--trusted-signers", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag-object-sha", required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    parser.add_argument("--tag-fingerprint", required=True)
    parser.add_argument("--artifact-fingerprint", required=True)
    parser.add_argument("--anchor-existed", choices=("true", "false"), required=True)
    parser.add_argument("--targets", type=Path, required=True)
    parser.add_argument("--native-builder-lock", type=Path, required=True)
    parser.add_argument("--conan-requirements", type=Path, required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        release_id, manifest_sha256 = download_and_verify_anchor(args)
        result = "SKIPPED_ALREADY_IDENTICAL" if args.anchor_existed == "true" else "PUBLISHED"
        with args.github_output.open("a", encoding="ascii") as output:
            output.write(
                f"release_id={release_id}\nmanifest_sha256={manifest_sha256}\nresult={result}\n"
            )
    except (
        OSError,
        UnicodeError,
        json.JSONDecodeError,
        subprocess.SubprocessError,
        PublicationPolicyError,
        ValidationError,
        ValueError,
    ) as error:
        raise SystemExit(f"github-anchor: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
