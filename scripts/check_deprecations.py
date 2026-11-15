#!/usr/bin/env python3
"""Require an attribute on every declaration documented as deprecated.

A Doxygen comment under ``include/`` with an ``@deprecated`` or ``\\deprecated`` tag must be followed by a
declaration marked ``MCP_DEPRECATED(...)`` (or ``[[deprecated(...)]]``), so users get a compiler
warning and not only a note in the documentation. A feature that legacy peers still need, whose
removal window has not opened, is deprecated in the documentation only: the comment then says
``doc-only`` on the tag line, for example ``@deprecated doc-only: removal no earlier than ...``.

Usage: ``check_deprecations.py [repo-root]``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("include",)
SOURCE_SUFFIXES = {".h", ".hpp", ".ipp"}

# A Doxygen comment is a ``/** */`` block or a run of consecutive ``///`` or ``//!`` lines.
_DOC_COMMENT = re.compile(
    r"/\*\*(?s:.*?)\*/|(?:^[ \t]*//[/!].*(?:\n|\Z))+", re.MULTILINE
)
_TAG = re.compile(r"[@\\]deprecated\b([^\n]*)")
_ATTRIBUTE = re.compile(r"\bMCP_DEPRECATED\s*\(|\[\[\s*deprecated\b")


def declaration_head(text: str) -> str:
    """Returns the text up to the first ``;`` or ``{`` outside a string literal."""
    in_string = False
    index = 0
    while index < len(text):
        char = text[index]
        if in_string:
            if char == "\\":
                index += 1
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char in ";{":
            return text[:index]
        index += 1
    return text


def find_violations(root: Path) -> list[str]:
    """Returns one line per violation; an empty list means the tree passes."""
    violations: list[str] = []
    for directory in SCAN_DIRS:
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8")
            relative = path.relative_to(root).as_posix()
            for comment in _DOC_COMMENT.finditer(text):
                body = comment.group(0)
                tag = _TAG.search(body)
                if tag is None or "doc-only" in tag.group(1):
                    continue
                if _ATTRIBUTE.search(declaration_head(text[comment.end():])):
                    continue
                line = text.count("\n", 0, comment.start() + tag.start()) + 1
                violations.append(
                    f"{relative}:{line}: @deprecated needs MCP_DEPRECATED(...) on the declaration "
                    "or a 'doc-only' note after the tag"
                )
    return violations


def main(argv: list[str]) -> int:
    root = Path(argv[1]).resolve() if len(argv) > 1 else REPO_ROOT
    violations = find_violations(root)
    for violation in violations:
        print(violation, file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
