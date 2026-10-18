"""Bounded, fail-closed JSON parsing for release security boundaries."""

from __future__ import annotations

import json
from pathlib import Path

from .model import ValidationError


DEFAULT_LIMIT = 1024 * 1024


def loads_strict(data: str | bytes, *, label: str) -> object:
    """Parse JSON while rejecting duplicate keys and non-finite numbers."""

    def reject_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise ValidationError(f"{label} contains duplicate key: {key}")
            result[key] = value
        return result

    def reject_constant(value: str) -> None:
        raise ValidationError(f"{label} contains non-finite number: {value}")

    try:
        text = data.decode("utf-8") if isinstance(data, bytes) else data
        return json.loads(
            text,
            object_pairs_hook=reject_duplicates,
            parse_constant=reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"{label} is malformed JSON") from error


def load_strict(path: Path, *, label: str, limit: int = DEFAULT_LIMIT) -> object:
    """Read and strictly parse one non-empty, size-bounded JSON file."""

    try:
        if path.is_symlink() or not path.is_file():
            raise ValidationError(f"{label} must be a regular file")
        expected_size = path.stat().st_size
        if not 0 < expected_size <= limit:
            raise ValidationError(
                f"{label} must be non-empty and at most {limit} bytes"
            )
        data = path.read_bytes()
    except OSError as error:
        raise ValidationError(f"cannot read {label}: {path}") from error
    if len(data) != expected_size:
        raise ValidationError(f"{label} changed while it was read")
    return loads_strict(data, label=label)
