"""Generate the shared/static CMake consumer used by package validators."""

from __future__ import annotations

from pathlib import Path
import shutil

from .model import SemVer, ValidationError


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
    (directory / "CMakeLists.txt").write_text(
        cmake, encoding="utf-8", newline="\n"
    )
    (directory / "main.cpp").write_text(
        source, encoding="utf-8", newline="\n"
    )
