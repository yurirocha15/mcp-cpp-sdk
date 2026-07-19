"""Construct the deterministic, provider-independent release payload.

This module deliberately contains no GitHub Actions concerns.  It can be run
locally against a tagged checkout and unit-tested without parsing workflow YAML.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from typing import Sequence

from .artifacts import (
    SourceInventory,
    build_sboms,
    build_source_archives,
    canonical_json_bytes,
    load_conan_requirements,
    write_atomic,
)
from .model import SemVer, ValidationError
from .publication_contract import CONTRACT_ASSET, write_publication_contract
from .templates import render_file


_REPOSITORY_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
_COMMIT_RE = re.compile(r"[0-9a-f]{40}")


def _git_archive_sha256(commit: str) -> str:
    archive = subprocess.run(
        ["git", "archive", "--format=tar", commit],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    return hashlib.sha256(archive).hexdigest()


def _aur_srcinfo(
    *, version: SemVer, repository: str, source_url: str, source_sha256: str, fingerprint: str
) -> str:
    return (
        "pkgbase = mcp-cpp-sdk\n\tpkgdesc = Model Context Protocol C++ SDK\n"
        f"\tpkgver = {version}\n\tpkgrel = 1\n\turl = https://github.com/{repository}\n"
        "\tarch = x86_64\n\tlicense = Apache-2.0\n"
        "\tmakedepends = cmake>=3.20\n\tmakedepends = gtest\n"
        "\tmakedepends = ninja\n\tmakedepends = python\n"
        "\tdepends = boost>=1.74\n\tdepends = nlohmann-json>=3.10.5\n"
        "\tdepends = openssl>=3.0\n\toptions = !debug\n\toptions = staticlibs\n"
        f"\tsource = mcp-cpp-sdk-{version}.tar.gz::{source_url}\n"
        f"\tsource = mcp-cpp-sdk-{version}.tar.gz.asc::{source_url}.asc\n"
        f"\tvalidpgpkeys = {fingerprint}\n\tsha256sums = {source_sha256}\n"
        "\tsha256sums = SKIP\n\npkgname = mcp-cpp-sdk\n\n"
        f"pkgname = mcp-cpp-sdk-static\n\tdepends = mcp-cpp-sdk={version}-1\n"
    )


def construct_core(
    *,
    root: Path,
    output: Path,
    version_text: str,
    tag: str,
    commit: str,
    source_date_epoch: int,
    release_kind: str,
    repository: str,
    primary_fingerprint: str,
    artifact_fingerprint: str,
    provider_control_commits: dict[str, str],
    provider_destinations: dict[str, dict[str, str]],
) -> None:
    version = SemVer.parse(version_text)
    if tag != version.tag:
        raise ValidationError("tag does not match the release version")
    if _COMMIT_RE.fullmatch(commit) is None:
        raise ValidationError("commit must be a full lowercase SHA-1")
    if _REPOSITORY_RE.fullmatch(repository) is None:
        raise ValidationError("repository must be an owner/name pair")
    if release_kind not in {"stable", "rc"}:
        raise ValidationError("release kind must be stable or rc")
    if (release_kind == "rc") != version.is_prerelease:
        raise ValidationError("release kind does not match the version")
    if source_date_epoch < 1:
        raise ValidationError("source date epoch must be positive")

    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    inventory = SourceInventory.from_file(root / "release/source-archive-inventory.txt")
    build_source_archives(
        root=root,
        inventory=inventory,
        version=version,
        output_dir=output,
        source_date_epoch=source_date_epoch,
    )
    spdx, cyclonedx = build_sboms(
        version=version,
        files=inventory.expand(root),
        root=root,
        source_date_epoch=source_date_epoch,
        namespace_base=f"https://github.com/{repository}/releases/download/{tag}",
    )
    write_atomic(output / f"mcp-cpp-sdk-{version}.spdx.json", canonical_json_bytes(spdx))
    write_atomic(output / f"mcp-cpp-sdk-{version}.cdx.json", canonical_json_bytes(cyclonedx))
    shutil.copyfile(root / "keys/release-signing-key.asc", output / "release-signing-key.asc")

    tarball = output / f"mcp-cpp-sdk-{version}.tar.gz"
    source_sha256 = hashlib.sha256(tarball.read_bytes()).hexdigest()
    source_url = f"https://github.com/{repository}/releases/download/{tag}/{tarball.name}"
    metadata = {
        "commit": commit,
        "source_date_epoch": source_date_epoch,
        "source_tree_sha256": _git_archive_sha256(commit),
        "source_sha256": source_sha256,
    }
    write_atomic(output / "core-metadata.json", canonical_json_bytes(metadata))
    (output / "RELEASE_NOTES.md").write_text(
        f"mcp-cpp-sdk {version}\n\nSee CHANGELOG.md in the signed source archive.\n",
        encoding="utf-8",
        newline="\n",
    )
    if release_kind == "rc":
        return

    write_publication_contract(
        output / CONTRACT_ASSET,
        root=root,
        control_commits=provider_control_commits,
        destinations=provider_destinations,
    )

    render_file(
        root / "packaging/aur/PKGBUILD.in",
        output / "aur-PKGBUILD",
        {
            "VERSION": str(version),
            "PKGREL": "1",
            "SOURCE_URL": source_url,
            "SOURCE_SHA256": source_sha256,
            "SOURCE_SIGNATURE_URL": source_url + ".asc",
            "PRIMARY_FINGERPRINT": primary_fingerprint,
            "ARTIFACT_SUBKEY_FINGERPRINT": artifact_fingerprint,
        },
    )
    (output / "aur-SRCINFO").write_text(
        _aur_srcinfo(
            version=version,
            repository=repository,
            source_url=source_url,
            source_sha256=source_sha256,
            fingerprint=primary_fingerprint,
        ),
        encoding="utf-8",
        newline="\n",
    )
    render_file(
        root / "packaging/homebrew/mcp-cpp-sdk.rb.in",
        output / "homebrew-mcp-cpp-sdk.rb",
        {"VERSION": str(version), "SOURCE_URL": source_url, "SOURCE_SHA256": source_sha256},
    )
    render_file(
        root / "packaging/conan-center/config-entry.json.in",
        output / "conan-recipe-config-entry.json",
        {"VERSION": str(version)},
    )
    render_file(
        root / "packaging/conan-center/all/conandata-entry.json.in",
        output / "conan-recipe-conandata-entry.json",
        {"VERSION": str(version), "SOURCE_URL": source_url, "SOURCE_SHA256": source_sha256},
    )
    requirements = load_conan_requirements(root / "packaging/conan-center/requirements.json")
    render_file(
        root / "packaging/conan-center/all/conanfile.py.in",
        output / "conan-recipe-conanfile.py",
        {
            "BOOST_REFERENCE": requirements[0],
            "NLOHMANN_JSON_REFERENCE": requirements[1],
            "OPENSSL_REFERENCE": requirements[2],
        },
    )
    for source, destination in (
        ("CMakeLists.txt.in", "conan-recipe-test-CMakeLists.txt"),
        ("conanfile.py.in", "conan-recipe-test-conanfile.py"),
        ("test_package.cpp.in", "conan-recipe-test-test_package.cpp"),
    ):
        shutil.copyfile(
            root / "packaging/conan-center/all/test_package" / source,
            output / destination,
        )
    write_atomic(
        output / "conan-source.json",
        canonical_json_bytes(
            {
                "recipe": "mcp-cpp-sdk",
                "version": str(version),
                "tag": tag,
                "commit": commit,
                "source_url": source_url,
                "source_sha256": source_sha256,
            }
        ),
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--source-date-epoch", type=int, required=True)
    parser.add_argument("--release-kind", choices=("stable", "rc"), required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    parser.add_argument("--artifact-fingerprint", required=True)
    parser.add_argument("--homebrew-control-commit", default="")
    parser.add_argument("--chocolatey-control-commit", default="")
    parser.add_argument("--conan-control-commit", default="")
    parser.add_argument("--cloudsmith-namespace", default="")
    parser.add_argument("--cloudsmith-repository", default="")
    parser.add_argument("--cloudsmith-preflight-username", default="")
    parser.add_argument("--cloudsmith-publish-username", default="")
    parser.add_argument("--homebrew-repository-id", default="")
    parser.add_argument("--homebrew-bot-login", default="")
    parser.add_argument("--chocolatey-repository-id", default="")
    parser.add_argument("--conan-fork-repository", default="")
    parser.add_argument("--conan-fork-repository-id", default="")
    parser.add_argument("--conan-control-repository-id", default="")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        construct_core(
            root=args.root,
            output=args.output,
            version_text=args.version,
            tag=args.tag,
            commit=args.commit,
            source_date_epoch=args.source_date_epoch,
            release_kind=args.release_kind,
            repository=args.repository,
            primary_fingerprint=args.primary_fingerprint,
            artifact_fingerprint=args.artifact_fingerprint,
            provider_control_commits={
                "homebrew": args.homebrew_control_commit,
                "chocolatey": args.chocolatey_control_commit,
                "conan": args.conan_control_commit,
            },
            provider_destinations={
                "cloudsmith": {
                    "namespace": args.cloudsmith_namespace,
                    "repository": args.cloudsmith_repository,
                    "preflight_username": args.cloudsmith_preflight_username,
                    "publish_username": args.cloudsmith_publish_username,
                },
                "homebrew": {
                    "repository": "yurirocha15/homebrew-mcp-cpp-sdk",
                    "repository_id": args.homebrew_repository_id,
                    "bot_login": args.homebrew_bot_login,
                },
                "chocolatey": {
                    "repository": "yurirocha15/mcp-cpp-sdk-chocolatey-publisher",
                    "repository_id": args.chocolatey_repository_id,
                },
                "conan_fork": {
                    "repository": args.conan_fork_repository,
                    "repository_id": args.conan_fork_repository_id,
                    "upstream": "conan-io/conan-center-index",
                },
                "conan_control": {
                    "repository": "yurirocha15/mcp-cpp-sdk-release-control",
                    "repository_id": args.conan_control_repository_id,
                },
            },
        )
    except (OSError, subprocess.CalledProcessError, ValidationError) as error:
        raise SystemExit(f"construct-core: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
