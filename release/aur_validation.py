"""Generate and validate the installed AUR package consumer boundary."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
from typing import Sequence

from .artifacts import write_atomic
from .consumer_validation import validate_elf_dependencies, write_consumer_project
from .model import SemVer, ValidationError


_ARCHITECTURES = frozenset({"x86_64"})
_PACKAGE_EXTENSION = r"pkg\.tar\.(?:zst|xz|gz|bz2|lrz|lzo|Z)"


def write_consumer(directory: Path, version_text: str) -> None:
    """Write consumers that call the exported symbol from both installed variants."""

    write_consumer_project(directory, version_text, replace=True)


def validate_package_list(
    *,
    directory: Path,
    package_list: Path,
    output: Path,
    version_text: str,
    architecture: str,
) -> tuple[Path, ...]:
    """Validate makepkg's exact two-package output and write a canonical install list."""

    version = SemVer.parse(version_text, stable_only=True)
    if architecture not in _ARCHITECTURES:
        raise ValidationError("AUR validation architecture is unsupported")
    root = directory.resolve()
    if directory.is_symlink() or not directory.is_dir():
        raise ValidationError("AUR package directory must be a real directory")
    try:
        lines = package_list.read_text(encoding="utf-8").splitlines()
    except UnicodeError as error:
        raise ValidationError("makepkg package list is not UTF-8") from error
    if (
        len(lines) != 2
        or len(set(lines)) != 2
        or any(not line or line != line.strip() for line in lines)
    ):
        raise ValidationError("makepkg must report exactly two unique package paths")

    package_pattern = re.compile(
        rf"(?P<name>mcp-cpp-sdk(?:-static)?)-{re.escape(str(version))}-1-"
        rf"{re.escape(architecture)}\.{_PACKAGE_EXTENSION}"
    )
    packages: list[Path] = []
    package_names: set[str] = set()
    for line in lines:
        path = Path(line)
        if not path.is_absolute():
            path = directory / path
        resolved = path.resolve()
        if resolved.parent != root:
            raise ValidationError("makepkg package path escapes the AUR build directory")
        if path.is_symlink() or not resolved.is_file() or resolved.stat().st_size == 0:
            raise ValidationError("makepkg package output is missing, empty, or a symlink")
        match = package_pattern.fullmatch(resolved.name)
        if match is None:
            raise ValidationError(f"unexpected AUR package filename: {resolved.name}")
        package_names.add(match.group("name"))
        packages.append(resolved)
    if package_names != {"mcp-cpp-sdk", "mcp-cpp-sdk-static"}:
        raise ValidationError("AUR split-package names are incomplete or duplicated")
    canonical = tuple(sorted(packages, key=lambda path: path.name))
    write_atomic(output, "".join(f"{path}\n" for path in canonical).encode("utf-8"))
    return canonical


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    consumer = subcommands.add_parser("consumer")
    consumer.add_argument("--directory", type=Path, required=True)
    consumer.add_argument("--version", required=True)
    packages = subcommands.add_parser("packages")
    packages.add_argument("--directory", type=Path, required=True)
    packages.add_argument("--package-list", type=Path, required=True)
    packages.add_argument("--output", type=Path, required=True)
    packages.add_argument("--version", required=True)
    packages.add_argument("--architecture", required=True)
    elf = subcommands.add_parser("elf")
    elf.add_argument("--shared-readelf", type=Path, required=True)
    elf.add_argument("--static-readelf", type=Path, required=True)
    elf.add_argument("--version", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "consumer":
            write_consumer(args.directory, args.version)
        elif args.command == "packages":
            validate_package_list(
                directory=args.directory,
                package_list=args.package_list,
                output=args.output,
                version_text=args.version,
                architecture=args.architecture,
            )
        else:
            validate_elf_dependencies(
                shared_readelf=args.shared_readelf,
                static_readelf=args.static_readelf,
                version_text=args.version,
            )
    except (OSError, ValidationError) as error:
        raise SystemExit(f"aur-validation: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
