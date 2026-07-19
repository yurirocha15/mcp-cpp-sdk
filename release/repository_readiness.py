"""Validate a tagged checkout and emit the immutable release build matrices."""

from __future__ import annotations

import argparse
from datetime import date
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from typing import Sequence

from .build_identity import load_and_validate_target_projection
from .model import SemVer, ValidationError
from .native_builder import bind_build_matrices, load_builder_lock
from .trusted_signers import TrustedSigner, load_trusted_signers
from .verify_gnupg_status import validate_status


_FINGERPRINT_RE = re.compile(r"[0-9A-F]{40}|[0-9A-F]{64}")
_SHA_RE = re.compile(r"[0-9a-f]{40}")


def validate_tag_position(
    *, commit: str, dispatch_head_sha: str, anchor_exists: bool, is_ancestor: bool
) -> None:
    """Allow old tagged commits only for an already immutable release anchor."""

    if _SHA_RE.fullmatch(commit) is None or _SHA_RE.fullmatch(dispatch_head_sha) is None:
        raise ValidationError("tag or workflow-dispatch commit identity is malformed")
    if type(anchor_exists) is not bool or type(is_ancestor) is not bool:
        raise ValidationError("tag position state must be boolean")
    if not is_ancestor:
        raise ValidationError("tag commit is not reachable from origin/main")
    if not anchor_exists and commit != dispatch_head_sha:
        raise ValidationError(
            "a fresh release tag must point to the workflow-dispatch main HEAD"
        )


def _git(*arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *arguments],
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _verify_changelog(*, commit: str, version: SemVer, today: date) -> None:
    changelog = _git("show", f"{commit}:CHANGELOG.md", check=False)
    heading = re.compile(
        rf"(?m)^## \[{re.escape(str(version))}\] - ([0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}})$"
    )
    match = heading.search(changelog.stdout) if changelog.returncode == 0 else None
    if match is None:
        raise ValidationError("tagged CHANGELOG.md lacks an exact dated version heading")
    try:
        release_date = date.fromisoformat(match.group(1))
    except ValueError as error:
        raise ValidationError("tagged CHANGELOG.md contains an invalid release date") from error
    if release_date > today:
        raise ValidationError("tagged CHANGELOG.md release date is in the future")


def _verify_tag_signature(
    *,
    tag: str,
    signers: Sequence[TrustedSigner],
) -> TrustedSigner:
    """Accept a tag signed by exactly one retained historical signer."""

    if not signers:
        raise ValidationError("trusted release signer inventory is empty")
    with tempfile.TemporaryDirectory() as home:
        environment = {**os.environ, "GNUPGHOME": home}
        for signer in signers:
            shown = subprocess.run(
                [
                    "gpg", "--batch", "--with-colons", "--import-options",
                    "show-only", "--import", str(signer.public_key),
                ],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
                env=environment,
            ).stdout.splitlines()
            actual = {line.split(":")[9] for line in shown if line.startswith("fpr:")}
            if actual != set(signer.manifest_signers.values()):
                raise ValidationError(
                    "historical public key fingerprints differ from trusted policy"
                )
            subprocess.run(
                ["gpg", "--batch", "--import", str(signer.public_key)],
                check=True,
                env=environment,
            )
        status = subprocess.run(
            ["git", "verify-tag", "--raw", tag],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=environment,
        )
        if status.returncode:
            raise ValidationError("annotated tag signature verification failed")
        matches: list[TrustedSigner] = []
        for signer in signers:
            try:
                validate_status(
                    status.stdout,
                    signing_fingerprint=signer.tag_subkey_fingerprint,
                    primary_fingerprint=signer.primary_fingerprint,
                )
            except ValueError:
                continue
            matches.append(signer)
        if len(matches) != 1:
            raise ValidationError(
                "annotated tag signature does not select one historical signer"
            )
        return matches[0]


def validate_repository(args: argparse.Namespace) -> dict[str, str]:
    version = SemVer.parse(args.version)
    if args.tag != version.tag:
        raise ValidationError("tag does not match the requested version")
    object_type = _git("cat-file", "-t", f"refs/tags/{args.tag}", check=False)
    if object_type.returncode or object_type.stdout.strip() != "tag":
        raise ValidationError("release requires an existing annotated tag")
    tag_object_sha = _git("rev-parse", f"refs/tags/{args.tag}").stdout.strip()
    if _SHA_RE.fullmatch(tag_object_sha) is None:
        raise ValidationError("annotated tag object identity is malformed")
    commit = _git("rev-list", "-n", "1", f"refs/tags/{args.tag}").stdout.strip()
    if _SHA_RE.fullmatch(commit) is None:
        raise ValidationError("tag did not resolve to one commit")
    validate_tag_position(
        commit=commit,
        dispatch_head_sha=args.dispatch_head_sha,
        anchor_exists=args.anchor_exists == "true",
        is_ancestor=(
            _git("merge-base", "--is-ancestor", commit, "origin/main", check=False).returncode
            == 0
        ),
    )
    tagged_version = _git("show", f"{commit}:VERSION", check=False)
    if tagged_version.returncode or tagged_version.stdout.strip() != str(version):
        raise ValidationError("VERSION in the tagged tree does not match the tag")
    _verify_changelog(commit=commit, version=version, today=date.today())
    registry = load_trusted_signers(args.trusted_signers)
    if args.anchor_exists == "false":
        active = registry.active
        if (
            active.public_key.resolve() != args.public_key.resolve()
            or active.manifest_signers
            != {
                "primary_fingerprint": args.primary_fingerprint,
                "tag_subkey_fingerprint": args.tag_fingerprint,
                "artifact_subkey_fingerprint": args.artifact_fingerprint,
            }
        ):
            raise ValidationError("new release signer is not the active trusted signer")
        accepted_signers = (active,)
    else:
        accepted_signers = registry.signers
    selected_signer = _verify_tag_signature(tag=args.tag, signers=accepted_signers)
    epoch = _git("show", "-s", "--format=%ct", commit).stdout.strip()
    if re.fullmatch(r"[1-9][0-9]*", epoch) is None:
        raise ValidationError("tag commit timestamp is invalid")
    result = {
        "commit": commit,
        "tag_object_sha": tag_object_sha,
        "source_date_epoch": epoch,
        "primary_fingerprint": selected_signer.primary_fingerprint,
        "tag_fingerprint": selected_signer.tag_subkey_fingerprint,
        "artifact_fingerprint": selected_signer.artifact_subkey_fingerprint,
    }
    if args.anchor_exists == "false":
        targets = load_and_validate_target_projection(args.native_targets, args.target_catalog)
        builders = load_builder_lock(args.native_builder_lock, require_resolved=True)
        bound_targets, aur_builder, conan_builder = bind_build_matrices(targets, builders)
        for package_format in ("apt", "rpm"):
            matrix = [target for target in bound_targets if target["format"] == package_format]
            result[f"{package_format}_matrix"] = json.dumps(
                matrix, sort_keys=True, separators=(",", ":")
            )
        result["aur_builder"] = json.dumps(
            aur_builder, sort_keys=True, separators=(",", ":")
        )
        result["conan_builder"] = json.dumps(
            conan_builder, sort_keys=True, separators=(",", ":")
        )
    else:
        result.update(
            {
                "apt_matrix": "[]",
                "rpm_matrix": "[]",
                "aur_builder": '{"builder_image":"unused","runner":"ubuntu-24.04"}',
                "conan_builder": '{"builder_image":"unused","runner":"ubuntu-24.04"}',
            }
        )
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--dispatch-head-sha", required=True)
    parser.add_argument("--anchor-exists", choices=("true", "false"), required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    parser.add_argument("--tag-fingerprint", required=True)
    parser.add_argument("--artifact-fingerprint", required=True)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--trusted-signers", type=Path, required=True)
    parser.add_argument("--native-targets", type=Path, required=True)
    parser.add_argument("--native-builder-lock", type=Path, required=True)
    parser.add_argument("--target-catalog", type=Path, required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        values = validate_repository(args)
        with args.github_output.open("a", encoding="utf-8") as stream:
            for key, value in values.items():
                stream.write(f"{key}={value}\n")
    except (
        OSError,
        UnicodeError,
        subprocess.CalledProcessError,
        ValidationError,
    ) as error:
        raise SystemExit(f"repository-readiness: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
