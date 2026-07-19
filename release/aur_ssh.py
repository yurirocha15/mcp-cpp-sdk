"""Validate the exact public AUR SSH host key used by release jobs."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import os
from pathlib import Path
import struct
import sys
from typing import Sequence


AUR_HOST = "aur.archlinux.org"
AUR_KEY_TYPE = "ssh-ed25519"
AUR_ED25519_FINGERPRINT = "SHA256:RFzBCUItH9LZS0cKB5UE6ceAYhBD5C8GeOBip8Z11+4"


class AurSshError(ValueError):
    """Raised when configured AUR SSH trust material is not exact."""


def _fingerprint(blob: bytes) -> str:
    value = base64.b64encode(hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")
    return f"SHA256:{value}"


def _algorithm(blob: bytes) -> str:
    if len(blob) < 4:
        raise AurSshError("AUR host key blob is truncated")
    length = struct.unpack(">I", blob[:4])[0]
    if length < 1 or length > 64 or len(blob) < 4 + length:
        raise AurSshError("AUR host key algorithm field is malformed")
    try:
        return blob[4 : 4 + length].decode("ascii")
    except UnicodeDecodeError as error:
        raise AurSshError("AUR host key algorithm is not ASCII") from error


def validate_known_hosts(
    text: str,
    *,
    expected_fingerprint: str = AUR_ED25519_FINGERPRINT,
) -> str:
    """Return one canonical known_hosts line after exact host/key verification."""

    lines = text.splitlines()
    if len(lines) != 1 or not lines[0]:
        raise AurSshError("AUR known_hosts must contain exactly one non-empty line")
    fields = lines[0].split()
    if len(fields) != 3:
        raise AurSshError("AUR known_hosts line must contain only host, key type, and key")
    host, key_type, encoded = fields
    if host != AUR_HOST:
        raise AurSshError("AUR known_hosts entry must use the exact canonical hostname")
    if key_type != AUR_KEY_TYPE:
        raise AurSshError("AUR known_hosts entry must use the reviewed Ed25519 key")
    try:
        blob = base64.b64decode(encoded, validate=True)
    except (binascii.Error, ValueError) as error:
        raise AurSshError("AUR host key is not canonical base64") from error
    if _algorithm(blob) != AUR_KEY_TYPE:
        raise AurSshError("AUR host key blob algorithm does not match its declaration")
    if len(blob) != 4 + len(AUR_KEY_TYPE) + 4 + 32:
        raise AurSshError("AUR Ed25519 host key blob has an unexpected shape")
    if _fingerprint(blob) != expected_fingerprint:
        raise AurSshError("AUR Ed25519 host-key fingerprint is not the reviewed value")
    return f"{AUR_HOST} {AUR_KEY_TYPE} {encoded}\n"


def _write_exclusive(path: Path, content: str) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        value = os.environ.get("AUR_KNOWN_HOSTS")
        if value is None:
            raise AurSshError("AUR_KNOWN_HOSTS is not set")
        _write_exclusive(args.output, validate_known_hosts(value))
    except (AurSshError, OSError) as error:
        print(f"aur-ssh: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
