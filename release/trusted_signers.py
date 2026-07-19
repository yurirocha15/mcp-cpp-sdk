"""Validate the append-only trust registry for release signing keys."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
from typing import Mapping

from .model import ValidationError


_FINGERPRINT = re.compile(r"[0-9A-F]{40}|[0-9A-F]{64}")
_DIGEST = re.compile(r"[0-9a-f]{64}")
_IDENTIFIER = re.compile(r"[a-z0-9][a-z0-9-]{0,63}")
_SIGNER_FIELDS = frozenset(
    {
        "id",
        "public_key",
        "public_key_sha256",
        "primary_fingerprint",
        "tag_subkey_fingerprint",
        "artifact_subkey_fingerprint",
    }
)


@dataclass(frozen=True)
class TrustedSigner:
    """One retained release signer and its exact public key material."""

    identifier: str
    public_key: Path
    public_key_sha256: str
    primary_fingerprint: str
    tag_subkey_fingerprint: str
    artifact_subkey_fingerprint: str

    @property
    def manifest_signers(self) -> dict[str, str]:
        return {
            "primary_fingerprint": self.primary_fingerprint,
            "tag_subkey_fingerprint": self.tag_subkey_fingerprint,
            "artifact_subkey_fingerprint": self.artifact_subkey_fingerprint,
        }


@dataclass(frozen=True)
class TrustedSignerRegistry:
    """Every historical signer plus the signer allowed for new releases."""

    active_signer_id: str
    signers: tuple[TrustedSigner, ...]

    @property
    def active(self) -> TrustedSigner:
        return next(signer for signer in self.signers if signer.identifier == self.active_signer_id)

    def match(self, signers: object, public_key: bytes) -> TrustedSigner:
        """Resolve signed release metadata to exactly one retained trust record."""

        digest = hashlib.sha256(public_key).hexdigest()
        matches = [
            signer
            for signer in self.signers
            if signer.manifest_signers == signers and signer.public_key_sha256 == digest
        ]
        if len(matches) != 1:
            raise ValidationError("release signer is absent or ambiguous in historical trust policy")
        return matches[0]


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValidationError(f"trusted signer registry contains duplicate field: {key}")
        value[key] = item
    return value


def load_trusted_signers(path: Path) -> TrustedSignerRegistry:
    """Load and authenticate every retained public key named by the registry."""

    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_strict_pairs
        )
    except (json.JSONDecodeError, UnicodeError) as error:
        raise ValidationError("trusted signer registry is malformed JSON") from error
    if not isinstance(value, Mapping) or set(value) != {
        "schema_version",
        "active_signer_id",
        "signers",
    }:
        raise ValidationError("trusted signer registry schema is not exact")
    active = value.get("active_signer_id")
    entries = value.get("signers")
    if (
        value.get("schema_version") != 1
        or not isinstance(active, str)
        or _IDENTIFIER.fullmatch(active) is None
        or not isinstance(entries, list)
        or not entries
    ):
        raise ValidationError("trusted signer registry identity is malformed")

    root = path.resolve().parent.parent
    signers: list[TrustedSigner] = []
    for entry in entries:
        if not isinstance(entry, Mapping) or set(entry) != _SIGNER_FIELDS:
            raise ValidationError("trusted signer entry schema is not exact")
        identifier = entry.get("id")
        key_text = entry.get("public_key")
        digest = entry.get("public_key_sha256")
        fingerprints = (
            entry.get("primary_fingerprint"),
            entry.get("tag_subkey_fingerprint"),
            entry.get("artifact_subkey_fingerprint"),
        )
        if (
            not isinstance(identifier, str)
            or _IDENTIFIER.fullmatch(identifier) is None
            or not isinstance(key_text, str)
            or not isinstance(digest, str)
            or _DIGEST.fullmatch(digest) is None
            or any(
                not isinstance(fingerprint, str)
                or _FINGERPRINT.fullmatch(fingerprint) is None
                for fingerprint in fingerprints
            )
            or len(set(fingerprints)) != 3
        ):
            raise ValidationError("trusted signer entry identity is malformed")
        key_relative = PurePosixPath(key_text)
        if (
            key_relative.is_absolute()
            or ".." in key_relative.parts
            or str(key_relative) != key_text
            or not key_relative.parts
        ):
            raise ValidationError("trusted signer public-key path is unsafe")
        key_path = root.joinpath(*key_relative.parts)
        if key_path.is_symlink() or not key_path.is_file():
            raise ValidationError("trusted signer public key is missing or unsafe")
        if hashlib.sha256(key_path.read_bytes()).hexdigest() != digest:
            raise ValidationError("trusted signer public-key digest differs from policy")
        signers.append(
            TrustedSigner(
                identifier=identifier,
                public_key=key_path,
                public_key_sha256=digest,
                primary_fingerprint=fingerprints[0],
                tag_subkey_fingerprint=fingerprints[1],
                artifact_subkey_fingerprint=fingerprints[2],
            )
        )
    identifiers = [signer.identifier for signer in signers]
    identities = [
        (signer.public_key_sha256, tuple(signer.manifest_signers.values()))
        for signer in signers
    ]
    if (
        identifiers != sorted(identifiers)
        or len(set(identifiers)) != len(identifiers)
        or len(set(identities)) != len(identities)
        or active not in identifiers
    ):
        raise ValidationError("trusted signer registry is duplicated or noncanonical")
    return TrustedSignerRegistry(active, tuple(signers))
