#!/usr/bin/python3
"""Root-owned, narrow pacman wrapper for the unprivileged AUR builder."""

from __future__ import annotations

from pathlib import Path
import os
import re
import sys
from typing import Sequence


_PACKAGE = re.compile(
    r"mcp-cpp-sdk(?P<static>-static)?-"
    r"(?P<version>[0-9]+\.[0-9]+\.[0-9]+)-1-x86_64\.pkg\.tar\.zst"
)
_DIRECTORY = Path("/work/source/aur-check")


def validate_arguments(arguments: Sequence[str]) -> tuple[str, str]:
    if len(arguments) != 2:
        raise ValueError("exactly two AUR package paths are required")
    paths = tuple(Path(value) for value in arguments)
    if any(not path.is_absolute() or path.parent != _DIRECTORY for path in paths):
        raise ValueError("AUR package path is outside the reviewed build directory")
    matches = tuple(_PACKAGE.fullmatch(path.name) for path in paths)
    if any(match is None for match in matches):
        raise ValueError("AUR package filename is not canonical")
    if {bool(match and match.group("static")) for match in matches} != {False, True}:
        raise ValueError("shared and static AUR package paths are both required")
    if len({match.group("version") for match in matches if match is not None}) != 1:
        raise ValueError("shared and static AUR package versions must match")
    if any(path.is_symlink() or not path.is_file() or path.stat().st_size == 0 for path in paths):
        raise ValueError("AUR package path is missing, empty, or a symlink")
    return str(paths[0]), str(paths[1])


def main(argv: Sequence[str] | None = None) -> int:
    try:
        packages = validate_arguments(sys.argv[1:] if argv is None else argv)
    except (OSError, ValueError) as error:
        raise SystemExit(f"install-aur-packages: {error}") from error
    os.execv("/usr/bin/pacman", ("pacman", "-U", "--noconfirm", "--", *packages))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
