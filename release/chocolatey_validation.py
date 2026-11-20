#!/usr/bin/env python3
"""Check a signed candidate's Chocolatey package with the publisher's own release check.

The Chocolatey publisher runs ``verify_release`` on the published GitHub Release
assets before it submits the package. This module gives that check the same
assets from a verified candidate, so a package the publisher would refuse fails
the candidate before the immutable release exists.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Sequence

from .github_publication import PublicationPolicyError, release_assets
from .model import SemVer, ValidationError

PUBLISHER_TOOL = "run_publisher_tool.py"
PUBLISHER_KEY = Path("keys/release-signing-key.asc")


class ChocolateyValidationError(ValueError):
    """Raised when the publisher refuses the candidate's package."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stage_release_assets(candidate: Path, destination: Path) -> Path:
    """Copy the files the GitHub Release would carry, and return the copy's directory."""

    try:
        _notes, assets = release_assets(candidate)
    except PublicationPolicyError as error:
        raise ChocolateyValidationError(str(error)) from error
    destination.mkdir(parents=True)
    for asset in assets:
        shutil.copyfile(asset, destination / asset.name)
    return destination


def _outputs(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in values:
            raise ChocolateyValidationError("the publisher wrote malformed outputs")
        values[key] = value
    return values


def validate(
    *,
    candidate: Path,
    publisher: Path,
    tag: str,
    commit: str,
    primary_fingerprint: str,
    artifact_fingerprint: str,
    publisher_repository_id: str,
    work_directory: Path,
) -> str:
    """Run the publisher's check and return the package name it accepted."""

    try:
        version = SemVer.from_tag(tag, stable_only=True)
    except ValidationError as error:
        raise ChocolateyValidationError(str(error)) from error
    tool = publisher / PUBLISHER_TOOL
    if not tool.is_file() or not (publisher / PUBLISHER_KEY).is_file():
        raise ChocolateyValidationError("the publisher checkout lacks its release tool or key")
    if work_directory.exists():
        raise ChocolateyValidationError("the work directory already exists")
    assets = stage_release_assets(candidate, work_directory / "release-assets")
    manifest = assets / "release-manifest.json"
    if not manifest.is_file():
        raise ChocolateyValidationError("the candidate lacks release-manifest.json")
    outputs = work_directory / "publisher-outputs"
    outputs.touch()
    completed = subprocess.run(
        [
            sys.executable,
            "-I",
            "-S",
            str(tool.resolve()),
            "verify_release",
            "--directory",
            str(assets.resolve()),
            "--tag",
            tag,
            "--commit",
            commit,
            "--manifest-sha256",
            _sha256(manifest),
            "--primary-fingerprint",
            primary_fingerprint,
            "--artifact-fingerprint",
            artifact_fingerprint,
            "--public-key",
            str((publisher / PUBLISHER_KEY).resolve()),
            "--publisher-repository-id",
            publisher_repository_id,
        ],
        check=False,
        cwd=work_directory,
        env={**os.environ, "GITHUB_OUTPUT": str(outputs.resolve())},
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=600,
    )
    if completed.returncode != 0:
        raise ChocolateyValidationError(
            f"the publisher refused the candidate:\n{completed.stdout.strip()}"
        )
    values = _outputs(outputs)
    expected = f"mcp-cpp-sdk.{version}.nupkg"
    if values.get("package_name") != expected:
        raise ChocolateyValidationError(
            f"the publisher accepted {values.get('package_name')!r} instead of {expected!r}"
        )
    if values.get("package_sha256") != _sha256(candidate / expected):
        raise ChocolateyValidationError("the publisher accepted a package other than the candidate's")
    return expected


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--publisher", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    parser.add_argument("--artifact-fingerprint", required=True)
    parser.add_argument("--publisher-repository-id", required=True)
    parser.add_argument("--work-directory", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        package = validate(
            candidate=args.candidate,
            publisher=args.publisher,
            tag=args.tag,
            commit=args.commit,
            primary_fingerprint=args.primary_fingerprint,
            artifact_fingerprint=args.artifact_fingerprint,
            publisher_repository_id=args.publisher_repository_id,
            work_directory=args.work_directory,
        )
    except (OSError, UnicodeError, subprocess.TimeoutExpired, ChocolateyValidationError) as error:
        raise SystemExit(f"chocolatey-validation: {error}") from error
    print(f"the Chocolatey publisher accepts {package}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
