#!/usr/bin/env python3
"""Keep certificate verification settings in the TLS context factory.

Every TLS context the SDK makes verifies its peer, and `src/transport/tls.cpp` is where that is
set. A verify-mode call anywhere else in the library, its headers or its examples could weaken
that for one code path without the factory's tests noticing, so this check fails on one.

Usage: ``check_tls_verification.py [repo-root]``.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("src", "include", "examples", "conformance")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".ipp"}
FACTORY = Path("src/transport/tls.cpp")

# Calls and constants that set or relax how a TLS peer is verified.
_VERIFY_SETTING = re.compile(
    r"\b(?:set_verify_mode|set_verify_callback|set_verify_depth|SSL_CTX_set_verify|SSL_set_verify"
    r"|SSL_CTX_set_cert_verify_callback|verify_none|SSL_VERIFY_NONE)\b"
)


def violations(root: Path) -> list[str]:
    """Returns `path:line: text` for each verification setting outside the factory."""
    found = []
    for directory in SCAN_DIRS:
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            relative = path.relative_to(root)
            if relative == FACTORY:
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if _VERIFY_SETTING.search(line):
                    found.append(f"{relative.as_posix()}:{number}: {line.strip()}")
    return sorted(found)


def main(argv: list[str]) -> int:
    root = Path(argv[1]).resolve() if len(argv) > 1 else REPO_ROOT
    found = violations(root)
    for entry in found:
        print(entry)
    if found:
        print(f"TLS verification is set outside {FACTORY.as_posix()}; set it in the factory only.")
        return 1
    print(f"TLS verification settings: only in {FACTORY.as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
