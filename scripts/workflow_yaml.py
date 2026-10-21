"""Fail-closed duplicate-key validation for checked-in Actions workflows.

Actionlint remains the full YAML and Actions parser.  This dependency-free pass
runs before it and rejects mapping-key shadowing, including inside sequence
items, so Python policy code can never silently collapse duplicate jobs or
security settings.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import re
from typing import Iterable


class WorkflowYamlError(ValueError):
    """Raised when workflow mapping structure is ambiguous or duplicated."""


_KEY = r'(?:[A-Za-z_][A-Za-z0-9_.-]*|<<|"(?:[^"\\]|\\.)*"|\'(?:[^\']|\'\')*\')'
_ENTRY = re.compile(
    rf"^(?P<indent> *)(?:(?P<item>-) +)?(?P<key>{_KEY}):(?P<value>.*)$"
)
_BLOCK_SCALAR = re.compile(r"^[|>](?:[+-]?[1-9]?|[1-9]?[+-])$")


@dataclass
class _Mapping:
    key_indent: int
    keys: dict[str, int] = field(default_factory=dict)


def _key_value(token: str) -> str:
    if token.startswith('"'):
        value = json.loads(token)
        if not isinstance(value, str):
            raise WorkflowYamlError("double-quoted mapping key is not a string")
        return value
    if token.startswith("'"):
        return token[1:-1].replace("''", "'")
    return token


def validate_workflow_yaml(text: str, *, label: str = "workflow") -> None:
    """Reject duplicate YAML mapping keys without interpreting scalar values."""

    if not text.strip():
        raise WorkflowYamlError(f"{label}: workflow is empty")
    mappings: list[_Mapping] = []
    block_scalar_indent: int | None = None
    for line_number, raw in enumerate(text.splitlines(), start=1):
        if "\t" in raw:
            raise WorkflowYamlError(f"{label}:{line_number}: tab characters are forbidden")
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        if block_scalar_indent is not None:
            if indent > block_scalar_indent:
                continue
            block_scalar_indent = None

        match = _ENTRY.fullmatch(raw)
        if match is None:
            continue
        key_indent = indent + 2 if match.group("item") else indent
        if match.group("item"):
            while mappings and mappings[-1].key_indent >= key_indent:
                mappings.pop()
            mappings.append(_Mapping(key_indent))
        else:
            while mappings and mappings[-1].key_indent > key_indent:
                mappings.pop()
            if not mappings or mappings[-1].key_indent < key_indent:
                mappings.append(_Mapping(key_indent))

        key = _key_value(match.group("key"))
        current = mappings[-1]
        if key in current.keys:
            first = current.keys[key]
            raise WorkflowYamlError(
                f"{label}:{line_number}: duplicate mapping key {key!r}; first declared at line {first}"
            )
        current.keys[key] = line_number
        if _BLOCK_SCALAR.fullmatch(match.group("value").strip()):
            block_scalar_indent = key_indent


def workflow_paths(root: Path = Path(".")) -> tuple[Path, ...]:
    """Return every source and downstream-bootstrap Actions workflow."""

    candidates: set[Path] = set()
    source = root / ".github" / "workflows"
    if source.is_dir():
        candidates.update(
            path for path in source.iterdir() if path.suffix in {".yml", ".yaml"}
        )
    bootstrap = root / "bootstrap"
    if bootstrap.is_dir():
        candidates.update(
            path
            for path in bootstrap.rglob("*")
            if path.is_file()
            and path.suffix in {".yml", ".yaml"}
            and path.parent.name == "workflows"
            and path.parent.parent.name == ".github"
        )
    return tuple(sorted(candidates))


def validate_workflows(paths: Iterable[Path]) -> None:
    paths = tuple(paths)
    if not paths:
        raise WorkflowYamlError("no GitHub Actions workflows were found")
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise WorkflowYamlError(f"{path}: workflow is unreadable: {error}") from error
        validate_workflow_yaml(text, label=str(path))
