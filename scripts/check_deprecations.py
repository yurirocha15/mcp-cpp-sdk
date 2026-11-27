#!/usr/bin/env python3
"""Check the public headers against the deprecation registry.

``scripts/deprecation_registry.json`` lists the deprecated features: the rows of the
specification's registry of deprecated features (``kind: "spec"``) and the SDK's own deprecations
(``kind: "sdk"``). For each row it names the declarations that carry ``MCP_DEPRECATED``, the ones
that are reported at run time only, and the public identifiers that belong to the feature but stay
unmarked, each with its reason. The check fails when:

1. a listed declaration is missing from its header, or is not marked ``MCP_DEPRECATED`` with the
   message its row requires: the proposal and "earliest removal" for a ``spec`` row, "use
   <replacement>" and "removal no earlier than <release>" for an ``sdk`` row;
2. an ``MCP_DEPRECATED`` marker in ``include/`` belongs to no row;
3. a public identifier that contains one of the registry's stems (a case-insensitive substring
   match, outside comments and string literals) is neither listed nor exempted, so every new
   symbol of a deprecated feature is classified;
4. a Doxygen ``@deprecated`` tag documents a declaration that has no ``MCP_DEPRECATED`` and is not
   listed as reported at run time only;
5. a row's ``notice_test`` names a test that does not exist under ``test/``.

Usage: ``check_deprecations.py [repo-root]``.
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
REGISTRY = Path("scripts") / "deprecation_registry.json"
HEADER_DIR = "include"
TEST_DIR = "test"
SOURCE_SUFFIXES = {".h", ".hpp", ".ipp"}
TEST_SUFFIXES = {".cpp"}

# A Doxygen comment is a ``/** */`` block or a run of consecutive ``///`` or ``//!`` lines.
_DOC_COMMENT = re.compile(r"/\*\*(?s:.*?)\*/|(?:^[ \t]*//[/!].*(?:\n|\Z))+", re.MULTILINE)
_TAG = re.compile(r"[@\\]deprecated\b")
_MARKER = re.compile(r"\bMCP_DEPRECATED\s*\(")
_STANDARD_ATTRIBUTE = re.compile(r"\[\[\s*deprecated\b")
_COMMENT_OR_STRING = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL
)
_STRING = re.compile(r'"((?:\\.|[^"\\\n])*)"')
_IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")
_NAME_BEFORE_END = re.compile(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$")


def blank_comments_and_strings(text: str) -> str:
    """Replaces comments and literals with spaces, keeping every offset and line break."""

    def replace(match: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    return _COMMENT_OR_STRING.sub(replace, text)


def blank_comments(text: str) -> str:
    """Replaces comments with spaces, keeping literals, offsets and line breaks."""

    def replace(match: re.Match[str]) -> str:
        token = match.group(0)
        if token.startswith("/"):
            return re.sub(r"[^\n]", " ", token)
        return token

    return _COMMENT_OR_STRING.sub(replace, text)


def closing_paren(text: str, open_index: int) -> int:
    """Returns the index of the ``)`` that closes the ``(`` at ``open_index``, or -1."""
    depth = 0
    index = open_index
    while index < len(text):
        char = text[index]
        if char == '"':
            match = _STRING.match(text, index)
            index = match.end() if match else index + 1
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    return -1


def declaration_head(code: str, start: int) -> str:
    """Returns the code from ``start`` up to the first ``;``, ``{`` or ``}`` at nesting depth 0.

    ``code`` has its comments and literals blanked.
    """
    depth = 0
    for index in range(start, len(code)):
        char = code[index]
        if char in "(<[":
            depth += 1
        elif char in ")>]":
            depth -= 1
        elif depth <= 0 and char in ";{},":
            return code[start:index]
    return code[start:]


def declared_name(head: str) -> str | None:
    """Returns the name a declaration head declares.

    The name is the identifier before the first ``(`` or ``=`` at depth 0 of the head, or before
    its end when it has neither. Attribute and macro arguments are skipped.
    """
    depth = 0
    index = 0
    while index < len(head):
        char = head[index]
        if head.startswith("[[", index):
            end = head.find("]]", index)
            index = end + 2 if end >= 0 else len(head)
            continue
        if char in "<[":
            depth += 1
        elif char in ">]":
            depth -= 1
        elif depth == 0 and char in "(=":
            before = head[:index].rstrip()
            match = _NAME_BEFORE_END.search(before)
            if match is None:
                return None
            name = match.group(1)
            if name.isupper() and char == "(":
                # A macro such as MCP_DEPRECATED(...) or MCP_API-style function macro: skip it.
                end = closing_paren(head, index)
                if end < 0:
                    return None
                head = head[:match.start()] + " " * (end + 1 - match.start()) + head[end + 1:]
                index = end + 1
                continue
            return name
        index += 1
    match = _NAME_BEFORE_END.search(head.rstrip())
    return match.group(1) if match else None


@dataclass(frozen=True)
class Marker:
    """An ``MCP_DEPRECATED(...)`` in a public header."""

    path: str
    line: int
    name: str | None
    message: str


def find_markers(path: str, text: str) -> list[Marker]:
    """Returns the deprecation markers of one header, with the name each one marks."""
    commentless = blank_comments(text)
    code = blank_comments_and_strings(text)
    markers: list[Marker] = []
    for match in _MARKER.finditer(code):
        line_start = code.rfind("\n", 0, match.start()) + 1
        if code[line_start:match.start()].lstrip().startswith("#"):
            continue  # the macro's own definition
        open_index = match.end() - 1
        close_index = closing_paren(commentless, open_index)
        if close_index < 0:
            continue
        message = "".join(
            part.group(1) for part in _STRING.finditer(commentless, open_index, close_index)
        )
        boundary = max(line_start, code.rfind("{", 0, match.start()) + 1,
                       code.rfind(",", 0, match.start()) + 1)
        before = code[boundary:match.start()].strip()
        if re.fullmatch(r"\w+", before):
            name: str | None = before  # an enumerator: `eName MCP_DEPRECATED("...")`
        else:
            name = declared_name(declaration_head(code, close_index + 1))
        line = text.count("\n", 0, match.start()) + 1
        markers.append(Marker(path, line, name, message.replace('\\"', '"')))
    return markers


def iter_headers(root: Path) -> list[tuple[str, str]]:
    base = root / HEADER_DIR
    headers = []
    for path in sorted(base.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            headers.append((path.relative_to(root).as_posix(), path.read_text(encoding="utf-8")))
    return headers


def test_names(root: Path) -> set[str]:
    names: set[str] = set()
    pattern = re.compile(r"\bTEST(?:_F|_P)?\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)")
    for path in sorted((root / TEST_DIR).rglob("*")):
        if path.is_file() and path.suffix in TEST_SUFFIXES:
            for match in pattern.finditer(path.read_text(encoding="utf-8")):
                names.add(f"{match.group(1)}.{match.group(2)}")
    return names


def split_declaration(entry: str) -> tuple[str, str]:
    path, _, name = entry.rpartition(":")
    return path, name


def message_problems(row: dict, message: str) -> list[str]:
    if row["kind"] == "spec":
        required = [row["proposal"], "earliest removal"]
    else:
        required = [f"use {row['replacement']}", f"removal no earlier than {row['earliest_removal']}"]
    return [f"'{text}'" for text in required if text not in message]


def find_violations(root: Path) -> list[str]:
    """Returns one line per violation; an empty list means the tree passes."""
    registry = json.loads((root / REGISTRY).read_text(encoding="utf-8"))
    rows = registry["rows"]
    stems = [stem.lower() for stem in registry["stems"]]
    unrelated = [re.compile(entry["pattern"]) for entry in registry["unrelated"]]

    headers = iter_headers(root)
    markers = [marker for path, text in headers for marker in find_markers(path, text)]
    marker_by_place = {(marker.path, marker.name): marker for marker in markers}

    violations: list[str] = []
    listed: dict[tuple[str, str], dict] = {}
    runtime_only: set[tuple[str, str]] = set()
    named: set[str] = set()
    exemptions: list[re.Pattern[str]] = list(unrelated)
    header_text = dict(headers)

    for row in rows:
        for entry in row["declarations"]:
            path, name = split_declaration(entry)
            listed[(path, name)] = row
            named.add(name)
            marker = marker_by_place.get((path, name))
            if marker is None:
                text = header_text.get(path, "")
                found = re.search(rf"\b{re.escape(name)}\b", blank_comments_and_strings(text))
                where = "has no MCP_DEPRECATED marker" if found else "is not declared there"
                violations.append(f"{path}: {name} ({row['id']}) {where}")
                continue
            missing = message_problems(row, marker.message)
            if missing:
                violations.append(
                    f"{path}:{marker.line}: the MCP_DEPRECATED message of {name} lacks "
                    + ", ".join(missing)
                )
        for entry in row.get("runtime_only", []):
            path, name = split_declaration(entry["declaration"])
            runtime_only.add((path, name))
            named.add(name)
            if not re.search(rf"\b{re.escape(name)}\b", blank_comments_and_strings(header_text.get(path, ""))):
                violations.append(f"{path}: {name} ({row['id']}) is not declared there")
        exemptions.extend(re.compile(entry["pattern"]) for entry in row.get("exemptions", []))

    for marker in markers:
        if (marker.path, marker.name) not in listed:
            violations.append(
                f"{marker.path}:{marker.line}: MCP_DEPRECATED on {marker.name or 'an unnamed declaration'} "
                f"belongs to no row of {REGISTRY.as_posix()}"
            )

    reported: set[str] = set()
    for path, text in headers:
        code = blank_comments_and_strings(text)
        for match in _IDENTIFIER.finditer(code):
            identifier = match.group(0)
            if identifier in reported or identifier in named:
                continue
            lowered = identifier.lower()
            if not any(stem in lowered for stem in stems):
                continue
            if any(pattern.fullmatch(identifier) for pattern in exemptions):
                continue
            reported.add(identifier)
            line = text.count("\n", 0, match.start()) + 1
            violations.append(
                f"{path}:{line}: {identifier} names a deprecated feature but is neither listed nor "
                f"exempted in {REGISTRY.as_posix()}"
            )

        for comment in _DOC_COMMENT.finditer(text):
            tag = _TAG.search(comment.group(0))
            if tag is None:
                continue
            head = declaration_head(code, comment.end())
            if _MARKER.search(head) or _STANDARD_ATTRIBUTE.search(head):
                continue
            name = declared_name(head)
            if (path, name) in runtime_only:
                continue
            line = text.count("\n", 0, comment.start() + tag.start()) + 1
            violations.append(
                f"{path}:{line}: @deprecated needs MCP_DEPRECATED(...) on the declaration, or the "
                f"declaration listed under runtime_only in {REGISTRY.as_posix()}"
            )

    known_tests = test_names(root)
    for row in rows:
        test = row.get("notice_test")
        if test and test not in known_tests:
            violations.append(f"{REGISTRY.as_posix()}: notice_test {test} of {row['id']} does not exist")

    return violations


def main(argv: list[str]) -> int:
    root = Path(argv[1]).resolve() if len(argv) > 1 else REPO_ROOT
    violations = find_violations(root)
    for violation in violations:
        print(violation, file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
