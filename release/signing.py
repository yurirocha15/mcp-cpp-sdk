"""Validate checksum boundaries around the isolated GPG signing operation."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
from typing import Sequence

from .artifacts import build_sha256sums, write_atomic
from .model import ValidationError


_ASSET_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}")
_DIGEST_RE = re.compile(r"[0-9a-f]{64}")
_EXCLUDED_FINAL_NAMES = frozenset({"RELEASE_NOTES.md", "SHA256SUMS", "SHA256SUMS.asc"})


def _regular_files(directory: Path) -> tuple[Path, ...]:
    if directory.is_symlink() or not directory.is_dir():
        raise ValidationError("signing directory must be a real directory")
    files: list[Path] = []
    for path in sorted(directory.iterdir(), key=lambda item: item.name):
        if path.is_symlink() or not path.is_file():
            raise ValidationError(f"signing input is not a regular file: {path.name}")
        if _ASSET_NAME_RE.fullmatch(path.name) is None:
            raise ValidationError(f"signing input has an unsafe asset name: {path.name!r}")
        files.append(path)
    return tuple(files)


def _parse_checksums(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except UnicodeError as error:
        raise ValidationError(f"{path.name} is not ASCII") from error
    records: dict[str, str] = {}
    for line in lines:
        digest, separator, name = line.partition("  ")
        if (
            not separator
            or _DIGEST_RE.fullmatch(digest) is None
            or _ASSET_NAME_RE.fullmatch(name) is None
            or name in records
        ):
            raise ValidationError(f"{path.name} contains a malformed or duplicate record")
        records[name] = digest
    if not records:
        raise ValidationError(f"{path.name} is empty")
    return records


def _verify_checksums(
    directory: Path,
    checksum_path: Path,
    *,
    excluded_names: frozenset[str],
) -> None:
    records = _parse_checksums(checksum_path)
    expected = {
        path.name
        for path in _regular_files(directory)
        if path.name not in excluded_names and path != checksum_path
    }
    if set(records) != expected:
        raise ValidationError(f"{checksum_path.name} does not cover the exact file boundary")
    for name, expected_digest in records.items():
        actual_digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
        if actual_digest != expected_digest:
            raise ValidationError(f"checksum mismatch for signing input: {name}")


def _detached_assets(directory: Path) -> tuple[str, ...]:
    names = tuple(
        path.name
        for path in _regular_files(directory)
        if path.name == "release-manifest.json" or path.name.endswith((".tar.gz", ".zip"))
    )
    if "release-manifest.json" not in names or not any(name.endswith(".tar.gz") for name in names):
        raise ValidationError("detached-signature asset inventory is incomplete")
    return names


def _assert_external_plan(directory: Path, plan: Path) -> None:
    try:
        plan.resolve().relative_to(directory.resolve())
    except ValueError:
        return
    raise ValidationError("signing plan must be outside the signed asset directory")


def _read_plan(plan: Path) -> tuple[str, ...]:
    try:
        names = tuple(plan.read_text(encoding="ascii").splitlines())
    except UnicodeError as error:
        raise ValidationError("signing plan is not ASCII") from error
    if (
        not names
        or tuple(sorted(names)) != names
        or len(set(names)) != len(names)
        or any(_ASSET_NAME_RE.fullmatch(name) is None for name in names)
    ):
        raise ValidationError("signing plan is malformed or noncanonical")
    return names


def prepare_signing(directory: Path, plan: Path) -> tuple[str, ...]:
    """Verify and consume UNSIGNED-SHA256SUMS, then emit the exact signature plan."""

    _assert_external_plan(directory, plan)
    boundary = directory / "UNSIGNED-SHA256SUMS"
    _verify_checksums(
        directory,
        boundary,
        excluded_names=frozenset({"RELEASE_NOTES.md", "UNSIGNED-SHA256SUMS"}),
    )
    assets = _detached_assets(directory)
    boundary.unlink()
    write_atomic(plan, "".join(f"{name}\n" for name in assets).encode("ascii"))
    return assets


def write_final_checksums(directory: Path, plan: Path) -> None:
    """Require every planned detached signature and write canonical SHA256SUMS."""

    _assert_external_plan(directory, plan)
    assets = _read_plan(plan)
    if (directory / "UNSIGNED-SHA256SUMS").exists():
        raise ValidationError("unsigned checksum boundary was not consumed")
    if (directory / "SHA256SUMS").exists() or (directory / "SHA256SUMS.asc").exists():
        raise ValidationError("final checksum boundary already exists")
    for name in assets:
        signature = directory / f"{name}.asc"
        if not signature.is_file() or signature.is_symlink() or signature.stat().st_size == 0:
            raise ValidationError(f"planned detached signature is missing: {signature.name}")
    actual_detached = {
        path.name.removesuffix(".asc")
        for path in _regular_files(directory)
        if path.name.endswith((".tar.gz.asc", ".zip.asc"))
        or path.name == "release-manifest.json.asc"
    }
    if actual_detached != set(assets):
        raise ValidationError("detached signature inventory differs from the signing plan")
    checksum_inputs = [
        path for path in _regular_files(directory) if path.name not in _EXCLUDED_FINAL_NAMES
    ]
    write_atomic(directory / "SHA256SUMS", build_sha256sums(checksum_inputs))


def verify_final_boundary(directory: Path, plan: Path) -> None:
    """Verify the checksum signature exists and SHA256SUMS still binds every final asset."""

    _assert_external_plan(directory, plan)
    _read_plan(plan)
    signature = directory / "SHA256SUMS.asc"
    if not signature.is_file() or signature.is_symlink() or signature.stat().st_size == 0:
        raise ValidationError("SHA256SUMS detached signature is missing")
    _verify_checksums(
        directory,
        directory / "SHA256SUMS",
        excluded_names=_EXCLUDED_FINAL_NAMES,
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    for name in ("prepare", "finalize", "verify"):
        command = subcommands.add_parser(name)
        command.add_argument("--directory", type=Path, required=True)
        command.add_argument("--plan", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "prepare":
            prepare_signing(args.directory, args.plan)
        elif args.command == "finalize":
            write_final_checksums(args.directory, args.plan)
        else:
            verify_final_boundary(args.directory, args.plan)
    except (OSError, ValidationError) as error:
        raise SystemExit(f"signing-boundary: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
