#!/usr/bin/env python3
"""Build the exact attempt-scoped handoff consumed by the isolated push job."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from typing import Sequence
from uuid import UUID


_STABLE_TAG = re.compile(
    r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
)
_SHA1 = re.compile(r"[0-9a-f]{40}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POSITIVE = re.compile(r"[1-9][0-9]*")


class HandoffError(ValueError):
    """Raised when the verified package cannot form an exact handoff."""


def artifact_name(run_id: str, run_attempt: str) -> str:
    if _POSITIVE.fullmatch(run_id) is None or _POSITIVE.fullmatch(run_attempt) is None:
        raise HandoffError("workflow run identity is not canonical")
    return f"chocolatey-submission-{run_id}-{run_attempt}"


def _identity(
    *,
    source_tag: str,
    source_commit_sha: str,
    release_manifest_sha256: str,
    package_name: str,
    package_sha256: str,
    request_uuid: str,
    run_id: str,
    run_attempt: str,
    publisher_client_sha256: str,
) -> dict[str, object]:
    match = _STABLE_TAG.fullmatch(source_tag)
    if match is None:
        raise HandoffError("source tag is not a canonical stable tag")
    version = source_tag[1:]
    if package_name != f"mcp-cpp-sdk.{version}.nupkg":
        raise HandoffError("Chocolatey package name does not match the source tag")
    if _SHA1.fullmatch(source_commit_sha) is None:
        raise HandoffError("source commit is not a lowercase full SHA")
    for label, value in (
        ("release manifest", release_manifest_sha256),
        ("package", package_sha256),
        ("publisher client", publisher_client_sha256),
    ):
        if _SHA256.fullmatch(value) is None:
            raise HandoffError(f"{label} digest is not a lowercase SHA-256")
    try:
        parsed_uuid = UUID(request_uuid)
    except ValueError as error:
        raise HandoffError("request UUID is invalid") from error
    if str(parsed_uuid) != request_uuid:
        raise HandoffError("request UUID is not canonical lowercase")
    artifact_name(run_id, run_attempt)
    return {
        "schema_version": 2,
        "source_tag": source_tag,
        "source_commit_sha": source_commit_sha,
        "release_manifest_sha256": release_manifest_sha256,
        "package_name": package_name,
        "package_sha256": package_sha256,
        "publisher_client_name": "push_package.ps1",
        "publisher_client_sha256": publisher_client_sha256,
        "request_uuid": request_uuid,
        "run_id": run_id,
        "run_attempt": run_attempt,
    }


def create_handoff(
    *,
    source_package: Path,
    output_directory: Path,
    source_tag: str,
    source_commit_sha: str,
    release_manifest_sha256: str,
    package_name: str,
    package_sha256: str,
    request_uuid: str,
    run_id: str,
    run_attempt: str,
    publisher_client: Path,
) -> str:
    if (
        publisher_client.name != "push_package.ps1"
        or publisher_client.is_symlink()
        or not publisher_client.is_file()
        or publisher_client.stat().st_size == 0
    ):
        raise HandoffError("protected Chocolatey publisher client is missing or unsafe")
    publisher_client_sha256 = hashlib.sha256(publisher_client.read_bytes()).hexdigest()
    identity = _identity(
        source_tag=source_tag,
        source_commit_sha=source_commit_sha,
        release_manifest_sha256=release_manifest_sha256,
        package_name=package_name,
        package_sha256=package_sha256,
        request_uuid=request_uuid,
        run_id=run_id,
        run_attempt=run_attempt,
        publisher_client_sha256=publisher_client_sha256,
    )
    if (
        source_package.name != package_name
        or source_package.is_symlink()
        or not source_package.is_file()
        or source_package.stat().st_size == 0
    ):
        raise HandoffError("verified Chocolatey package input is missing or unsafe")
    actual_digest = hashlib.sha256(source_package.read_bytes()).hexdigest()
    if actual_digest != package_sha256:
        raise HandoffError("verified Chocolatey package bytes changed before handoff")
    if output_directory.exists() or output_directory.is_symlink():
        raise HandoffError("Chocolatey handoff output already exists")
    output_directory.mkdir(parents=False)
    destination = output_directory / package_name
    shutil.copyfile(source_package, destination)
    shutil.copyfile(publisher_client, output_directory / "push_package.ps1")
    (output_directory / "identity.json").write_text(
        json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return artifact_name(run_id, run_attempt)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-package", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--source-tag", required=True)
    parser.add_argument("--source-commit-sha", required=True)
    parser.add_argument("--release-manifest-sha256", required=True)
    parser.add_argument("--package-name", required=True)
    parser.add_argument("--package-sha256", required=True)
    parser.add_argument("--request-uuid", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", required=True)
    parser.add_argument("--publisher-client", type=Path, required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        name = create_handoff(
            source_package=args.source_package,
            output_directory=args.output_directory,
            source_tag=args.source_tag,
            source_commit_sha=args.source_commit_sha,
            release_manifest_sha256=args.release_manifest_sha256,
            package_name=args.package_name,
            package_sha256=args.package_sha256,
            request_uuid=args.request_uuid,
            run_id=args.run_id,
            run_attempt=args.run_attempt,
            publisher_client=args.publisher_client,
        )
        with args.github_output.open("a", encoding="ascii") as output:
            client_sha256 = hashlib.sha256(args.publisher_client.read_bytes()).hexdigest()
            identity_sha256 = hashlib.sha256(
                (args.output_directory / "identity.json").read_bytes()
            ).hexdigest()
            output.write(
                f"artifact_name={name}\n"
                f"publisher_client_sha256={client_sha256}\n"
                f"identity_sha256={identity_sha256}\n"
            )
    except (HandoffError, OSError, UnicodeError) as error:
        print(f"handoff: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
