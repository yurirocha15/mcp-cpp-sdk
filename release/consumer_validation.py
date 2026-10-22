"""Generate and check the shared/static CMake consumer used by package validators."""

from __future__ import annotations

from pathlib import Path
import re
import shutil

from .model import SemVer, ValidationError


_NEEDED_RE = re.compile(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]")
_SDK_SONAME_RE = re.compile(r"libmcp-cpp-sdk\.so(?:\..+)?")


def consumer_sources(version: SemVer) -> tuple[str, str]:
    """Return one consumer project that exercises both installed linkages."""

    cmake = """cmake_minimum_required(VERSION 3.20)
project(mcp_cpp_sdk_package_consumer LANGUAGES CXX)
find_package(mcp-cpp-sdk CONFIG REQUIRED COMPONENTS shared static)
add_executable(shared-consumer main.cpp)
target_link_libraries(shared-consumer PRIVATE mcp::sdk_shared)
add_executable(static-consumer main.cpp)
target_link_libraries(static-consumer PRIVATE mcp::sdk_static)
foreach(consumer IN ITEMS shared-consumer static-consumer)
  target_compile_features(${consumer} PRIVATE cxx_std_20)
endforeach()
"""
    source = f"""#include <mcp/mcp.hpp>
#include <string_view>

int main() {{
  constexpr std::string_view expected{{\"{version}\"}};
  return mcp::g_VERSION == expected && mcp::version() == expected ? 0 : 1;
}}
"""
    return cmake, source


def write_consumer_project(
    directory: Path, version_text: str, *, replace: bool = False
) -> None:
    """Write the canonical consumer project to a new or replaceable directory."""

    version = SemVer.parse(version_text, stable_only=True)
    if directory.exists() or directory.is_symlink():
        if not replace or directory.is_symlink() or not directory.is_dir():
            raise ValidationError("consumer output directory is not safely replaceable")
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    cmake, source = consumer_sources(version)
    (directory / "CMakeLists.txt").write_bytes(cmake.encode("utf-8"))
    (directory / "main.cpp").write_bytes(source.encode("utf-8"))


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
        raise ValidationError("shared package consumer does not bind the exact SDK ABI SONAME")
    if static_sdk:
        raise ValidationError("static package consumer has an SDK dynamic dependency")
