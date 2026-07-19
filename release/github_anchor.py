"""Validate downloaded GitHub Release metadata and write the anchor handoff."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from typing import Any, Mapping, Sequence

from .artifacts import STABLE_CHANNEL_CAPABILITIES
from .github_publication import (
    PublicationPolicyError,
    _gh_json,
    verify_live_github_tag,
    verify_release_identity,
)
from .model import SemVer, ValidationError
from .historical_anchor import verify_historical_candidate_v2
from .publication_contract import CONTRACT_ASSET
from .trusted_signers import load_trusted_signers
from .verify_candidate import server_asset_digests, verify_candidate


_DIGEST_RE = re.compile(r"[0-9a-f]{64}")
_GITHUB_API_VERSION = "X-GitHub-Api-Version: 2026-03-10"
_ASSET_RESPONSE_LIMIT = 8 * 1024 * 1024
_MANIFEST_FIELDS = {
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


def _json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _file_digests(directory: Path) -> dict[str, str]:
    return {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in directory.iterdir()
        if path.is_file()
    }


def verify_anchor(args: argparse.Namespace) -> tuple[int, str]:
    version = SemVer.parse(args.version)
    if version.tag != args.tag:
        raise ValidationError("anchor tag and version disagree")
    release_id = verify_release_identity(
        _json(args.release_graphql),
        _json(args.release_rest),
        tag=args.tag,
        prerelease=version.is_prerelease,
    )
    local = _file_digests(args.directory)
    if server_asset_digests(_json(args.release_assets)) != local:
        raise ValidationError("downloaded release assets do not match server-side SHA-256 digests")
    manifest_path = args.directory / "release-manifest.json"
    manifest = _json(manifest_path)
    if getattr(args, "policy_source", "current") == "embedded":
        manifest_sha256 = local.get("release-manifest.json")
        if manifest_sha256 is None:
            raise ValidationError("historical release manifest is missing")
        if args.prior_manifest_sha256 and args.prior_manifest_sha256 != manifest_sha256:
            raise ValidationError(
                "existing ledger manifest conflicts with the verified immutable anchor"
            )
        anchor = {
            "schema_version": 1,
            "repository": args.repository,
            "release_id": str(release_id),
            "tag": args.tag,
            "commit": args.commit,
            "manifest_sha256": manifest_sha256,
        }
        (args.directory / "ANCHOR.json").write_text(
            json.dumps(anchor, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        return release_id, manifest_sha256
    expected_capabilities = (
        ["github"] if version.is_prerelease else list(STABLE_CHANNEL_CAPABILITIES)
    )
    if (
        not isinstance(manifest, dict)
        or set(manifest) != _MANIFEST_FIELDS
        or manifest.get("schema_version") != 2
        or manifest.get("channel_capabilities") != expected_capabilities
    ):
        raise ValidationError("release manifest channel capabilities are incomplete or malformed")
    expected_signers = {
        "primary_fingerprint": args.primary_fingerprint,
        "tag_subkey_fingerprint": args.tag_fingerprint,
        "artifact_subkey_fingerprint": args.artifact_fingerprint,
    }
    expected_ledger = {
        "issue_id": args.ledger_issue,
        "issue_url": f"https://github.com/{args.repository}/issues/{args.ledger_issue}",
    }
    if (
        manifest.get("tag") != args.tag
        or manifest.get("version") != str(version)
        or manifest.get("commit") != args.commit
        or manifest.get("release_ledger") != expected_ledger
        or manifest.get("signers") != expected_signers
    ):
        raise ValidationError("release manifest does not match the dispatch identity")
    if (args.directory / "release-signing-key.asc").read_bytes() != args.public_key.read_bytes():
        raise ValidationError("release verification key differs from the tagged public key")

    payloads = manifest.get("payloads")
    if not isinstance(payloads, list):
        raise ValidationError("release manifest payload list is missing")
    records: dict[str, Mapping[str, Any]] = {}
    for record in payloads:
        if (
            not isinstance(record, dict)
            or set(record) - {"name", "size", "sha256", "role", "build_tuple"}
            or not {"name", "size", "sha256", "role"} <= set(record)
        ):
            raise ValidationError("release manifest contains a malformed payload record")
        name = record["name"]
        if not isinstance(name, str) or name in records or name not in local:
            raise ValidationError("release manifest payload name is unsafe or duplicated")
        if (
            record["size"] != (args.directory / name).stat().st_size
            or record["sha256"] != local[name]
        ):
            raise ValidationError("release manifest payload identity differs from the immutable asset")
        records[name] = record
    controls = {
        "release-manifest.json",
        "release-manifest.json.asc",
        "SHA256SUMS",
        "SHA256SUMS.asc",
    }
    detached = {f"{name}.asc" for name in records if name.endswith((".tar.gz", ".zip"))}
    if set(local) != set(records) | controls | detached:
        raise ValidationError("release manifest payload set does not cover the immutable release")
    if not version.is_prerelease:
        stable_controls = {
            "aur-PKGBUILD",
            "aur-SRCINFO",
            "homebrew-mcp-cpp-sdk.rb",
            "conan-recipe-config-entry.json",
            "conan-recipe-conandata-entry.json",
            "conan-recipe-conanfile.py",
            "conan-recipe-test-CMakeLists.txt",
            "conan-recipe-test-conanfile.py",
            "conan-recipe-test-test_package.cpp",
            "conan-source.json",
            "cloudsmith-routes.json",
            CONTRACT_ASSET,
            f"mcp-cpp-sdk-{version}-windows-x64-v143-md.zip",
            f"mcp-cpp-sdk.{version}.nupkg",
        }
        if not stable_controls <= set(records):
            raise ValidationError("stable release omits a future channel's immutable input")
        if not any(name.endswith(".deb") for name in records) or not any(
            name.endswith(".rpm") for name in records
        ):
            raise ValidationError("stable release omits native APT or RPM packages")
    manifest_sha256 = local["release-manifest.json"]
    if args.prior_manifest_sha256 and args.prior_manifest_sha256 != manifest_sha256:
        raise ValidationError("existing ledger manifest conflicts with the verified immutable anchor")
    anchor = {
        "schema_version": 1,
        "repository": args.repository,
        "release_id": str(release_id),
        "tag": args.tag,
        "commit": args.commit,
        "manifest_sha256": manifest_sha256,
    }
    (args.directory / "ANCHOR.json").write_text(
        json.dumps(anchor, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return release_id, manifest_sha256


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
    if args.prior_manifest_sha256 and _DIGEST_RE.fullmatch(args.prior_manifest_sha256) is None:
        raise ValidationError("prior manifest SHA-256 is malformed")
    verify_live_github_tag(
        repository=args.repository,
        tag=args.tag,
        tag_object_sha=args.tag_object_sha,
        commit=args.commit,
    )
    _download_release(directory=args.directory, repository=args.repository, tag=args.tag)
    graphql, rest, assets = _release_metadata(repository=args.repository, tag=args.tag)
    verify_release_identity(
        graphql,
        rest,
        tag=args.tag,
        prerelease=version.is_prerelease,
    )
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
            ledger_issue=args.ledger_issue,
            repository=args.repository,
            signers=signer.manifest_signers,
            public_key=verification_key,
        )
        if policy_source == "embedded"
        else verify_candidate(
            args.directory,
            tag=args.tag,
            commit=args.commit,
            ledger_issue=args.ledger_issue,
            repository=args.repository,
            primary_fingerprint=primary_fingerprint,
            tag_fingerprint=tag_fingerprint,
            artifact_fingerprint=artifact_fingerprint,
            public_key=verification_key,
            targets_path=args.targets,
            target_catalog_path=args.target_catalog,
            native_builder_lock_path=args.native_builder_lock,
            conan_requirements_path=args.conan_requirements,
            include_release_notes=False,
            policy_source=policy_source,
        )
    )
    with tempfile.TemporaryDirectory() as temporary:
        metadata = Path(temporary)
        paths: dict[str, Path] = {}
        for name, value in (
            ("release-graphql", graphql),
            ("release-rest", rest),
            ("release-assets", assets),
        ):
            path = metadata / f"{name}.json"
            path.write_text(
                json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
                newline="\n",
            )
            paths[name] = path
        verification_values = {
            **vars(args),
            "public_key": verification_key,
            "primary_fingerprint": primary_fingerprint,
            "tag_fingerprint": tag_fingerprint,
            "artifact_fingerprint": artifact_fingerprint,
            "policy_source": policy_source,
            "release_graphql": paths["release-graphql"],
            "release_rest": paths["release-rest"],
            "release_assets": paths["release-assets"],
        }
        verification = argparse.Namespace(**verification_values)
        release_id, manifest_sha256 = verify_anchor(verification)
    if candidate_manifest != manifest_sha256:
        raise ValidationError("candidate and anchor manifest digests disagree")
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
    parser.add_argument("--ledger-issue", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag-object-sha", required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    parser.add_argument("--tag-fingerprint", required=True)
    parser.add_argument("--artifact-fingerprint", required=True)
    parser.add_argument("--prior-manifest-sha256", default="")
    parser.add_argument("--anchor-existed", choices=("true", "false"), required=True)
    parser.add_argument("--targets", type=Path, required=True)
    parser.add_argument("--target-catalog", type=Path, required=True)
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
