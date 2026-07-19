"""Generate and validate the installed AUR package consumer boundary."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
from typing import Sequence

from .artifacts import write_atomic
from .model import SemVer, ValidationError


_ARCHITECTURES = frozenset({"x86_64"})
_PACKAGE_EXTENSION = r"pkg\.tar\.(?:zst|xz|gz|bz2|lrz|lzo|Z)"
_NEEDED_RE = re.compile(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]")
_SDK_SONAME_RE = re.compile(r"libmcp-cpp-sdk\.so(?:\..+)?")


def write_consumer(directory: Path, version_text: str) -> None:
    """Write consumers that call the exported symbol from both installed variants."""

    version = SemVer.parse(version_text, stable_only=True)
    if directory.exists():
        if directory.is_symlink() or not directory.is_dir():
            raise ValidationError("AUR consumer output must be a real directory")
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    (directory / "CMakeLists.txt").write_text(
        f"""cmake_minimum_required(VERSION 3.20)
project(mcp_cpp_sdk_aur_consumer LANGUAGES CXX)
find_package(mcp-cpp-sdk CONFIG REQUIRED COMPONENTS shared static)
add_executable(shared-consumer main.cpp)
target_link_libraries(shared-consumer PRIVATE mcp::sdk_shared)
add_executable(static-consumer main.cpp)
target_link_libraries(static-consumer PRIVATE mcp::sdk_static)
foreach(consumer IN ITEMS shared-consumer static-consumer)
  target_compile_features(${{consumer}} PRIVATE cxx_std_20)
  target_compile_definitions(${{consumer}} PRIVATE MCP_EXPECTED_VERSION=\\\"{version}\\\")
endforeach()
""",
        encoding="utf-8",
        newline="\n",
    )
    (directory / "main.cpp").write_text(
        """#include <mcp/mcp.hpp>

#include <string_view>

#ifndef MCP_EXPECTED_VERSION
#error MCP_EXPECTED_VERSION must be defined
#endif

int main() {
  constexpr std::string_view expected{MCP_EXPECTED_VERSION};
  return mcp::g_VERSION == expected && mcp::version() == expected ? 0 : 1;
}
""",
        encoding="utf-8",
        newline="\n",
    )


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


def _needed_libraries(path: Path) -> tuple[str, ...]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeError as error:
        raise ValidationError("readelf dynamic dependency output is not UTF-8") from error
    dependencies = tuple(_NEEDED_RE.findall(text))
    if not dependencies or len(dependencies) != len(set(dependencies)):
        raise ValidationError("readelf dynamic dependency output is empty or duplicated")
    return dependencies


def validate_elf_dependencies(
    *,
    shared_readelf: Path,
    static_readelf: Path,
    version_text: str,
) -> None:
    """Prove that installed shared/static CMake targets select distinct linkage."""

    version = SemVer.parse(version_text, stable_only=True)
    expected_shared = f"libmcp-cpp-sdk.so.{version.abi_version}"
    shared_sdk = tuple(
        name for name in _needed_libraries(shared_readelf) if _SDK_SONAME_RE.fullmatch(name)
    )
    static_sdk = tuple(
        name for name in _needed_libraries(static_readelf) if _SDK_SONAME_RE.fullmatch(name)
    )
    if shared_sdk != (expected_shared,):
        raise ValidationError("shared AUR consumer does not bind the exact SDK ABI SONAME")
    if static_sdk:
        raise ValidationError("static AUR consumer has an SDK dynamic dependency")


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
