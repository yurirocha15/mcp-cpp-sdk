"""Resolve and publish native release builders in two reviewed stages.

This administrative tool never edits the checked-in lock.  It emits bounded,
machine-readable evidence and a proposed replacement lock for human review.
Release jobs reject the ``UNRESOLVED`` bootstrap sentinel.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from typing import Mapping, Sequence

from .artifacts import canonical_json_bytes, write_atomic
from .model import ValidationError
from .native_builder_lock import load_builder_lock


_DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
_COMMIT = re.compile(r"[0-9a-f]{40}")
_INDEX_MEDIA_TYPES = {
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.index.v1+json",
}
_MANIFEST_MEDIA_TYPES = {
    "application/vnd.docker.distribution.manifest.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
}
_OCI_ARCHITECTURES = {"x86_64": "amd64", "aarch64": "arm64"}
_STAGES = ("resolve-bases", "build-images")


def _json_object(data: bytes, *, label: str) -> Mapping[str, object]:
    def strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise ValidationError(f"{label} contains duplicate key: {key}")
            result[key] = value
        return result

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=strict_pairs)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"{label} is malformed") from error
    if not isinstance(value, Mapping):
        raise ValidationError(f"{label} is not an object")
    return value


def select_platform_digest(index_bytes: bytes, architecture: str) -> str:
    """Select exactly one Linux platform manifest, never the mutable index."""

    document = _json_object(index_bytes, label="base image manifest index")
    if document.get("mediaType") not in _INDEX_MEDIA_TYPES:
        raise ValidationError("base image tag did not resolve to a reviewed manifest index")
    manifests = document.get("manifests")
    if not isinstance(manifests, list):
        raise ValidationError("base image manifest index has no manifest inventory")
    expected = _OCI_ARCHITECTURES.get(architecture)
    if expected is None:
        raise ValidationError("builder architecture is unsupported")
    matches: list[str] = []
    for descriptor in manifests:
        if not isinstance(descriptor, Mapping) or not isinstance(descriptor.get("platform"), Mapping):
            continue
        platform = descriptor["platform"]
        if platform.get("os") != "linux" or platform.get("architecture") != expected:
            continue
        if expected == "arm64" and platform.get("variant") not in (None, "v8"):
            continue
        digest = descriptor.get("digest")
        if not isinstance(digest, str) or _DIGEST.fullmatch(digest) is None:
            raise ValidationError("base image platform descriptor has a malformed digest")
        matches.append(digest)
    if len(matches) != 1:
        raise ValidationError("base image index must contain exactly one matching Linux platform")
    return matches[0]


def _run_bytes(arguments: Sequence[str]) -> bytes:
    return subprocess.run(
        list(arguments), check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    ).stdout


def _target(lock: Path, target_id: str, *, require_bases: bool) -> dict[str, str]:
    builders = load_builder_lock(lock, require_resolved=False)
    target = next((dict(record) for record in builders if record["id"] == target_id), None)
    if target is None:
        raise ValidationError("bootstrap target is not in the reviewed builder lock")
    if require_bases and target["base_digest"] == "UNRESOLVED":
        raise ValidationError("builder image stage requires a reviewed base digest first")
    return target


def bootstrap_matrix(*, lock: Path, stage: str) -> str:
    """Validate a bootstrap stage and return its exact hosted-runner matrix."""

    targets = load_builder_lock(lock, require_resolved=False)
    if stage == "resolve-bases":
        if any(target["base_digest"] != "UNRESOLVED" for target in targets):
            raise ValidationError("base digest stage requires the initial unresolved base lock")
    elif stage == "build-images":
        if any(target["base_digest"] == "UNRESOLVED" for target in targets):
            raise ValidationError("image stage requires every reviewed base digest")
    else:
        raise ValidationError("builder bootstrap stage is invalid")
    if any(target["image_digest"] != "UNRESOLVED" for target in targets):
        raise ValidationError("builder bootstrap is already complete or partially resolved")
    return json.dumps(
        {
            "include": [
                {"id": target["id"], "runner": target["runner"]}
                for target in targets
            ]
        },
        sort_keys=True,
        separators=(",", ":"),
    )


def resolve_base(*, lock: Path, target_id: str, evidence: Path) -> None:
    target = _target(lock, target_id, require_bases=False)
    raw = _run_bytes(
        ("docker", "buildx", "imagetools", "inspect", "--raw", target["base_image"])
    )
    digest = select_platform_digest(raw, target["architecture"])
    write_atomic(
        evidence,
        canonical_json_bytes(
            {
                "schema_version": 1,
                "stage": "base",
                "target_id": target_id,
                "architecture": target["architecture"],
                "base_image": target["base_image"],
                "base_digest": digest,
                "manifest_index_sha256": hashlib.sha256(raw).hexdigest(),
            }
        ),
    )


def _pushed_digest(output: bytes) -> str:
    matches = re.findall(rb"(?m)^.*digest: (sha256:[0-9a-f]{64}) size: [0-9]+\s*$", output)
    values = {value.decode("ascii") for value in matches}
    if len(values) != 1:
        raise ValidationError("Docker push did not report exactly one immutable image digest")
    return values.pop()


def bootstrap_tag(target: Mapping[str, str], source_commit: str) -> str:
    if _COMMIT.fullmatch(source_commit) is None:
        raise ValidationError("builder source commit must be a full lowercase Git SHA")
    return f"{target['image']}:bootstrap-{target['id']}-{source_commit[:12]}"


def _container_output(image: str, arguments: Sequence[str]) -> str:
    return _run_bytes(("docker", "run", "--rm", "--network", "none", image, *arguments)).decode(
        "utf-8"
    ).strip()


def inspect_local_image(
    image: str, target: Mapping[str, str], source_commit: str
) -> dict[str, object]:
    """Verify the built platform, revision label, and required offline tools."""

    raw = _run_bytes(("docker", "image", "inspect", image))
    try:
        images = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError("Docker returned malformed local image inspection") from error
    if not isinstance(images, list) or len(images) != 1 or not isinstance(images[0], Mapping):
        raise ValidationError("Docker local image inspection is not singular")
    value: Mapping[str, object] = images[0]
    expected_architecture = _OCI_ARCHITECTURES[target["architecture"]]
    config = value.get("Config")
    labels = config.get("Labels") if isinstance(config, Mapping) else None
    if (
        value.get("Os") != "linux"
        or value.get("Architecture") != expected_architecture
        or not isinstance(labels, Mapping)
        or labels.get("org.opencontainers.image.revision") != source_commit
        or labels.get("org.opencontainers.image.source")
        != "https://github.com/yurirocha15/mcp-cpp-sdk"
    ):
        raise ValidationError("built image platform or reviewed OCI labels are incorrect")
    required_tools = {
        "apt": ("cmake", "dpkg-buildpackage", "fakeroot", "g++", "ninja", "python3"),
        "rpm": ("cmake", "g++", "ninja", "python3", "rpmbuild"),
        "aur": ("cmake", "g++", "gpg", "makepkg", "ninja", "pacman", "python3", "readelf"),
        "conan": ("cmake", "conan", "g++", "nasm", "ninja", "pkg-config", "python3"),
    }[target["kind"]]
    probe = (
        "import json,shutil,sys;"
        f"names={required_tools!r};"
        "paths={name:shutil.which(name) for name in names};"
        "sys.exit(2) if any(value is None for value in paths.values()) else print(json.dumps(paths,sort_keys=True))"
    )
    tools = _json_object(
        _container_output(image, ("python3", "-c", probe)).encode("utf-8"),
        label="builder tool probe",
    )
    if set(tools) != set(required_tools):
        raise ValidationError("builder tool probe inventory is incomplete")
    machine = _container_output(image, ("uname", "-m"))
    if machine != target["architecture"]:
        raise ValidationError("builder uname architecture differs from the lock")
    package_architecture = ""
    if target["kind"] == "apt":
        package_architecture = _container_output(image, ("dpkg", "--print-architecture"))
        expected = "arm64" if target["architecture"] == "aarch64" else "amd64"
        if package_architecture != expected:
            raise ValidationError("APT builder package architecture is incorrect")
    elif target["kind"] == "rpm":
        package_architecture = _container_output(image, ("rpm", "--eval", "%{_arch}"))
        if package_architecture != target["architecture"]:
            raise ValidationError("RPM builder macro architecture is incorrect")
        if target["id"].startswith("el-"):
            _container_output(image, ("rpm", "-q", "epel-release", "json-devel"))
    elif target["kind"] == "aur":
        if _container_output(image, ("id", "-u")) != "1001":
            raise ValidationError("AUR builder must use the reviewed unprivileged UID")
    else:
        if _container_output(image, ("conan", "--version")) != "Conan version 2.30.0":
            raise ValidationError("Conan builder client version is not exact")
    image_id = value.get("Id")
    if not isinstance(image_id, str) or _DIGEST.fullmatch(image_id) is None:
        raise ValidationError("Docker local image ID is malformed")
    return {
        "image_id": image_id,
        "architecture": machine,
        "package_architecture": package_architecture,
        "tools": dict(tools),
    }


def build_image(
    *, lock: Path, target_id: str, source_commit: str, source: Path, evidence: Path
) -> None:
    if _COMMIT.fullmatch(source_commit) is None:
        raise ValidationError("builder source commit must be a full lowercase Git SHA")
    target = _target(lock, target_id, require_bases=True)
    if target["image_digest"] != "UNRESOLVED":
        raise ValidationError("builder image stage only accepts an unresolved image digest")
    dockerfile = source / target["dockerfile"]
    if dockerfile.is_symlink() or not dockerfile.is_file():
        raise ValidationError("reviewed builder Dockerfile is missing or unsafe")
    tag = bootstrap_tag(target, source_commit)
    platform = f"linux/{_OCI_ARCHITECTURES[target['architecture']]}"
    base = f"{target['base_image']}@{target['base_digest']}"
    subprocess.run(
        [
            "docker", "build", "--pull", "--no-cache", "--platform", platform,
            "--build-arg", f"BASE_IMAGE={base}",
            "--label", f"org.opencontainers.image.revision={source_commit}",
            "--file", str(dockerfile), "--tag", tag, str(source),
        ],
        check=True,
    )
    inspection = inspect_local_image(tag, target, source_commit)
    pushed = subprocess.run(
        ["docker", "push", tag], check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    ).stdout
    digest = _pushed_digest(pushed)
    raw = _run_bytes(("docker", "buildx", "imagetools", "inspect", "--raw", f"{target['image']}@{digest}"))
    manifest = _json_object(raw, label="published builder manifest")
    if manifest.get("mediaType") not in _MANIFEST_MEDIA_TYPES or "manifests" in manifest:
        raise ValidationError("published builder must be a single-platform image manifest")
    if f"sha256:{hashlib.sha256(raw).hexdigest()}" != digest:
        raise ValidationError("published builder bytes do not match Docker's reported digest")
    write_atomic(
        evidence,
        canonical_json_bytes(
            {
                "schema_version": 1,
                "stage": "image",
                "target_id": target_id,
                "architecture": target["architecture"],
                "base_image": base,
                "dockerfile_sha256": hashlib.sha256(dockerfile.read_bytes()).hexdigest(),
                "image": target["image"],
                "image_digest": digest,
                "inspection": inspection,
                "inspection_sha256": hashlib.sha256(canonical_json_bytes(inspection)).hexdigest(),
                "source_commit": source_commit,
            }
        ),
    )


def _read_evidence(path: Path) -> Mapping[str, object]:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 64 * 1024:
        raise ValidationError(f"bootstrap evidence is missing or unsafe: {path.name}")
    return _json_object(path.read_bytes(), label=f"bootstrap evidence {path.name}")


def merge_evidence(
    *, lock: Path, stage: str, evidence_directory: Path, output: Path,
    source_commit: str | None = None,
) -> None:
    targets = [dict(record) for record in load_builder_lock(lock, require_resolved=False)]
    if output.exists() or output.is_symlink():
        raise ValidationError("proposed builder lock output must be a new path")
    if stage == "image" and (source_commit is None or _COMMIT.fullmatch(source_commit) is None):
        raise ValidationError("image evidence merge requires the reviewed full source commit")
    expected_files = {f"{target['id']}.json" for target in targets}
    actual_files = {path.name for path in evidence_directory.iterdir() if path.is_file()}
    if actual_files != expected_files:
        raise ValidationError("bootstrap evidence inventory does not match every builder target")
    for target in targets:
        evidence = _read_evidence(evidence_directory / f"{target['id']}.json")
        common = {
            "schema_version": 1,
            "stage": stage,
            "target_id": target["id"],
            "architecture": target["architecture"],
        }
        if any(evidence.get(key) != value for key, value in common.items()):
            raise ValidationError(f"bootstrap evidence identity mismatch: {target['id']}")
        if stage == "base":
            if set(evidence) != {
                *common, "base_image", "base_digest", "manifest_index_sha256"
            } or evidence.get("base_image") != target["base_image"]:
                raise ValidationError(f"base evidence schema mismatch: {target['id']}")
            digest = evidence.get("base_digest")
            if not isinstance(digest, str) or _DIGEST.fullmatch(digest) is None:
                raise ValidationError(f"base evidence digest is invalid: {target['id']}")
            target["base_digest"] = digest
        elif stage == "image":
            dockerfile = Path(target["dockerfile"])
            expected = {
                **common,
                "base_image": f"{target['base_image']}@{target['base_digest']}",
                "dockerfile_sha256": hashlib.sha256(dockerfile.read_bytes()).hexdigest(),
                "image": target["image"],
                "source_commit": source_commit,
            }
            if set(evidence) != {*expected, "image_digest", "inspection", "inspection_sha256"} or any(
                evidence.get(key) != value for key, value in expected.items()
            ):
                raise ValidationError(f"image evidence schema mismatch: {target['id']}")
            inspection = evidence.get("inspection")
            inspection_sha256 = evidence.get("inspection_sha256")
            if (
                not isinstance(inspection, Mapping)
                or not isinstance(inspection_sha256, str)
                or hashlib.sha256(canonical_json_bytes(inspection)).hexdigest()
                != inspection_sha256
                or inspection.get("architecture") != target["architecture"]
            ):
                raise ValidationError(f"image inspection evidence is invalid: {target['id']}")
            digest = evidence.get("image_digest")
            if not isinstance(digest, str) or _DIGEST.fullmatch(digest) is None:
                raise ValidationError(f"image evidence digest is invalid: {target['id']}")
            target["image_digest"] = digest
        else:
            raise ValidationError("bootstrap merge stage must be base or image")
    write_atomic(output, canonical_json_bytes({"schema_version": 1, "targets": targets}))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    matrix = commands.add_parser("matrix")
    matrix.add_argument("--lock", type=Path, required=True)
    matrix.add_argument("--stage", choices=_STAGES, required=True)
    resolve = commands.add_parser("resolve-base")
    resolve.add_argument("--lock", type=Path, required=True)
    resolve.add_argument("--target-id", required=True)
    resolve.add_argument("--evidence", type=Path, required=True)
    build = commands.add_parser("build-image")
    build.add_argument("--lock", type=Path, required=True)
    build.add_argument("--target-id", required=True)
    build.add_argument("--source-commit", required=True)
    build.add_argument("--source", type=Path, required=True)
    build.add_argument("--evidence", type=Path, required=True)
    merge = commands.add_parser("merge")
    merge.add_argument("--lock", type=Path, required=True)
    merge.add_argument("--stage", choices=("base", "image"), required=True)
    merge.add_argument("--evidence-directory", type=Path, required=True)
    merge.add_argument("--output", type=Path, required=True)
    merge.add_argument("--source-commit")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.command == "matrix":
            print(
                bootstrap_matrix(
                    lock=args.lock, stage=args.stage
                )
            )
        elif args.command == "resolve-base":
            resolve_base(lock=args.lock, target_id=args.target_id, evidence=args.evidence)
        elif args.command == "build-image":
            build_image(
                lock=args.lock, target_id=args.target_id, source_commit=args.source_commit,
                source=args.source, evidence=args.evidence,
            )
        else:
            merge_evidence(
                lock=args.lock, stage=args.stage, evidence_directory=args.evidence_directory,
                output=args.output, source_commit=args.source_commit,
            )
    except (OSError, subprocess.SubprocessError, ValidationError) as error:
        raise SystemExit(f"native-builder-bootstrap: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
