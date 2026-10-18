"""Prepare and validate byte-exact pre-anchor Homebrew package tests."""

from __future__ import annotations

import argparse
from pathlib import Path
import platform
import re
import subprocess
from typing import Sequence

from .consumer_validation import write_consumer_project
from .model import SemVer, ValidationError


def _regular_bytes(path: Path, label: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValidationError(f"{label} must be a regular file")
    return path.read_bytes()


def compare_files(expected: Path, actual: Path) -> None:
    if _regular_bytes(expected, "expected package file") != _regular_bytes(
        actual, "actual package file"
    ):
        raise ValidationError("package file copy differs from the signed candidate bytes")


def copy_exact_file(source: Path, destination: Path) -> None:
    content = _regular_bytes(source, "signed candidate file")
    if destination.exists() or destination.is_symlink():
        raise ValidationError("package cache destination must be new")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(content)
    compare_files(source, destination)


def _run(arguments: list[str]) -> str:
    return subprocess.run(
        arguments,
        check=True,
        capture_output=True,
        text=True,
        timeout=600,
    ).stdout


def validate_homebrew_consumers(*, version_text: str, prefix: Path, work: Path) -> None:
    version = SemVer.parse(version_text, stable_only=True)
    system = platform.system()
    if system not in {"Darwin", "Linux"}:
        raise ValidationError("Homebrew consumer validation supports only macOS and Linux")
    if work.exists():
        raise ValidationError("Homebrew consumer work directory must be new")
    write_consumer_project(work, str(version))
    build = work / "build"
    _run(["cmake", "-S", str(work), "-B", str(build), f"-DCMAKE_PREFIX_PATH={prefix}"])
    _run(["cmake", "--build", str(build), "--config", "Release"])
    shared = build / "shared-consumer"
    static = build / "static-consumer"
    _run([str(shared)])
    _run([str(static)])
    if system == "Darwin":
        shared_inspection = ["otool", "-L", str(shared)]
        static_inspection = ["otool", "-L", str(static)]
    else:
        shared_inspection = ["ldd", str(shared)]
        static_inspection = ["ldd", str(static)]
    shared_links = _run(shared_inspection)
    static_links = _run(static_inspection)
    library = r"libmcp-cpp-sdk(?:(?:\.[0-9.]+)?\.dylib|\.so(?:\.[0-9.]+)?)"
    expected = (
        f"libmcp-cpp-sdk.{version.abi_version}.dylib"
        if system == "Darwin"
        else f"libmcp-cpp-sdk.so.{version.abi_version}"
    )
    if set(re.findall(library, shared_links)) != {expected}:
        raise ValidationError("shared Homebrew consumer has the wrong SDK dependency")
    if re.search(library, static_links) is not None:
        raise ValidationError("static Homebrew consumer unexpectedly links the shared SDK")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    compare_file = commands.add_parser("compare-files")
    compare_file.add_argument("--expected", type=Path, required=True)
    compare_file.add_argument("--actual", type=Path, required=True)
    copy_file = commands.add_parser("copy-exact-file")
    copy_file.add_argument("--source", type=Path, required=True)
    copy_file.add_argument("--destination", type=Path, required=True)
    consumer = commands.add_parser("homebrew-consumer")
    consumer.add_argument("--version", required=True)
    consumer.add_argument("--prefix", type=Path, required=True)
    consumer.add_argument("--work", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.command == "compare-files":
            compare_files(args.expected, args.actual)
        elif args.command == "copy-exact-file":
            copy_exact_file(args.source, args.destination)
        elif args.command == "homebrew-consumer":
            validate_homebrew_consumers(
                version_text=args.version,
                prefix=args.prefix,
                work=args.work,
            )
        else:
            raise AssertionError(f"unhandled command: {args.command}")
    except (
        OSError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        ValidationError,
    ) as error:
        raise SystemExit(f"package-validation: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
