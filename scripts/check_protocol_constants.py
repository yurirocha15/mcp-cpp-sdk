#!/usr/bin/env python3
"""Guard the uses of protocol version constants and retired error codes.

``g_LATEST_PROTOCOL_VERSION``, ``g_SUPPORTED_PROTOCOL_VERSIONS`` and
``is_supported_protocol_version()`` change meaning when a new protocol
revision becomes the latest one. The library code therefore names the era it
means (``g_LATEST_LEGACY_PROTOCOL_VERSION``, ``is_legacy_protocol_version()``,
``is_modern_protocol_version()``) and uses the era-neutral names only where
they are defined. A new use under ``src/`` or ``include/`` fails this check
until it is either rewritten or added to ``ALLOWED_USES`` on purpose.

Implementations of protocol revision 2026-07-28 must not emit ``-32002``
(resource not found in 2025-11-25 and earlier) or ``-32042`` (URL elicitation
required, 2025-11-25 only). The library may hold ``-32002`` only as the value
of ``g_CONNECTION_CLOSED``, a client-local error that never goes on the wire;
any other literal of either code under ``src/`` or ``include/`` fails.

Comments are ignored. Usage: ``check_protocol_constants.py [repo-root]``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("src", "include")
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".ipp"}

GUARDED_NAMES = (
    "g_LATEST_PROTOCOL_VERSION",
    "g_SUPPORTED_PROTOCOL_VERSIONS",
    "is_supported_protocol_version",
)

# (path relative to the repository root, name) -> number of uses outside comments.
ALLOWED_USES: dict[tuple[str, str], int] = {
    ("include/mcp/protocol/capabilities.hpp", "g_LATEST_PROTOCOL_VERSION"): 1,
    ("include/mcp/protocol/capabilities.hpp", "g_SUPPORTED_PROTOCOL_VERSIONS"): 4,
    ("include/mcp/protocol/capabilities.hpp", "is_supported_protocol_version"): 1,
}

RETIRED_CODES = ("-32002", "-32042")

# (path, code) -> number of literals allowed outside comments.
ALLOWED_CODES: dict[tuple[str, str], int] = {
    ("include/mcp/protocol/base.hpp", "-32002"): 1,  # g_CONNECTION_CLOSED
}

_COMMENT_OR_STRING = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
    re.DOTALL,
)


def strip_comments(text: str) -> str:
    """Removes comments, keeping string and character literals and line breaks."""

    def replace(match: re.Match[str]) -> str:
        token = match.group(0)
        if token.startswith("/"):
            return "\n" * token.count("\n")
        return token

    return _COMMENT_OR_STRING.sub(replace, text)


def _name_pattern(name: str) -> re.Pattern[str]:
    return re.compile(r"\b" + re.escape(name) + r"\b")


def _code_pattern(code: str) -> re.Pattern[str]:
    # -32002, - 32002 and -32002L, but not -320020 or 1-32002.
    return re.compile(r"(?<![\w.])-\s*" + re.escape(code[1:]) + r"(?![\w.])")


def iter_sources(root: Path) -> list[Path]:
    files: list[Path] = []
    for directory in SCAN_DIRS:
        base = root / directory
        if not base.is_dir():
            continue
        files.extend(
            path
            for path in sorted(base.rglob("*"))
            if path.is_file() and path.suffix in SOURCE_SUFFIXES
        )
    return files


def find_violations(root: Path) -> list[str]:
    """Returns one line per violation; an empty list means the tree passes."""
    violations: list[str] = []
    seen_names: dict[tuple[str, str], int] = {}
    seen_codes: dict[tuple[str, str], int] = {}
    for path in iter_sources(root):
        relative = path.relative_to(root).as_posix()
        code = strip_comments(path.read_text(encoding="utf-8"))
        lines = code.splitlines()
        for name in GUARDED_NAMES:
            for match in _name_pattern(name).finditer(code):
                key = (relative, name)
                seen_names[key] = seen_names.get(key, 0) + 1
                if seen_names[key] > ALLOWED_USES.get(key, 0):
                    line = code.count("\n", 0, match.start()) + 1
                    violations.append(
                        f"{relative}:{line}: {name} names whatever revision is latest; use the "
                        f"era-specific constant or add the site to ALLOWED_USES: "
                        f"{lines[line - 1].strip()}"
                    )
        for retired in RETIRED_CODES:
            for match in _code_pattern(retired).finditer(code):
                key = (relative, retired)
                seen_codes[key] = seen_codes.get(key, 0) + 1
                if seen_codes[key] > ALLOWED_CODES.get(key, 0):
                    line = code.count("\n", 0, match.start()) + 1
                    violations.append(
                        f"{relative}:{line}: error code {retired} must not be emitted by a "
                        f"2026-07-28 implementation: {lines[line - 1].strip()}"
                    )
    for allowed, expected in (*ALLOWED_USES.items(), *ALLOWED_CODES.items()):
        found = seen_names.get(allowed, 0) + seen_codes.get(allowed, 0)
        if found < expected:
            violations.append(
                f"{allowed[0]}: expected {expected} use(s) of {allowed[1]}, found {found}; "
                "shrink the allowlist to match"
            )
    return violations


def main(argv: list[str]) -> int:
    root = Path(argv[1]).resolve() if len(argv) > 1 else REPO_ROOT
    violations = find_violations(root)
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        print(f"{len(violations)} protocol constant violation(s)", file=sys.stderr)
        return 1
    print("Protocol constant uses match the allowlist")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
