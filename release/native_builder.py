"""Run native package builds in reviewed, digest-pinned OCI builders.

The release workflow runs only on fresh GitHub-hosted machines.  This module
keeps the container policy out of workflow YAML, validates the checked-in
builder lock, verifies the image that Docker actually pulled, and starts the
offline package build with no credentials mounted into the container.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
from typing import Any

from .model import ValidationError


_DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
_IMAGE = re.compile(r"ghcr\.io/yurirocha15/mcp-cpp-sdk-release-builders")
_TARGET_ID = re.compile(r"[a-z0-9][a-z0-9_-]{0,63}")
_UNRESOLVED = "UNRESOLVED"
_EXPECTED_IDS = (
    "ubuntu-jammy-amd64",
    "ubuntu-jammy-arm64",
    "ubuntu-noble-amd64",
    "ubuntu-noble-arm64",
    "ubuntu-resolute-amd64",
    "ubuntu-resolute-arm64",
    "debian-bookworm-amd64",
    "debian-bookworm-arm64",
    "debian-trixie-amd64",
    "debian-trixie-arm64",
    "fedora-43-x86_64",
    "fedora-43-aarch64",
    "fedora-44-x86_64",
    "fedora-44-aarch64",
    "el-9-x86_64",
    "el-9-aarch64",
    "el-10-x86_64",
    "el-10-aarch64",
    "aur-x86_64",
    "conan-linux-x86_64",
)
_RECORD_FIELDS = frozenset(
    {
        "architecture",
        "base_digest",
        "base_image",
        "dockerfile",
        "id",
        "image",
        "image_digest",
        "kind",
        "runner",
    }
)
_RUNNER_ARCHITECTURES = {
    "ubuntu-24.04": "x86_64",
    "ubuntu-24.04-arm": "aarch64",
}
_EXPECTED_BASE_IMAGES = {
    "ubuntu-jammy": "ubuntu:22.04",
    "ubuntu-noble": "ubuntu:24.04",
    "ubuntu-resolute": "ubuntu:26.04",
    "debian-bookworm": "debian:12",
    "debian-trixie": "debian:13",
    "fedora-43": "fedora:43",
    "fedora-44": "fedora:44",
    "el-9": "almalinux:9.8",
    "el-10": "almalinux:10.2",
    "aur": "archlinux:base",
    "conan-linux": "ubuntu:24.04",
}


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"native builder lock contains duplicate key: {key}")
        result[key] = value
    return result


def _string(name: str, value: object, *, maximum: int = 256) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > maximum
        or value != value.strip()
        or any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)
    ):
        raise ValidationError(f"{name} must be a bounded canonical string")
    return value


def _load_document(path: Path) -> object:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValidationError(f"cannot read native builder lock: {error}") from error
    if not data or len(data) > 1024 * 1024:
        raise ValidationError("native builder lock must be a non-empty file below 1 MiB")
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_strict_pairs)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"native builder lock is malformed: {error}") from error


def load_builder_lock(path: Path, *, require_resolved: bool) -> tuple[dict[str, str], ...]:
    """Load the exact builder inventory, optionally rejecting bootstrap sentinels."""

    document = _load_document(path)
    if not isinstance(document, Mapping) or set(document) != {"schema_version", "targets"}:
        raise ValidationError("native builder lock schema is not exact")
    if document["schema_version"] != 1 or not isinstance(document["targets"], list):
        raise ValidationError("native builder lock schema version or target list is invalid")
    targets: list[dict[str, str]] = []
    for index, raw in enumerate(document["targets"]):
        if not isinstance(raw, Mapping) or set(raw) != _RECORD_FIELDS:
            raise ValidationError(f"native builder target {index} schema is not exact")
        record = {field: _string(f"native builder target {index}.{field}", raw[field]) for field in _RECORD_FIELDS}
        target_id = record["id"]
        if _TARGET_ID.fullmatch(target_id) is None:
            raise ValidationError("native builder target ID is unsafe")
        expected_kind = (
            "aur"
            if target_id.startswith("aur-")
            else "conan"
            if target_id.startswith("conan-")
            else "apt"
            if target_id.startswith(("ubuntu-", "debian-"))
            else "rpm"
        )
        if record["kind"] != expected_kind:
            raise ValidationError(f"native builder kind is inconsistent: {target_id}")
        family = next(
            (name for name in _EXPECTED_BASE_IMAGES if target_id == name or target_id.startswith(f"{name}-")),
            None,
        )
        if family is None or record["base_image"] != _EXPECTED_BASE_IMAGES[family]:
            raise ValidationError(f"native builder base image is inconsistent: {target_id}")
        expected_architecture = "aarch64" if target_id.endswith(("-arm64", "-aarch64")) else "x86_64"
        if record["architecture"] != expected_architecture:
            raise ValidationError(f"native builder architecture is inconsistent: {target_id}")
        expected_runner = "ubuntu-24.04-arm" if expected_architecture == "aarch64" else "ubuntu-24.04"
        if record["runner"] != expected_runner:
            raise ValidationError(f"native builder runner is not GitHub-hosted: {target_id}")
        if record["image"] != "ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders":
            raise ValidationError(f"native builder repository is inconsistent: {target_id}")
        expected_dockerfile = {
            "apt": "release/native-builders/apt.Dockerfile",
            "rpm": (
                "release/native-builders/fedora.Dockerfile"
                if target_id.startswith("fedora-")
                else "release/native-builders/el.Dockerfile"
            ),
            "aur": "release/native-builders/arch.Dockerfile",
            "conan": "release/native-builders/conan.Dockerfile",
        }[expected_kind]
        if record["dockerfile"] != expected_dockerfile:
            raise ValidationError(f"native builder Dockerfile is not reviewed: {target_id}")
        for field in ("base_digest", "image_digest"):
            value = record[field]
            if value == _UNRESOLVED:
                if require_resolved:
                    raise ValidationError(
                        f"native builder {target_id} has unresolved {field}; complete the two-stage builder bootstrap"
                    )
            elif _DIGEST.fullmatch(value) is None:
                raise ValidationError(f"native builder {target_id} has malformed {field}")
        targets.append(record)
    if tuple(record["id"] for record in targets) != _EXPECTED_IDS:
        raise ValidationError("native builder target inventory or order is not exact")
    return tuple(targets)


def bind_build_matrices(
    targets: Sequence[Mapping[str, str]],
    builders: Sequence[Mapping[str, str]],
) -> tuple[tuple[dict[str, str], ...], dict[str, str], dict[str, str]]:
    """Bind native routes and package validators to exact OCI images."""

    by_id = {record["id"]: record for record in builders}
    if len(by_id) != len(builders):
        raise ValidationError("native builder target IDs are duplicated")
    matrix: list[dict[str, str]] = []
    for target in targets:
        target_id = target.get("id")
        if not isinstance(target_id, str) or target_id not in by_id:
            raise ValidationError("native route is missing its exact builder")
        builder = by_id[target_id]
        value = dict(target)
        if value.get("runner") != builder["runner"]:
            raise ValidationError(f"native route runner conflicts with builder lock: {target_id}")
        value["builder_image"] = f"{builder['image']}@{builder['image_digest']}"
        matrix.append(value)
    aur = by_id["aur-x86_64"]
    conan = by_id["conan-linux-x86_64"]
    return tuple(matrix), {
        "architecture": "x86_64",
        "builder_image": f"{aur['image']}@{aur['image_digest']}",
        "id": aur["id"],
        "runner": aur["runner"],
    }, {
        "architecture": "x86_64",
        "builder_image": f"{conan['image']}@{conan['image_digest']}",
        "id": conan["id"],
        "runner": conan["runner"],
    }


def validate_builder_image(image: str, image_id: str) -> tuple[str, str]:
    """Validate the canonical pinned image reference and local Docker identity."""

    repository, separator, digest = image.partition("@")
    if not separator or _IMAGE.fullmatch(repository) is None or _DIGEST.fullmatch(digest) is None:
        raise ValidationError("native builder image is not the canonical digest-pinned GHCR reference")
    if _DIGEST.fullmatch(image_id) is None:
        raise ValidationError("native builder local image ID is malformed")
    return image, image_id


def bind_container_identity(
    identity: Mapping[str, object], *, image: str, image_id: str
) -> dict[str, object]:
    """Bind a validated platform identity to Docker's independently observed image."""

    image, image_id = validate_builder_image(image, image_id)
    if set(identity) != {"schema_version", "kind", "target_id", "facts", "evidence", "evidence_sha256"}:
        raise ValidationError("platform build identity schema is not exact")
    if identity.get("schema_version") != 1 or not isinstance(identity.get("facts"), Mapping):
        raise ValidationError("platform build identity is malformed")
    evidence = {
        "builder": {"image": image, "image_id": image_id},
        "platform": identity["evidence"],
    }
    facts = dict(identity["facts"])
    facts.update({"builder_image": image, "builder_image_id": image_id})
    canonical = json.dumps(evidence, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    return {
        "schema_version": 2,
        "kind": identity["kind"],
        "target_id": identity["target_id"],
        "facts": {field: facts[field] for field in sorted(facts)},
        "evidence": evidence,
        "evidence_sha256": hashlib.sha256(canonical).hexdigest(),
    }


def validate_container_identity(
    identity: object,
    *,
    expected_platform_identity: Mapping[str, object],
    expected_image: str | None = None,
) -> dict[str, object]:
    """Reconstruct and compare a signed native container identity exactly."""

    if not isinstance(identity, Mapping) or not isinstance(identity.get("evidence"), Mapping):
        raise ValidationError("container build identity is malformed")
    evidence = identity["evidence"]
    if set(evidence) != {"builder", "platform"} or evidence["platform"] != expected_platform_identity["evidence"]:
        raise ValidationError("container build identity platform evidence is not exact")
    builder = evidence["builder"]
    if not isinstance(builder, Mapping) or set(builder) != {"image", "image_id"}:
        raise ValidationError("container build identity builder evidence is not exact")
    image = builder["image"]
    image_id = builder["image_id"]
    if not isinstance(image, str) or not isinstance(image_id, str):
        raise ValidationError("container build identity builder values are malformed")
    if expected_image is not None and image != expected_image:
        raise ValidationError("container build identity does not match the reviewed builder lock")
    expected = bind_container_identity(expected_platform_identity, image=image, image_id=image_id)
    if dict(identity) != expected:
        raise ValidationError("container build identity is not canonical")
    return expected


def _run(arguments: Sequence[str], *, cwd: Path | None = None) -> str:
    return subprocess.run(
        list(arguments), cwd=cwd, check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ).stdout.strip()


def _inspect_image(image: str) -> str:
    _run(("docker", "pull", image))
    digests_raw = _run(("docker", "image", "inspect", "--format={{json .RepoDigests}}", image))
    try:
        repo_digests = json.loads(digests_raw)
    except json.JSONDecodeError as error:
        raise ValidationError("Docker returned malformed repository digests") from error
    if not isinstance(repo_digests, list) or image not in repo_digests:
        raise ValidationError("Docker did not resolve the exact requested builder digest")
    image_id = _run(("docker", "image", "inspect", "--format={{.Id}}", image))
    validate_builder_image(image, image_id)
    return image_id


def _container_command(args: argparse.Namespace, image_id: str) -> list[str]:
    source = Path.cwd().resolve()
    output = args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    command = [
        "docker", "run", "--rm",
        "--tmpfs", "/work:exec,mode=1777",
        "--tmpfs", "/tmp:exec,mode=1777",
        "--env", f"MCP_RELEASE_BUILDER_IMAGE={args.image}",
        "--env", f"MCP_RELEASE_BUILDER_IMAGE_ID={image_id}",
        "--env", "RUNNER_TEMP=/tmp",
        "--mount", f"type=bind,src={source},dst=/source,readonly",
        "--mount", f"type=bind,src={output},dst=/output",
    ]
    if args.kind != "conan":
        command.extend(("--network", "none"))
    if args.kind == "rpm":
        command.extend(("--mount", f"type=bind,src={args.core.resolve()},dst=/core,readonly"))
    if args.kind in {"aur", "conan"}:
        command.extend(("--mount", f"type=bind,src={args.assets.resolve()},dst=/release-assets,readonly"))
    command.extend(
        (
            args.image,
            "python3", "-I", "-S", "/source/scripts/run_release_tool.py",
            "release.native_builder", "execute",
            "--kind", args.kind,
            "--target-id", args.target_id,
            "--version", args.version,
            "--output", "/output/files",
        )
    )
    for name in ("architecture", "distribution", "release", "repository", "source_date_epoch", "tag"):
        value = getattr(args, name, None)
        if value is not None:
            command.extend((f"--{name.replace('_', '-')}", str(value)))
    if args.kind == "rpm":
        command.extend(("--core", "/core"))
    if args.kind == "aur":
        command.extend(("--assets", "/release-assets"))
    if args.kind == "conan":
        command.extend(("--assets", "/release-assets"))
    return command


def run_locked_builder(args: argparse.Namespace) -> None:
    builders = load_builder_lock(args.lock, require_resolved=True)
    builder = next((record for record in builders if record["id"] == args.target_id), None)
    if builder is None or builder["kind"] != args.kind:
        raise ValidationError("requested native builder is not in the reviewed lock")
    expected_image = f"{builder['image']}@{builder['image_digest']}"
    if args.image != expected_image:
        raise ValidationError("workflow image does not match the reviewed native builder lock")
    if platform.machine().lower() != builder["architecture"]:
        raise ValidationError("GitHub-hosted runner architecture does not match the builder")
    image_id = _inspect_image(args.image)
    subprocess.run(_container_command(args, image_id), check=True)


def verify_output_writable(output: Path) -> None:
    """Fail before an expensive build if the container cannot write artifacts."""

    output.mkdir(parents=True, exist_ok=True)
    probe = output / ".native-builder-write-probe"
    probe.write_bytes(b"")
    probe.unlink()


def execute_in_builder(args: argparse.Namespace) -> None:
    source = Path("/source")
    work = Path("/work/source")
    verify_output_writable(args.output)
    shutil.copytree(
        source,
        work,
        symlinks=True,
        ignore=shutil.ignore_patterns(".git", "graphify-out", "build", "out", "__pycache__"),
    )
    os.chdir(work)
    if args.kind in {"apt", "rpm"}:
        from . import native_build

        arguments = [
            args.kind,
            "--version", args.version,
            "--route-id", args.target_id,
            "--distribution", args.distribution,
            "--release", args.release,
            "--architecture", args.architecture,
            "--source-date-epoch", str(args.source_date_epoch),
            "--output", str(args.output),
        ]
        if args.kind == "rpm":
            arguments.extend(("--tag", args.tag, "--repository", args.repository, "--core", str(args.core)))
        if native_build.main(arguments) != 0:
            raise ValidationError("native package builder returned a failure")
        return
    if args.kind == "aur":
        subprocess.run(
            [
                "bash", "scripts/release/validate_aur_packages.sh", "x86_64",
                args.version, str(args.assets),
            ],
            check=True,
        )
        args.output.mkdir(parents=True, exist_ok=True)
        shutil.copyfile("aur-build-identity.json", args.output / "build-identity-aur-x86_64.json")
        return
    if args.kind == "conan":
        from .conan_validation import validate_candidate

        args.output.mkdir(parents=True, exist_ok=True)
        validate_candidate(
            assets=args.assets,
            archive=args.assets / f"mcp-cpp-sdk-{args.version}.tar.gz",
            version=args.version,
            work=Path("/work/conan-validation"),
            conan="/opt/conan/bin/conan",
            expected_conan_version="2.30.0",
            build_profile=source / "release/native-builders/conan-linux-release.profile",
            host_profile=source / "release/native-builders/conan-linux-release.profile",
            lockfile=source / "release/windows/conan.lock",
            evidence=args.output / "conan-validation-linux.json",
        )
        return
    raise ValidationError("native package validator kind is unsupported")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "execute"):
        command = subcommands.add_parser(name)
        command.add_argument("--kind", choices=("apt", "rpm", "aur", "conan"), required=True)
        command.add_argument("--target-id", required=True)
        command.add_argument("--version", required=True)
        command.add_argument("--architecture")
        command.add_argument("--distribution")
        command.add_argument("--release")
        command.add_argument("--source-date-epoch", type=int)
        command.add_argument("--tag")
        command.add_argument("--repository")
        command.add_argument("--core", type=Path)
        command.add_argument("--assets", type=Path)
        command.add_argument("--output", type=Path, required=True)
    run = subcommands.choices["run"]
    run.add_argument("--lock", type=Path, required=True)
    run.add_argument("--image", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.command == "run":
            run_locked_builder(args)
        else:
            execute_in_builder(args)
    except (OSError, subprocess.SubprocessError, ValidationError) as error:
        raise SystemExit(f"native-builder: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
