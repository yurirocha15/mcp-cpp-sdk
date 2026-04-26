#!/usr/bin/env python3
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


def setup_msvc_env():
    if platform.system() != "Windows":
        return
    if shutil.which("cl"):
        return
    vs_paths = [
        Path("C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build"),
        Path("C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build"),
        Path("C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Auxiliary/Build"),
        Path("C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Auxiliary/Build"),
        Path("C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Auxiliary/Build"),
        Path("C:/Program Files (x86)/Microsoft Visual Studio/2019/Professional/VC/Auxiliary/Build"),
        Path("C:/Program Files (x86)/Microsoft Visual Studio/2019/Enterprise/VC/Auxiliary/Build"),
    ]
    for vs_path in vs_paths:
        vcvars = vs_path / "vcvars64.bat"
        if vcvars.exists():
            vcvars_str = str(vcvars).replace('/', '\\')
            cmd = f'"{vcvars_str}" && set'
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                check=False,
                shell=True
            )
            if result.returncode == 0:
                for line in result.stdout.splitlines():
                    if "=" in line:
                        key, value = line.split("=", 1)
                        os.environ[key] = value
                return


def ensure_cmake_in_path():
    if shutil.which("cmake"):
        return
    if platform.system() == "Windows":
        for path in [
            Path("C:/Program Files/CMake/bin"),
            Path("C:/ProgramData/chocolatey/bin"),
        ]:
            if path.exists():
                os.environ["PATH"] = str(path) + os.pathsep + os.environ.get("PATH", "")
                break
    if not shutil.which("cmake"):
        raise FileNotFoundError("cmake not found. Please install cmake and ensure it's in PATH.")


def cpu_half():
    total = os.cpu_count() or 2
    if os.environ.get("CI"):
        return total
    return max(1, total // 2)


def run(*args, extra_env=None, **kwargs):
    env = None
    if extra_env:
        env = os.environ.copy()
        env.update(extra_env)
    subprocess.run(list(args), check=True, env=env, **kwargs)


def ensure_conan_profile():
    result = subprocess.run(
        ["conan", "profile", "show", "default"],
        capture_output=True,
        check=False
    )
    if result.returncode != 0:
        print("[*] Conan profile not found, detecting...")
        subprocess.run(["conan", "profile", "detect", "--force"], check=True)
        print("[+] Conan profile created")


def conan_install(output_folder, jobs, build_type="Release"):
    ensure_conan_profile()
    run(
        "conan", "install", ".",
        f"--output-folder={output_folder}",
        "--build=missing",
        "-s", "compiler.cppstd=20",
        "-s", f"build_type={build_type}",
        "-c", "tools.cmake.cmaketoolchain:generator=Ninja",
        "-c", f"tools.build:jobs={jobs}",
    )


def compiler_launcher():
    for tool in ("sccache", "ccache"):
        if shutil.which(tool):
            return tool
    return None


def cmake_configure(build_dir, build_type, *extra_args):
    ensure_cmake_in_path()
    setup_msvc_env()
    toolchain = f"{build_dir}/conan_toolchain.cmake"
    launcher_args = []
    launcher = compiler_launcher()
    if launcher:
        launcher_args = [f"-DCMAKE_C_COMPILER_LAUNCHER={launcher}",
                         f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}"]
    run(
        "cmake", "-B", build_dir,
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-G", "Ninja",
        *launcher_args,
        *extra_args,
    )


def cmake_build(build_dir, jobs):
    run("cmake", "--build", build_dir, f"-j{jobs}")


def write_user_presets(build_dir):
    presets_file = Path(build_dir) / "CMakePresets.json"
    if presets_file.exists():
        Path("CMakeUserPresets.json").write_text(
            json.dumps({
                "version": 4,
                "vendor": {"conan": {}},
                "include": [str(presets_file)],
            })
        )


EPILOG = """\
examples:
  python scripts/build.py                          release build
  python scripts/build.py --debug                  debug build
  python scripts/build.py --test                   release build + run tests
  python scripts/build.py --debug --test           debug build + run tests
  python scripts/build.py --sanitize --test        ASan/UBSan build + run tests
  python scripts/build.py --coverage --test        gcov build + run tests + report
  python scripts/build.py --no-examples --test     skip examples, run tests
  python scripts/build.py --sanitize --no-examples --test
  python scripts/build.py --docs                   build documentation
  python scripts/build.py --clean                  remove all build artifacts
"""


def main():
    parser = argparse.ArgumentParser(
        description="Build mcp-cpp-sdk",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=EPILOG,
    )
    parser.add_argument("--debug", action="store_true",
                        help="Debug build (default: Release)")
    parser.add_argument("--sanitize", action="store_true",
                        help="Enable ASan + UBSan (implies --debug)")
    parser.add_argument("--coverage", action="store_true",
                        help="Enable gcov coverage (implies --debug)")
    parser.add_argument("--test", action="store_true",
                        help="Build and run tests")
    parser.add_argument("--examples", action="store_true",
                        help="Build example programs")
    parser.add_argument("--docs", action="store_true",
                        help="Build documentation")
    parser.add_argument("--clean", action="store_true",
                        help="Remove all build artifacts and exit")
    parser.add_argument("--jobs", type=int, default=cpu_half(), metavar="N",
                        help=f"Parallel build jobs (default: {cpu_half()})")

    args = parser.parse_args()

    if args.clean:
        shutil.rmtree("build", ignore_errors=True)
        print("[+] Build directory removed")
        return

    is_debug = args.debug or args.sanitize or args.coverage
    build_type = "Debug" if is_debug else "Release"

    if args.sanitize:
        build_dir = "build/sanitize"
    elif args.coverage:
        build_dir = "build/coverage"
    elif is_debug:
        build_dir = "build/debug"
    else:
        build_dir = "build/release"

    extra_cmake = [
        f"-DBUILD_TESTING={'ON' if args.test else 'OFF'}",
        f"-DBUILD_EXAMPLES={'ON' if args.examples else 'OFF'}",
    ]
    if args.sanitize:
        extra_cmake.append("-DENABLE_SANITIZERS=ON")
    if args.coverage:
        extra_cmake.append("-DENABLE_COVERAGE=ON")

    conan_install(build_dir, args.jobs, build_type)
    cmake_configure(build_dir, build_type, *extra_cmake)
    cmake_build(build_dir, args.jobs)
    write_user_presets(build_dir)

    if args.test:
        test_jobs = args.jobs
        extra_env = (
            {"ASAN_OPTIONS": "detect_leaks=0",
             "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"}
            if args.sanitize else None
        )
        run("ctest", "--test-dir", build_dir, f"-j{test_jobs}", "--output-on-failure",
            extra_env=extra_env)

        if args.coverage:
            run("gcovr", "-r", ".", "--html", "--html-details",
                "-o", f"{build_dir}/coverage.html", "-f", "include/")
            run("gcovr", "-r", ".", "-f", "include/")

    if args.docs:
        try:
            run("cmake", "--build", build_dir, "--target", "docs", f"-j{args.jobs}")
            print("[+] Doxygen XML generated")
        except subprocess.CalledProcessError:
            print("[!] Doxygen not available, building Sphinx docs without API reference")
        run("sphinx-build", "-b", "html", "docs", "build/docs/html")


if __name__ == "__main__":
    os.chdir(Path(__file__).parent.parent)
    try:
        main()
    except subprocess.CalledProcessError as e:
        sys.exit(e.returncode)
    except KeyboardInterrupt:
        sys.exit(130)
