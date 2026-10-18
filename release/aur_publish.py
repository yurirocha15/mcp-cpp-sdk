"""Pure policy checks used by the thin AUR Git/SSH publisher adapter."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
from typing import Sequence

from .model import SemVer, ValidationError


_PKGVER_RE = re.compile(r"(?m)^pkgver=([0-9]+)\.([0-9]+)\.([0-9]+)$")
_EXPECTED_TREE = (".SRCINFO", "PKGBUILD")


def verify_existing_version(path: Path, target_text: str) -> None:
    target = SemVer.parse(target_text, stable_only=True)
    if not path.exists():
        return
    match = _PKGVER_RE.search(path.read_text(encoding="utf-8"))
    if match is None:
        raise ValidationError("existing AUR pkgver is missing or noncanonical")
    remote = tuple(int(part) for part in match.groups())
    if remote > (target.major, target.minor, target.patch):
        raise ValidationError("refusing to replace a newer AUR package version")


def verify_tree_inventory(path: Path) -> None:
    """Require the complete tracked AUR tree to contain only reviewed metadata."""

    lines = tuple(path.read_text(encoding="utf-8").splitlines())
    if lines != _EXPECTED_TREE:
        raise ValidationError("AUR repository tree must contain exactly .SRCINFO and PKGBUILD")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--existing", type=Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--tree-inventory", type=Path)
    args = parser.parse_args(argv)
    try:
        verify_existing_version(args.existing, args.target)
        if args.tree_inventory is not None:
            verify_tree_inventory(args.tree_inventory)
    except (OSError, UnicodeError, ValidationError) as error:
        raise SystemExit(f"aur-publish: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
