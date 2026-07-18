#!/usr/bin/env python3
"""Validate GnuPG status output against exact release signing identities."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


FINGERPRINT_RE = re.compile(r"[0-9A-F]{40,64}")
FATAL_STATUS = frozenset(
    {
        "BADSIG",
        "ERRSIG",
        "EXPKEYSIG",
        "EXPSIG",
        "KEYEXPIRED",
        "NO_PUBKEY",
        "REVKEYSIG",
        "SIGEXPIRED",
    }
)


def validate_status(text: str, *, signing_fingerprint: str, primary_fingerprint: str) -> None:
    signing = _fingerprint(signing_fingerprint)
    primary = _fingerprint(primary_fingerprint)
    valid_signatures: list[list[str]] = []
    for line in text.splitlines():
        if not line.startswith("[GNUPG:] "):
            if line.strip():
                raise ValueError("status stream contains non-status output")
            continue
        fields = line[len("[GNUPG:] ") :].split()
        if not fields:
            raise ValueError("status stream contains an empty record")
        if fields[0] in FATAL_STATUS:
            raise ValueError(f"GnuPG reported {fields[0]}")
        if fields[0] == "VALIDSIG":
            valid_signatures.append(fields)
    if len(valid_signatures) != 1:
        raise ValueError("status stream must contain exactly one VALIDSIG")
    fields = valid_signatures[0]
    if len(fields) < 11:
        raise ValueError("VALIDSIG record is malformed")
    if _fingerprint(fields[1]) != signing:
        raise ValueError("signature was not made by the approved artifact subkey")
    if _fingerprint(fields[-1]) != primary:
        raise ValueError("signature does not chain to the approved primary key")


def _fingerprint(value: str) -> str:
    normalized = value.upper()
    if FINGERPRINT_RE.fullmatch(normalized) is None:
        raise ValueError("fingerprint is not full uppercase hexadecimal")
    return normalized


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--status", required=True, type=Path)
    parser.add_argument("--signing-fingerprint", required=True)
    parser.add_argument("--primary-fingerprint", required=True)
    args = parser.parse_args()
    try:
        validate_status(
            args.status.read_text(encoding="utf-8"),
            signing_fingerprint=args.signing_fingerprint,
            primary_fingerprint=args.primary_fingerprint,
        )
    except (OSError, UnicodeError, ValueError) as error:
        print(f"verify-gnupg-status: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
