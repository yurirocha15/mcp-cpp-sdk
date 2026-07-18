"""Release target identity and retry decisions."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Mapping

from .model import ValidationError


class IdempotencyDecision(str, Enum):
    PUBLISH = "PUBLISH"
    SKIP_IDENTICAL = "SKIP_IDENTICAL"
    BLOCK_CONFLICT = "BLOCK_CONFLICT"
    BLOCK_AMBIGUOUS = "BLOCK_AMBIGUOUS"


@dataclass(frozen=True)
class ArtifactIdentity:
    """Provider-independent immutable package identity."""

    destination: str
    coordinates: tuple[tuple[str, str], ...]
    sha256: str
    manifest_sha256: str

    @classmethod
    def create(
        cls, *, destination: str, coordinates: Mapping[str, str], sha256: str, manifest_sha256: str
    ) -> "ArtifactIdentity":
        required = TARGET_IDENTITY_FIELDS.get(destination)
        if required is None:
            raise ValidationError(f"unknown release destination: {destination}")
        if set(coordinates) != set(required):
            raise ValidationError(f"{destination} identity must use exactly {required}")
        if any(not isinstance(value, str) or not value for value in coordinates.values()):
            raise ValidationError("identity coordinate values must be non-empty strings")
        for field, value in (("sha256", sha256), ("manifest_sha256", manifest_sha256)):
            if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
                raise ValidationError(f"{field} must be a lowercase SHA-256")
        return cls(destination, tuple(sorted(coordinates.items())), sha256, manifest_sha256)

    @property
    def coordinate_mapping(self) -> dict[str, str]:
        return dict(self.coordinates)


TARGET_IDENTITY_FIELDS: dict[str, tuple[str, ...]] = {
    "github": ("repository_id", "release_id", "tag", "asset_name"),
    "conan2": ("recipe", "version", "user_channel", "recipe_revision"),
    "deb_apt": ("repository", "distribution", "architecture", "name", "version"),
    "rpm": ("repository", "distribution", "architecture", "name", "epoch", "version", "release"),
    "arch_aur": ("package_base", "pkgver", "pkgrel", "source_url"),
    "homebrew": ("tap", "formula", "version", "bottle_tag"),
    "chocolatey": ("feed", "package_id", "version"),
}


def decide_idempotency(local: ArtifactIdentity, remote_matches: list[ArtifactIdentity]) -> IdempotencyDecision:
    """Fail closed unless the remote state is absent or exactly identical."""

    if not remote_matches:
        return IdempotencyDecision.PUBLISH
    if len(remote_matches) != 1:
        return IdempotencyDecision.BLOCK_AMBIGUOUS
    remote = remote_matches[0]
    if remote.destination != local.destination or remote.coordinates != local.coordinates:
        return IdempotencyDecision.BLOCK_CONFLICT
    if remote.sha256 == local.sha256 and remote.manifest_sha256 == local.manifest_sha256:
        return IdempotencyDecision.SKIP_IDENTICAL
    return IdempotencyDecision.BLOCK_CONFLICT
