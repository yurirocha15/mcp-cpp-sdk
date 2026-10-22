#!/usr/bin/env python3
"""
Build and test the SDK inside a locked release builder image.

Each image is one target distribution with its own compiler, Boost and Python. The CMake
options are the package builds' (see packaging/debian/rules.in), but not the packaging's own
compiler flags: scripts/package_build.py builds the packages themselves. The release tooling's
tests run with the image's Python as well, since the package builds run that tooling there.

  python3 scripts/distro_build.py ubuntu-jammy-amd64
  python3 scripts/distro_build.py --matrix    # one x86_64 image per distribution, as JSON
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "release/native-builders/lock.json"

CMAKE_OPTIONS = [
    "-DMCP_CPP_SDK_BUILD_SHARED=ON",
    "-DMCP_CPP_SDK_BUILD_STATIC=ON",
    "-DBUILD_TESTING=ON",
    "-DMCP_CPP_SDK_CHECK_JSON_MATRIX=OFF",
    "-DBUILD_EXAMPLES=OFF",
    "-DBUILD_DOCS=OFF",
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
]


def load_targets():
    return json.loads(LOCK.read_text(encoding="utf-8"))["targets"]


def matrix(targets):
    # The Conan image builds with Conan's Boost, which the Conan CI jobs already cover.
    return [
        {"id": target["id"], "runner": target["runner"]}
        for target in targets
        if target["architecture"] == "x86_64" and target["kind"] != "conan"
    ]


def container_script(jobs):
    configure = " ".join(["cmake -S /source -B /out -G Ninja", *CMAKE_OPTIONS])
    return "\n".join([
        "set -e",
        configure,
        f"cmake --build /out -j {jobs}",
        f"ctest --test-dir /out -j {jobs} --timeout 300",
        "cd /source",
        "PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s test/release -p 'test_*.py'",
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", nargs="?", help="target id from release/native-builders/lock.json")
    parser.add_argument("--matrix", action="store_true",
                        help="print the CI matrix as JSON and exit")
    parser.add_argument("--jobs", type=int, default=4,
                        help="parallel compile and test jobs (default: 4)")
    parser.add_argument("--build-dir", type=Path,
                        help="build directory (default: build/distro/<target>)")
    parser.add_argument("--docker-arg", action="append", default=[],
                        help="extra `docker run` argument, e.g. --docker-arg=--memory=12g")
    args = parser.parse_args()

    targets = load_targets()
    if args.matrix:
        print(json.dumps(matrix(targets), separators=(",", ":")))
        return 0
    if not args.target:
        parser.error("a target id is required")
    by_id = {target["id"]: target for target in targets}
    if args.target not in by_id:
        parser.error(f"unknown target {args.target}; known: {', '.join(by_id)}")
    target = by_id[args.target]

    build_dir = (args.build_dir or ROOT / "build/distro" / args.target).resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    command = [
        "docker", "run", "--rm",
        "--user", f"{os.getuid()}:{os.getgid()}",
        "-e", "HOME=/tmp",
        "-e", "CTEST_OUTPUT_ON_FAILURE=1",
        "--mount", f"type=bind,src={ROOT},dst=/source,readonly",
        "--mount", f"type=bind,src={build_dir},dst=/out",
        "--tmpfs", "/tmp:exec",
        *args.docker_arg,
        f"{target['image']}@{target['image_digest']}",
        "sh", "-c", container_script(args.jobs),
    ]
    print(f"== {args.target}: {target['base_image']} -> {build_dir}", flush=True)
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
