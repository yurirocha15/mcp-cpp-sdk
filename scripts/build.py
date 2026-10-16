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
        ["conan", "profile", "path", "default"],
        capture_output=True,
        check=False
    )
    if result.returncode != 0:
        print("[*] Conan profile not found, detecting...")
        subprocess.run(["conan", "profile", "detect", "--force"], check=True)
        print("[+] Conan profile created")


def conan_install(output_folder, jobs, cppstd, build_type="Release", extra_args=()):
    ensure_conan_profile()
    run(
        "conan", "install", ".",
        f"--output-folder={output_folder}",
        "--build=missing",
        "-s", f"compiler.cppstd={cppstd}",
        "-s", f"build_type={build_type}",
        "-c", "tools.cmake.cmaketoolchain:generator=Ninja",
        "-c", f"tools.build:jobs={jobs}",
        *extra_args,
    )
    generators = Path(output_folder) / "build" / build_type / "generators"
    toolchain = generators / "conan_toolchain.cmake"
    if not toolchain.is_file():
        raise FileNotFoundError(f"Conan did not generate {toolchain}")
    return generators


def compiler_launcher():
    for tool in ("sccache", "ccache"):
        if shutil.which(tool):
            return tool
    return None


def cmake_configure(build_dir, build_type, toolchain, *extra_args):
    ensure_cmake_in_path()
    setup_msvc_env()
    launcher_args = []
    launcher = compiler_launcher()
    if launcher:
        launcher_args = [f"-DCMAKE_C_COMPILER_LAUNCHER={launcher}",
                         f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}"]
    run(
        "cmake", "-B", build_dir,
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain.resolve()}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-G", "Ninja",
        *launcher_args,
        *extra_args,
    )


def no_aslr():
    """Command prefix that disables ASLR, which ThreadSanitizer and MemorySanitizer need on kernels
    with high mmap entropy.

    The test binary also runs at build time (gtest_discover_tests), so the build and ctest both use it.
    """
    setarch = shutil.which("setarch")
    if setarch and sys.platform.startswith("linux"):
        return [setarch, os.uname().machine, "-R"]
    return []


def cmake_build(build_dir, jobs, prefix=()):
    run(*prefix, "cmake", "--build", build_dir, f"-j{jobs}")


# The Clang release the Clang builds look for first, and the LLVM commit (llvmorg-18.1.8) whose
# libc++ the MemorySanitizer build compiles with it. Keep the two on the same major version.
CLANG_VERSION = "18"
MSAN_LLVM_COMMIT = "3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff"
MSAN_LIBCXX_DIR = "build/msan-libcxx"


def find_clang():
    """Return (C compiler, C++ compiler) for Clang, or None if there is no complete pair."""
    for suffix in (f"-{CLANG_VERSION}", ""):
        c_compiler = shutil.which(f"clang{suffix}")
        cxx_compiler = shutil.which(f"clang++{suffix}")
        if c_compiler and cxx_compiler:
            return c_compiler, cxx_compiler
    return None


def build_msan_libcxx(prefix, clang, jobs):
    """Build libc++ and libc++abi instrumented for MemorySanitizer and install them under prefix.

    MemorySanitizer tracks which bytes have been initialised, and only instrumented code tells it.
    Memory a prebuilt standard library writes stays "uninitialised" to it and every later read is
    reported, so the standard library has to be an instrumented build as well.
    """
    prefix = Path(prefix).resolve()
    # Written last, so it also marks a build that ran to the end. A prefix from another commit,
    # such as a stale CI cache, is rebuilt.
    stamp = prefix / "llvm-commit"
    if stamp.is_file() and stamp.read_text().strip() == MSAN_LLVM_COMMIT:
        return prefix

    work = prefix.with_name(prefix.name + "-work")
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(prefix, ignore_errors=True)
    source = work / "llvm-project"
    source.mkdir(parents=True)
    # Only the runtimes and the CMake modules they share with LLVM, at one pinned commit.
    run("git", "init", "-q", str(source))
    run("git", "-C", str(source), "remote", "add", "origin",
        "https://github.com/llvm/llvm-project.git")
    run("git", "-C", str(source), "sparse-checkout", "set",
        "runtimes", "libcxx", "libcxxabi", "llvm/cmake", "llvm/utils/llvm-lit", "cmake",
        "third-party")
    run("git", "-C", str(source), "fetch", "-q", "--depth", "1", "--filter=blob:none",
        "origin", MSAN_LLVM_COMMIT)
    run("git", "-C", str(source), "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD")

    c_compiler, cxx_compiler = clang
    launcher_args = []
    launcher = compiler_launcher()
    if launcher:
        launcher_args = [f"-DCMAKE_C_COMPILER_LAUNCHER={launcher}",
                         f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}"]
    run(
        "cmake", "-S", str(source / "runtimes"), "-B", str(work / "build"), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_C_COMPILER={c_compiler}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi",
        "-DLLVM_USE_SANITIZER=MemoryWithOrigins",
        # Unwind with the system's libgcc. An instrumented libunwind reads registers that its own
        # assembly saved, reports them as uninitialised, and unwinds again to print the report.
        "-DLIBCXXABI_USE_LLVM_UNWINDER=OFF",
        "-DLIBCXX_INCLUDE_TESTS=OFF",
        "-DLIBCXX_INCLUDE_BENCHMARKS=OFF",
        "-DLIBCXXABI_INCLUDE_TESTS=OFF",
        *launcher_args,
    )
    run("cmake", "--build", str(work / "build"), f"-j{jobs}",
        "--target", "install-cxx", "install-cxxabi")
    shutil.rmtree(work, ignore_errors=True)
    stamp.write_text(MSAN_LLVM_COMMIT + "\n")
    return prefix


def msan_conan_args(libcxx, clang):
    """Conan arguments that build the SDK and every dependency against the instrumented libc++.

    The flags reach the dependencies through Conan and the SDK through the toolchain file it
    generates, so CMake needs no option of its own for this mode.
    """
    c_compiler, cxx_compiler = clang
    compile_flags = ["-fsanitize=memory", "-fsanitize-memory-track-origins=2",
                     "-fno-omit-frame-pointer", "-g"]
    # Conan adds -stdlib=libc++ for the libc++ setting. -nostdinc++ then makes the instrumented
    # headers the only ones, which leaves -stdlib with nothing to do when compiling.
    quiet = "-Wno-unused-command-line-argument"
    cxx_flags = compile_flags + ["-nostdinc++", f"-isystem{libcxx}/include/c++/v1", quiet]
    link_flags = ["-fsanitize=memory", "-stdlib=libc++", f"-L{libcxx}/lib",
                  f"-Wl,-rpath,{libcxx}/lib", "-lc++abi", quiet]
    executables = {"c": c_compiler, "cpp": cxx_compiler}
    return [
        "-s", "compiler=clang",
        "-s", f"compiler.version={CLANG_VERSION}",
        "-s", "compiler.libcxx=libc++",
        # MemorySanitizer cannot see what OpenSSL's hand-written assembly initialises.
        "-o", "openssl/*:no_asm=True",
        "-c", f"tools.build:compiler_executables={json.dumps(executables)}",
        "-c", f"tools.build:cflags={json.dumps(compile_flags)}",
        "-c", f"tools.build:cxxflags={json.dumps(cxx_flags)}",
        "-c", f"tools.build:exelinkflags={json.dumps(link_flags)}",
        "-c", f"tools.build:sharedlinkflags={json.dumps(link_flags)}",
    ]


def write_user_presets(generators_dir):
    presets_file = Path(generators_dir) / "CMakePresets.json"
    if presets_file.exists():
        include_path = presets_file.resolve().relative_to(Path.cwd().resolve())
        Path("CMakeUserPresets.json").write_text(
            json.dumps({
                "version": 4,
                "vendor": {"conan": {}},
                "include": [str(include_path)],
            })
        )


EPILOG = """\
examples:
  python scripts/build.py                          release build
  python scripts/build.py --debug                  debug build
  python scripts/build.py --test                   release build + run tests
  python scripts/build.py --debug --test           debug build + run tests
  python scripts/build.py --sanitize --test        ASan/UBSan build + run tests
  python scripts/build.py --tsan --test            ThreadSanitizer build + run tests
  python scripts/build.py --tsan --compiler clang --test   same, compiled with Clang
  python scripts/build.py --msan --test            MemorySanitizer build + run tests
  python scripts/build.py --coverage --test        gcov build + run tests + report
  python scripts/build.py --test                   skip examples, run tests
  python scripts/build.py --sanitize --test        sanitized tests, no examples
  python scripts/build.py --linkage shared --test  build/test only shared SDK
  python scripts/build.py --linkage static --test  build/test only static SDK
  python scripts/build.py --cppstd 23 --test        build/test a C++23 consumer
  python scripts/build.py --conformance             build conformance fixtures
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
    parser.add_argument("--tsan", action="store_true",
                        help="Enable ThreadSanitizer (implies --debug; excludes --sanitize)")
    parser.add_argument("--msan", action="store_true",
                        help="Enable MemorySanitizer (implies --debug and Clang; excludes the "
                             "other sanitizers). Builds an instrumented libc++ on first use and "
                             "rebuilds the Conan dependencies against it")
    parser.add_argument("--compiler", choices=("default", "clang"), default="default",
                        help="Compile the SDK and tests with Clang (Conan dependencies keep "
                             "the detected profile); the build directory gains a -clang suffix")
    parser.add_argument("--coverage", action="store_true",
                        help="Enable gcov coverage (implies --debug)")
    parser.add_argument("--test", action="store_true",
                        help="Build and run tests")
    parser.add_argument("--examples", action="store_true",
                        help="Build example programs")
    parser.add_argument("--conformance", action="store_true",
                        help="Build fixtures for the official MCP conformance runner")
    parser.add_argument(
        "--linkage",
        choices=("both", "shared", "static"),
        default="both",
        help="SDK library variants to build (default: both)",
    )
    parser.add_argument(
        "--cppstd",
        choices=("20", "23"),
        default="20",
        help="C++ language standard used by Conan and CMake (default: 20)",
    )
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

    # ENABLE_SANITIZERS only takes effect inside the CMake tests block, so without --test the
    # flag is accepted and silently does nothing, leaving an unsanitized build in build/sanitize.
    # Refuse here rather than after a Conan install, so no build time is spent on it.
    if sum((args.sanitize, args.tsan, args.msan)) > 1:
        parser.error("--sanitize, --tsan and --msan cannot be combined: each sanitizer needs a "
                     "build of its own")
    if args.msan and args.coverage:
        parser.error("--msan and --coverage cannot be combined")
    if args.tsan and not args.test:
        parser.error(
            "--tsan requires --test: the sanitizer flags are only applied to a build with "
            "tests, so this would produce an unsanitized build in build/tsan"
        )
    if args.sanitize and not args.test:
        parser.error(
            "--sanitize requires --test: the sanitizer flags are only applied to a build with "
            "tests, so this would produce an unsanitized build in build/sanitize"
        )

    is_debug = args.debug or args.sanitize or args.tsan or args.msan or args.coverage
    build_type = "Debug" if is_debug else "Release"

    if args.sanitize:
        build_name = "sanitize"
    elif args.tsan:
        build_name = "tsan"
    elif args.msan:
        build_name = "msan"
    elif args.coverage:
        build_name = "coverage"
    elif is_debug:
        build_name = "debug"
    else:
        build_name = "release"
    if args.compiler == "clang" and not args.msan:
        build_name += "-clang"
    if args.cppstd != "20":
        build_name += f"-cxx{args.cppstd}"
    build_dir = f"build/{build_name}"

    extra_cmake = [
        f"-DBUILD_TESTING={'ON' if args.test else 'OFF'}",
        f"-DBUILD_EXAMPLES={'ON' if args.examples else 'OFF'}",
        f"-DBUILD_DOCS={'ON' if args.docs else 'OFF'}",
        f"-DMCP_CPP_SDK_BUILD_CONFORMANCE={'ON' if args.conformance else 'OFF'}",
        f"-DMCP_CPP_SDK_BUILD_SHARED={'ON' if args.linkage in ('both', 'shared') else 'OFF'}",
        f"-DMCP_CPP_SDK_BUILD_STATIC={'ON' if args.linkage in ('both', 'static') else 'OFF'}",
        f"-DMCP_CPP_SDK_DEFAULT_LINKAGE={'static' if args.linkage == 'static' else 'shared'}",
    ]
    clang = None
    if args.compiler == "clang" or args.msan:
        clang = find_clang()
        if clang is None:
            parser.error(f"this build needs clang and clang++ (or clang-{CLANG_VERSION} and "
                         f"clang++-{CLANG_VERSION}) on PATH")
    conan_args = []
    if args.msan:
        # The Conan toolchain file names the compiler and carries the flags.
        conan_args = msan_conan_args(build_msan_libcxx(MSAN_LIBCXX_DIR, clang, args.jobs), clang)
    elif clang:
        extra_cmake += [f"-DCMAKE_C_COMPILER={clang[0]}", f"-DCMAKE_CXX_COMPILER={clang[1]}"]
    if args.sanitize:
        extra_cmake.append("-DENABLE_SANITIZERS=ON")
    if args.tsan:
        extra_cmake.append("-DENABLE_TSAN=ON")
    if args.coverage:
        extra_cmake.append("-DENABLE_COVERAGE=ON")

    generators_dir = conan_install(
        build_dir, args.jobs, args.cppstd, build_type, conan_args
    )
    cmake_configure(
        build_dir,
        build_type,
        generators_dir / "conan_toolchain.cmake",
        *extra_cmake,
    )
    aslr_prefix = no_aslr() if args.tsan or args.msan else []
    cmake_build(build_dir, args.jobs, aslr_prefix)
    write_user_presets(generators_dir)

    if args.test:
        test_jobs = args.jobs
        extra_env = (
            {"ASAN_OPTIONS": "detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1"
                             ":detect_invalid_pointer_pairs=2",
             "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"}
            if args.sanitize else None
        )
        if args.tsan:
            suppressions = Path("test/tsan.supp").resolve()
            extra_env = {"TSAN_OPTIONS": f"halt_on_error=1:second_deadlock_stack=1:suppressions={suppressions}"}
        ctest_args = []
        if args.msan:
            extra_env = {"MSAN_OPTIONS": "halt_on_error=1"}
            # This test compiles a consumer with the compiler's own standard library, which
            # cannot link against an SDK built on the instrumented libc++.
            ctest_args = ["-E", "^packaging-pkgconfig-static-consumer$"]
        run(*aslr_prefix, "ctest", "--test-dir", build_dir, f"-j{test_jobs}", "--output-on-failure",
            *ctest_args, extra_env=extra_env)

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
        run(
            "sphinx-build", "-W", "--keep-going", "-b", "html",
            "docs", "build/docs/html",
        )


if __name__ == "__main__":
    os.chdir(Path(__file__).parent.parent)
    try:
        main()
    except subprocess.CalledProcessError as e:
        sys.exit(e.returncode)
    except KeyboardInterrupt:
        sys.exit(130)
