"""Prepare and validate pre-anchor Homebrew and Chocolatey package tests.

The public package metadata must keep its immutable GitHub Release URL.  Before
that release exists, Homebrew consumes the byte-identical archive through its
URL-derived cache path.  Chocolatey cannot do that, so a temporary package copy
has only its archive URL redirected to a loopback-only server.  This module
makes those checks explicit and fail-closed instead of hiding them in workflow
text.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import platform
import re
import subprocess
from typing import Sequence
from urllib.parse import urlsplit

from .model import SemVer, ValidationError


_CHOCO_VERSION = re.compile(r"(?m)^\$version = '(?P<version>[^']+)'$")
_CHOCO_URL = re.compile(r"(?m)^    -Url64bit '(?P<url>[^']+)' ")
_CHOCO_SHA = re.compile(r"-Checksum64 '(?P<sha>[0-9a-f]{64})' ")
_REPOSITORY = "yurirocha15/mcp-cpp-sdk"


def _regular_bytes(path: Path, label: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValidationError(f"{label} must be a regular file")
    return path.read_bytes()


def _utf8(path: Path, label: str) -> str:
    try:
        return _regular_bytes(path, label).decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValidationError(f"{label} must be UTF-8") from error


def _one_match(pattern: re.Pattern[str], text: str, label: str) -> re.Match[str]:
    matches = tuple(pattern.finditer(text))
    if len(matches) != 1:
        raise ValidationError(f"{label} must occur exactly once")
    return matches[0]


def _loopback_url(value: str) -> str:
    parsed = urlsplit(value)
    if (
        parsed.scheme != "http"
        or parsed.hostname != "127.0.0.1"
        or not parsed.path
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise ValidationError("candidate archive URL must use credential-free loopback HTTP")
    return value


def _write_single_field_adaptation(
    *, production: str, old: str, new: str, output: Path, label: str
) -> None:
    if old == new or production.count(old) != 1 or new in production:
        raise ValidationError(f"{label} URL adaptation is not an exact one-field replacement")
    adapted = production.replace(old, new, 1)
    if adapted.count(new) != 1 or adapted.replace(new, old, 1) != production:
        raise ValidationError(f"{label} adaptation changed more than the intended URL field")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(adapted, encoding="utf-8", newline="\n")


def adapt_chocolatey_install(
    *, script: Path, archive: Path, local_url: str, output: Path
) -> None:
    production = _utf8(script, "Chocolatey install script")
    version = SemVer.parse(
        _one_match(_CHOCO_VERSION, production, "Chocolatey version").group("version"),
        stable_only=True,
    )
    url = _one_match(_CHOCO_URL, production, "Chocolatey archive URL").group("url")
    digest = _one_match(_CHOCO_SHA, production, "Chocolatey archive digest").group("sha")
    archive_name = f"mcp-cpp-sdk-{version}-windows-x64-v143-md.zip"
    expected_url = (
        f"https://github.com/{_REPOSITORY}/releases/download/"
        f"v{version}/{archive_name}"
    )
    if url != expected_url or archive.name != archive_name:
        raise ValidationError("Chocolatey package has an unexpected production archive identity")
    if hashlib.sha256(_regular_bytes(archive, "Chocolatey candidate archive")).hexdigest() != digest:
        raise ValidationError("Chocolatey candidate archive differs from the install-script digest")
    replacement = f"    -Url64bit '{_loopback_url(local_url)}' "
    old = f"    -Url64bit '{url}' "
    _write_single_field_adaptation(
        production=production,
        old=old,
        new=replacement,
        output=output,
        label="Chocolatey install script",
    )


def _tree_inventory(root: Path) -> dict[str, str]:
    if root.is_symlink() or not root.is_dir():
        raise ValidationError("package tree must be a real directory")
    inventory: dict[str, str] = {}
    folded_names: set[str] = set()
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValidationError("package tree must not contain symbolic links")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            folded = relative.casefold()
            if folded in folded_names:
                raise ValidationError("package tree contains case-colliding file names")
            folded_names.add(folded)
            inventory[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
        elif not path.is_dir():
            raise ValidationError("package tree contains a non-file entry")
    if not inventory:
        raise ValidationError("package tree is empty")
    return inventory


def compare_package_trees(expected: Path, actual: Path) -> None:
    if _tree_inventory(expected) != _tree_inventory(actual):
        raise ValidationError("installed package tree differs from the staged archive tree")


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


def _consumer_sources(version: SemVer) -> tuple[str, str]:
    cmake = """cmake_minimum_required(VERSION 3.20)
project(mcp_cpp_sdk_package_consumer LANGUAGES CXX)
find_package(mcp-cpp-sdk CONFIG REQUIRED COMPONENTS shared static)
add_executable(shared-consumer main.cpp)
target_link_libraries(shared-consumer PRIVATE mcp::sdk_shared)
add_executable(static-consumer main.cpp)
target_link_libraries(static-consumer PRIVATE mcp::sdk_static)
"""
    source = f"""#include <mcp/mcp.hpp>
#include <string_view>

int main() {{
  constexpr std::string_view expected{{\"{version}\"}};
  return mcp::g_VERSION == expected && mcp::version() == expected ? 0 : 1;
}}
"""
    return cmake, source


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
    cmake, source = _consumer_sources(version)
    work.mkdir(parents=True)
    (work / "CMakeLists.txt").write_text(cmake, encoding="utf-8", newline="\n")
    (work / "main.cpp").write_text(source, encoding="utf-8", newline="\n")
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
    chocolatey = commands.add_parser("chocolatey-adapt")
    chocolatey.add_argument("--metadata", type=Path, required=True)
    chocolatey.add_argument("--archive", type=Path, required=True)
    chocolatey.add_argument("--local-url", required=True)
    chocolatey.add_argument("--output", type=Path, required=True)
    compare = commands.add_parser("compare-trees")
    compare.add_argument("--expected", type=Path, required=True)
    compare.add_argument("--actual", type=Path, required=True)
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
        if args.command == "chocolatey-adapt":
            adapt_chocolatey_install(
                script=args.metadata,
                archive=args.archive,
                local_url=args.local_url,
                output=args.output,
            )
        elif args.command == "compare-trees":
            compare_package_trees(args.expected, args.actual)
        elif args.command == "compare-files":
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
