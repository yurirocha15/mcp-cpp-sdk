"""Validate the immutable OCI builder inventory used by release jobs."""

from __future__ import annotations

from collections.abc import Mapping
import json
from pathlib import Path
import re

from .model import ValidationError


_DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
_TARGET_ID = re.compile(r"[a-z0-9][a-z0-9_-]{0,63}")
_UNRESOLVED = "UNRESOLVED"
_EXPECTED_IDS = (
    "ubuntu-jammy-amd64", "ubuntu-jammy-arm64",
    "ubuntu-noble-amd64", "ubuntu-noble-arm64",
    "ubuntu-resolute-amd64", "ubuntu-resolute-arm64",
    "debian-bookworm-amd64", "debian-bookworm-arm64",
    "debian-trixie-amd64", "debian-trixie-arm64",
    "fedora-43-x86_64", "fedora-43-aarch64",
    "fedora-44-x86_64", "fedora-44-aarch64",
    "el-9-x86_64", "el-9-aarch64", "el-10-x86_64", "el-10-aarch64",
    "aur-x86_64", "conan-linux-x86_64",
)
_FIELDS = frozenset(
    {
        "architecture", "base_digest", "base_image", "dockerfile", "id",
        "image", "image_digest", "kind", "runner",
    }
)
_BASE_IMAGES = {
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


def _string(name: str, value: object) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 256
        or value != value.strip()
        or any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)
    ):
        raise ValidationError(f"{name} must be a bounded canonical string")
    return value


def _document(path: Path) -> object:
    try:
        data = path.read_bytes()
        if not data or len(data) > 1024 * 1024:
            raise ValidationError("native builder lock must be a non-empty file below 1 MiB")
        return json.loads(data.decode("utf-8"), object_pairs_hook=_strict_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"native builder lock is malformed: {error}") from error


def load_builder_lock(path: Path, *, require_resolved: bool) -> tuple[dict[str, str], ...]:
    document = _document(path)
    if (
        not isinstance(document, Mapping)
        or set(document) != {"schema_version", "targets"}
        or document["schema_version"] != 1
        or not isinstance(document["targets"], list)
    ):
        raise ValidationError("native builder lock schema is not exact")
    targets: list[dict[str, str]] = []
    for index, raw in enumerate(document["targets"]):
        if not isinstance(raw, Mapping) or set(raw) != _FIELDS:
            raise ValidationError(f"native builder target {index} schema is not exact")
        record = {
            field: _string(f"native builder target {index}.{field}", raw[field])
            for field in _FIELDS
        }
        target_id = record["id"]
        kind = (
            "aur" if target_id.startswith("aur-") else
            "conan" if target_id.startswith("conan-") else
            "apt" if target_id.startswith(("ubuntu-", "debian-")) else "rpm"
        )
        family = next(
            (name for name in _BASE_IMAGES if target_id == name or target_id.startswith(f"{name}-")),
            None,
        )
        architecture = "aarch64" if target_id.endswith(("-arm64", "-aarch64")) else "x86_64"
        runner = "ubuntu-24.04-arm" if architecture == "aarch64" else "ubuntu-24.04"
        dockerfile = {
            "apt": "release/native-builders/apt.Dockerfile",
            "rpm": (
                "release/native-builders/fedora.Dockerfile"
                if target_id.startswith("fedora-")
                else "release/native-builders/el.Dockerfile"
            ),
            "aur": "release/native-builders/arch.Dockerfile",
            "conan": "release/native-builders/conan.Dockerfile",
        }[kind]
        if (
            _TARGET_ID.fullmatch(target_id) is None
            or record["kind"] != kind
            or family is None
            or record["base_image"] != _BASE_IMAGES[family]
            or record["architecture"] != architecture
            or record["runner"] != runner
            or record["dockerfile"] != dockerfile
            or record["image"] != "ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders"
        ):
            raise ValidationError(f"native builder target is inconsistent: {target_id}")
        for field in ("base_digest", "image_digest"):
            value = record[field]
            if value == _UNRESOLVED and require_resolved:
                raise ValidationError(
                    f"native builder {target_id} has unresolved {field}; "
                    "complete builder bootstrap"
                )
            if value != _UNRESOLVED and _DIGEST.fullmatch(value) is None:
                raise ValidationError(f"native builder {target_id} has malformed {field}")
        targets.append(record)
    if tuple(record["id"] for record in targets) != _EXPECTED_IDS:
        raise ValidationError("native builder target inventory or order is not exact")
    return tuple(targets)
