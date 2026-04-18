#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def cpu_half():
    total = os.cpu_count() or 2
    return max(1, total // 2)


def run(*args, extra_env=None, **kwargs):
    env = None
    if extra_env:
        env = os.environ.copy()
        env.update(extra_env)
    subprocess.run(list(args), check=True, env=env, **kwargs)


def write_user_presets():
    Path("CMakeUserPresets.json").write_text(
        json.dumps({"version": 4, "vendor": {"conan": {}}, "include": ["build/CMakePresets.json"]})
    )


def cmd_build():
    run("conan", "install", ".", "--output-folder=build", "--build=missing",
        "-s", "compiler.cppstd=20", "-c", "tools.cmake.cmaketoolchain:generator=Ninja")
    write_user_presets()
    run("cmake", "--preset", "conan-release")
    run("cmake", "--build", "--preset", "conan-release", f"-j{cpu_half()}")


def cmd_debug():
    run("conan", "install", ".", "--output-folder=build/debug", "--build=missing",
        "-s", "compiler.cppstd=20", "-s", "build_type=Debug",
        "-c", "tools.cmake.cmaketoolchain:generator=Ninja")
    run("cmake", "-B", "build/debug",
        "-DCMAKE_TOOLCHAIN_FILE=build/debug/conan_toolchain.cmake",
        "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-G", "Ninja")
    run("cmake", "--build", "build/debug", f"-j{cpu_half()}")
    write_user_presets()


def cmd_test():
    run("ctest", "--preset", "conan-release", f"-j{cpu_half()}")


def cmd_sanitize():
    run("conan", "install", ".", "--output-folder=build/sanitize", "--build=missing",
        "-s", "compiler.cppstd=20", "-s", "build_type=Debug",
        "-c", "tools.cmake.cmaketoolchain:generator=Ninja")
    run("cmake", "-B", "build/sanitize",
        "-DCMAKE_TOOLCHAIN_FILE=build/sanitize/conan_toolchain.cmake",
        "-DCMAKE_BUILD_TYPE=Debug", "-DENABLE_SANITIZERS=ON", "-G", "Ninja")
    run("cmake", "--build", "build/sanitize", f"-j{cpu_half()}")
    write_user_presets()
    run("ctest", "--test-dir", "build/sanitize", "-j1", "--output-on-failure",
        extra_env={"ASAN_OPTIONS": "detect_leaks=0",
                   "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"})


def cmd_coverage():
    run("conan", "install", ".", "--output-folder=build/coverage", "--build=missing",
        "-s", "compiler.cppstd=20", "-s", "build_type=Debug",
        "-c", "tools.cmake.cmaketoolchain:generator=Ninja")
    run("cmake", "--preset", "conan-debug",
        "-DENABLE_COVERAGE=ON", "-DCMAKE_BUILD_TYPE=Debug", "-B", "build/coverage")
    run("cmake", "--build", "build/coverage", f"-j{cpu_half()}")
    run("ctest", "--test-dir", "build/coverage", f"-j{cpu_half()}", "--output-on-failure")
    run("gcovr", "-r", ".", "--html", "--html-details",
        "-o", "build/coverage/coverage.html", "-f", "include/")
    run("gcovr", "-r", ".", "-f", "include/")


def cmd_docs():
    run("cmake", "--preset", "conan-release")
    try:
        run("cmake", "--build", "--preset", "conan-release", "--target", "docs",
            f"-j{cpu_half()}")
        print("[+] Doxygen XML generated")
    except subprocess.CalledProcessError:
        print("[!] Doxygen not available, building Sphinx docs without API reference")
    run("sphinx-build", "-b", "html", "docs", "build/docs/html")


def cmd_clean():
    shutil.rmtree("build", ignore_errors=True)


COMMANDS = {
    "build":    cmd_build,
    "debug":    cmd_debug,
    "test":     cmd_test,
    "sanitize": cmd_sanitize,
    "coverage": cmd_coverage,
    "docs":     cmd_docs,
    "clean":    cmd_clean,
}


def usage():
    print("Usage: python3 scripts/build.py <command>\n")
    print("Commands:")
    width = max(len(k) for k in COMMANDS)
    descriptions = {
        "build":    "Release build with optimizations",
        "debug":    "Debug build with symbols",
        "test":     "Run tests (requires release build)",
        "sanitize": "Debug build with ASan + UBSan, then run tests",
        "coverage": "Debug build with gcov, run tests, generate HTML report",
        "docs":     "Generate documentation (requires release build)",
        "clean":    "Remove all build artifacts",
    }
    for cmd, desc in descriptions.items():
        print(f"  {cmd:<{width}}  {desc}")
    sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in COMMANDS:
        usage()
    os.chdir(Path(__file__).parent.parent)
    try:
        COMMANDS[sys.argv[1]]()
    except subprocess.CalledProcessError as e:
        sys.exit(e.returncode)
    except KeyboardInterrupt:
        sys.exit(130)
