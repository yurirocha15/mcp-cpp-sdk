#!/usr/bin/env python3
"""Build exact shared/static consumers and verify their SDK linkage."""

from __future__ import annotations

import argparse
from pathlib import Path
import platform
import re
import subprocess
import sys


VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


def sources(version: str) -> tuple[str, str]:
    if VERSION.fullmatch(version) is None:
        raise ValueError("consumer version is not canonical stable SemVer")
    cmake = """cmake_minimum_required(VERSION 3.20)
project(mcp_cpp_sdk_bottle_consumer LANGUAGES CXX)
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


def abi_identity(version: str) -> str:
    match = VERSION.fullmatch(version)
    if match is None:
        raise ValueError("consumer version is not canonical stable SemVer")
    return version if match.group(1) == "0" else match.group(1)


def verify_linkage(shared: str, static: str, *, system: str, version: str) -> None:
    library = r"libmcp-cpp-sdk(?:(?:\.[0-9.]+)?\.dylib|\.so(?:\.[0-9.]+)?)"
    abi = abi_identity(version)
    expected = (
        f"libmcp-cpp-sdk.{abi}.dylib"
        if system == "Darwin"
        else f"libmcp-cpp-sdk.so.{abi}"
    )
    shared_dependencies = set(re.findall(library, shared))
    if shared_dependencies != {expected}:
        raise ValueError(f"shared consumer has the wrong SDK dynamic dependency on {system}")
    if re.search(library, static) is not None:
        raise ValueError(f"static consumer unexpectedly has an SDK dynamic dependency on {system}")


def _run(arguments: list[str], *, timeout: int = 600) -> str:
    return subprocess.run(
        arguments,
        check=True,
        capture_output=True,
        text=True,
        timeout=timeout,
    ).stdout


def run(*, version: str, prefix: Path, work: Path, system: str | None = None) -> None:
    system = system or platform.system()
    if system not in {"Darwin", "Linux"}:
        raise ValueError("consumer linkage test supports only macOS and Linux")
    if work.exists():
        raise ValueError("consumer work directory must be new")
    cmake, source = sources(version)
    work.mkdir(parents=True)
    (work / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    (work / "main.cpp").write_text(source, encoding="utf-8")
    build = work / "build"
    _run(["cmake", "-S", str(work), "-B", str(build), f"-DCMAKE_PREFIX_PATH={prefix}"])
    _run(["cmake", "--build", str(build), "--config", "Release"])
    shared = build / "shared-consumer"
    static = build / "static-consumer"
    _run([str(shared)])
    _run([str(static)])
    inspector = "otool" if system == "Darwin" else "ldd"
    shared_linkage = _run([inspector, "-L", str(shared)] if system == "Darwin" else [inspector, str(shared)])
    static_linkage = _run([inspector, "-L", str(static)] if system == "Darwin" else [inspector, str(static)])
    verify_linkage(shared_linkage, static_linkage, system=system, version=version)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    try:
        run(version=args.version, prefix=args.prefix, work=args.work)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as error:
        print(f"consumer-test: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
