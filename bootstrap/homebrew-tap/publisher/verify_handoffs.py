#!/usr/bin/env python3
"""Select and verify same-run Homebrew handoffs across rerun attempts."""

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
FIELDS = frozenset(
    {
        "schema_version",
        "artifact_name",
        "run_id",
        "run_attempt",
        "bottle_tag",
        "bottle_file",
        "bottle_sha256",
        "json_file",
        "json_sha256",
        "formula_file",
        "formula_sha256",
    }
)


def artifact_name(tag: str, run_id: str, run_attempt: str) -> str:
    if TAG.fullmatch(tag) is None:
        raise ValueError("bottle tag is malformed")
    if DECIMAL.fullmatch(run_id) is None or DECIMAL.fullmatch(run_attempt) is None:
        raise ValueError("workflow run identity is malformed")
    return f"bottle-{tag}-{run_id}-{run_attempt}"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _verified(path: Path, run_id: str, current_attempt: int) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="ascii"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"handoff is not ASCII JSON: {path}") from error
    if not isinstance(value, dict) or set(value) != FIELDS or value["schema_version"] != 2:
        raise ValueError(f"invalid handoff schema: {path}")
    if value["run_id"] != run_id:
        raise ValueError(f"handoff belongs to another workflow run: {path}")
    attempt = value["run_attempt"]
    if not isinstance(attempt, str) or not attempt.isdecimal() or str(int(attempt)) != attempt:
        raise ValueError(f"handoff attempt is malformed: {path}")
    if not 1 <= int(attempt) <= current_attempt:
        raise ValueError(f"handoff attempt is newer than the current rerun: {path}")
    tag = value["bottle_tag"]
    if not isinstance(tag, str) or value["artifact_name"] != artifact_name(tag, run_id, attempt):
        raise ValueError(f"handoff artifact name is not canonical: {path}")
    if path.parent.name != value["artifact_name"]:
        raise ValueError(f"downloaded artifact directory changed identity: {path}")
    for name_field, digest_field, suffix in (
        ("bottle_file", "bottle_sha256", ".tar.gz"),
        ("json_file", "json_sha256", ".json"),
        ("formula_file", "formula_sha256", ".rb"),
    ):
        name = value[name_field]
        digest = value[digest_field]
        if (
            not isinstance(name, str)
            or Path(name).name != name
            or not name.endswith(suffix)
            or not isinstance(digest, str)
            or DIGEST.fullmatch(digest) is None
        ):
            raise ValueError(f"invalid artifact identity: {path}")
        artifact = path.parent / name
        if not artifact.is_file() or artifact.is_symlink() or sha256(artifact) != digest:
            raise ValueError(f"artifact digest mismatch: {artifact}")
    metadata = json.loads((path.parent / value["json_file"]).read_text(encoding="utf-8"))
    serialized = json.dumps(metadata, sort_keys=True, separators=(",", ":"))
    if tag not in serialized or value["bottle_sha256"] not in serialized:
        raise ValueError(f"bottle metadata does not bind its tag and archive digest: {path}")
    if ROOT_URL not in serialized:
        raise ValueError(f"bottle metadata has an unexpected root URL: {path}")
    return value


def verify(
    directory: Path,
    run_id: str,
    run_attempt: str,
    expected_tags: set[str] | None = None,
    *,
    selected_directory: Path | None = None,
    formula_output: Path | None = None,
) -> dict[str, object]:
    if not run_id.isdecimal() or str(int(run_id)) != run_id:
        raise ValueError("workflow run ID is malformed")
    if not run_attempt.isdecimal() or str(int(run_attempt)) != run_attempt:
        raise ValueError("workflow run attempt is malformed")
    current_attempt = int(run_attempt)
    candidates: dict[str, dict[int, tuple[Path, dict[str, object]]]] = {}
    handoff_paths = sorted(directory.glob("*/handoff.json"))
    if not handoff_paths:
        raise ValueError("no bottle handoffs found")
    for path in handoff_paths:
        value = _verified(path, run_id, current_attempt)
        tag = str(value["bottle_tag"])
        attempt = int(str(value["run_attempt"]))
        by_attempt = candidates.setdefault(tag, {})
        if attempt in by_attempt:
            raise ValueError("duplicate bottle tag and attempt handoff")
        by_attempt[attempt] = (path.parent, value)
    if expected_tags is not None and set(candidates) != expected_tags:
        raise ValueError("bottle tag set does not match the approved matrix")
    selected = [candidates[tag][max(candidates[tag])] for tag in sorted(candidates)]
    formula_digests = {str(value["formula_sha256"]) for _, value in selected}
    if len(formula_digests) != 1:
        raise ValueError("selected bottles were built from different formula bytes")
    if selected_directory is not None:
        if selected_directory.exists():
            raise ValueError("selected handoff directory must be new")
        selected_directory.mkdir(parents=True)
        for source, value in selected:
            destination = selected_directory / str(value["artifact_name"])
            shutil.copytree(source, destination, symlinks=False)
    if formula_output is not None:
        if formula_output.exists() or formula_output.is_symlink():
            raise ValueError("selected formula output must be new")
        source, value = selected[0]
        formula_output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / str(value["formula_file"]), formula_output)
    return {
        "schema_version": 2,
        "run_id": run_id,
        "assembly_run_attempt": run_attempt,
        "bottles": [value for _, value in selected],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", required=True)
    parser.add_argument("--expected-tag", action="append", default=[])
    parser.add_argument("--selected-directory", type=Path)
    parser.add_argument("--formula-output", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        expected = set(args.expected_tag) if args.expected_tag else None
        ledger = verify(
            args.directory,
            args.run_id,
            args.run_attempt,
            expected,
            selected_directory=args.selected_directory,
            formula_output=args.formula_output,
        )
        args.output.write_text(
            json.dumps(ledger, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="ascii",
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(f"verify-handoffs: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
