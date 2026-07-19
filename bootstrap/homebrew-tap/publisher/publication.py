#!/usr/bin/env python3
"""Bind Homebrew publication artifacts and verify the publication bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


DIGEST = re.compile(r"[0-9a-f]{64}")
SHA = re.compile(r"[0-9a-f]{40}")
DECIMAL = re.compile(r"[1-9][0-9]*")
SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+/-]{0,199}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def bind_artifact_ids(ledger: object, response: object) -> dict[str, object]:
    if not isinstance(ledger, dict) or ledger.get("schema_version") != 2:
        raise ValueError("bottle ledger is malformed")
    bottles = ledger.get("bottles")
    if not isinstance(bottles, list) or len(bottles) != 4:
        raise ValueError("bottle ledger does not contain the complete matrix")
    pages = response if isinstance(response, list) else [response]
    if not pages or any(
        not isinstance(page, dict) or not isinstance(page.get("artifacts"), list)
        for page in pages
    ):
        raise ValueError("GitHub artifact response is malformed")
    by_name: dict[str, list[dict[str, object]]] = {}
    for page in pages:
        for raw in page["artifacts"]:
            if not isinstance(raw, dict) or not isinstance(raw.get("name"), str):
                raise ValueError("GitHub artifact entry is malformed")
            by_name.setdefault(raw["name"], []).append(raw)
    records = []
    for bottle in bottles:
        if not isinstance(bottle, dict) or not isinstance(bottle.get("artifact_name"), str):
            raise ValueError("bottle artifact identity is missing")
        name = bottle["artifact_name"]
        matches = by_name.get(name, [])
        if len(matches) != 1:
            raise ValueError(f"selected bottle artifact is missing or duplicated: {name}")
        artifact = matches[0]
        artifact_id = artifact.get("id")
        if (
            type(artifact_id) is not int
            or artifact_id <= 0
            or artifact.get("expired") is not False
            or artifact.get("name") != name
        ):
            raise ValueError(f"selected bottle artifact is expired or malformed: {name}")
        records.append(
            {
                "id": artifact_id,
                "name": name,
                "run_attempt": bottle["run_attempt"],
            }
        )
    return {**ledger, "artifact_ids": sorted(records, key=lambda item: item["name"])}


def write_checksums(directory: Path, output: Path) -> None:
    if output.parent.resolve() != directory.resolve():
        raise ValueError("SHA256SUMS must be inside the publication directory")
    paths = sorted(
        path
        for path in directory.rglob("*")
        if path.is_file() and not path.is_symlink() and path.resolve() != output.resolve()
    )
    if not paths:
        raise ValueError("publication directory is empty")
    output.write_text(
        "".join(f"{sha256(path)}  {path.relative_to(directory).as_posix()}\n" for path in paths),
        encoding="ascii",
    )


def verify_checksums(directory: Path, checksums: Path) -> dict[str, str]:
    records: dict[str, str] = {}
    for line in checksums.read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+/-]{0,199})", line)
        if match is None or match.group(2) in records:
            raise ValueError("publication checksum record is malformed or duplicated")
        records[match.group(2)] = match.group(1)
    files = {
        path.relative_to(directory).as_posix()
        for path in directory.rglob("*")
        if path.is_file() and not path.is_symlink() and path.resolve() != checksums.resolve()
    }
    if set(records) != files:
        raise ValueError("publication checksum inventory is not exact")
    for name, expected in records.items():
        relative = Path(name)
        if (
            SAFE_NAME.fullmatch(name) is None
            or relative.is_absolute()
            or ".." in relative.parts
            or sha256(directory / relative) != expected
        ):
            raise ValueError(f"publication digest mismatch: {name}")
    return records


def verify_finalization(
    ledger: object, *, run_id: str, publish_attempt: str, expected_head: str
) -> None:
    """Bind finalization evidence to the exact workflow run and formula head."""

    if (
        DECIMAL.fullmatch(run_id) is None
        or DECIMAL.fullmatch(publish_attempt) is None
        or SHA.fullmatch(expected_head) is None
    ):
        raise ValueError("finalization workflow identity is malformed")
    if (
        not isinstance(ledger, dict)
        or ledger.get("run_id") != run_id
        or ledger.get("publish_run_attempt") != publish_attempt
        or ledger.get("bottled_head_sha") != expected_head
    ):
        raise ValueError("finalization ledger does not match this workflow run")


def _load(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    bind = commands.add_parser("bind-artifacts")
    bind.add_argument("--ledger", type=Path, required=True)
    bind.add_argument("--artifacts", type=Path, required=True)
    bind.add_argument("--output", type=Path, required=True)
    create = commands.add_parser("create-checksums")
    create.add_argument("--directory", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    verify = commands.add_parser("verify-checksums")
    verify.add_argument("--directory", type=Path, required=True)
    verify.add_argument("--checksums", type=Path, required=True)
    finalization = commands.add_parser("verify-finalization")
    finalization.add_argument("--ledger", type=Path, required=True)
    finalization.add_argument("--run-id", required=True)
    finalization.add_argument("--publish-attempt", required=True)
    finalization.add_argument("--expected-head", required=True)
    args = parser.parse_args()
    try:
        if args.command == "bind-artifacts":
            value = bind_artifact_ids(_load(args.ledger), _load(args.artifacts))
            args.output.write_text(
                json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="ascii",
            )
        elif args.command == "create-checksums":
            write_checksums(args.directory, args.output)
        elif args.command == "verify-checksums":
            verify_checksums(args.directory, args.checksums)
        else:
            verify_finalization(
                _load(args.ledger),
                run_id=args.run_id,
                publish_attempt=args.publish_attempt,
                expected_head=args.expected_head,
            )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(f"publication: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
