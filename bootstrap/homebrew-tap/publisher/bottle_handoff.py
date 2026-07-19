#!/usr/bin/env python3
"""Create one digest-bound Homebrew bottle artifact handoff."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys


DIGEST = re.compile(r"[0-9a-f]{64}")
DECIMAL = re.compile(r"[1-9][0-9]*")
TAG = re.compile(r"[a-z0-9_]+")
ROOT_URL = "https://ghcr.io/v2/yurirocha15/mcp-cpp-sdk"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact_name(tag: str, run_id: str, run_attempt: str) -> str:
    if TAG.fullmatch(tag) is None:
        raise ValueError("bottle tag is malformed")
    if DECIMAL.fullmatch(run_id) is None or DECIMAL.fullmatch(run_attempt) is None:
        raise ValueError("workflow run identity is malformed")
    return f"bottle-{tag}-{run_id}-{run_attempt}"


def _regular(path: Path, suffix: str) -> Path:
    if not path.name.endswith(suffix):
        raise ValueError(f"artifact has the wrong extension: {path}")
    if not path.is_file() or path.is_symlink() or path.stat().st_size <= 0:
        raise ValueError(f"artifact is not a regular non-empty file: {path}")
    return path


def discover(directory: Path) -> tuple[Path, Path]:
    """Find exactly one Homebrew bottle archive and its JSON metadata."""

    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("bottle discovery root is not a real directory")
    bottles = sorted(directory.glob("mcp-cpp-sdk--*.bottle*.tar.gz"))
    metadata = sorted(directory.glob("mcp-cpp-sdk--*.bottle.json"))
    if len(bottles) != 1 or len(metadata) != 1:
        raise ValueError("Homebrew must produce exactly one bottle and metadata file")
    return _regular(bottles[0], ".tar.gz"), _regular(metadata[0], ".json")


def create(
    *,
    bottle: Path,
    metadata: Path,
    formula: Path,
    output: Path,
    tag: str,
    run_id: str,
    run_attempt: str,
) -> dict[str, object]:
    name = artifact_name(tag, run_id, run_attempt)
    bottle = _regular(bottle, ".tar.gz")
    metadata = _regular(metadata, ".json")
    formula = _regular(formula, ".rb")
    if output.exists():
        raise ValueError("handoff output directory must be new")
    bottle_digest = sha256(bottle)
    try:
        metadata_value = json.loads(metadata.read_text(encoding="utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("bottle metadata is not UTF-8 JSON") from error
    serialized = json.dumps(metadata_value, sort_keys=True, separators=(",", ":"))
    if tag not in serialized or bottle_digest not in serialized or ROOT_URL not in serialized:
        raise ValueError("bottle metadata does not bind its tag, digest, and root URL")
    value: dict[str, object] = {
        "schema_version": 2,
        "artifact_name": name,
        "run_id": run_id,
        "run_attempt": run_attempt,
        "bottle_tag": tag,
        "bottle_file": bottle.name,
        "bottle_sha256": bottle_digest,
        "json_file": metadata.name,
        "json_sha256": sha256(metadata),
        "formula_file": formula.name,
        "formula_sha256": sha256(formula),
    }
    output.mkdir(parents=True)
    for source in (bottle, metadata, formula):
        shutil.copyfile(source, output / source.name)
    (output / "handoff.json").write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="ascii",
    )
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--bottle", type=Path)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--formula", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", required=True)
    args = parser.parse_args()
    try:
        if args.directory is not None:
            if args.bottle is not None or args.metadata is not None:
                raise ValueError("use either bottle discovery or explicit paths")
            bottle, metadata = discover(args.directory)
        elif args.bottle is not None and args.metadata is not None:
            bottle, metadata = args.bottle, args.metadata
        else:
            raise ValueError("bottle and metadata inputs are incomplete")
        create(
            bottle=bottle,
            metadata=metadata,
            formula=args.formula,
            output=args.output,
            tag=args.tag,
            run_id=args.run_id,
            run_attempt=args.run_attempt,
        )
    except (OSError, UnicodeError, ValueError) as error:
        print(f"bottle-handoff: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
