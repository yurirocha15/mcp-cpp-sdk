#!/usr/bin/env python3
"""
Build and test the SDK's native packages the way a release does, without signing or publishing.

  python3 scripts/package_build.py --matrix
  python3 scripts/package_build.py core --output core
  python3 scripts/package_build.py build debian-bookworm-amd64 --core core --output out

`core` runs release.construct_core for HEAD. The provider repository ids and Cloudsmith key
fingerprint it is given are placeholders: they only fill the publication contract, which a
release constructs from live provider state and which no package build reads.

`build` runs release.native_builder for one route in its locked builder image, as the release
does: the package build with its own tests, then a consumer built against the extracted
packages and another against the installed ones.

`--matrix` lists the x86_64 apt and rpm routes. The AUR build is not covered: it checks the
release's signature on the source archive, which only a release has.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = [sys.executable, "-I", "-S", str(ROOT / "scripts/run_release_tool.py")]
PLACEHOLDER_COMMIT = "0" * 40
PLACEHOLDER_REPOSITORY_ID = "1"


def git(*arguments):
    return subprocess.run(["git", *arguments], cwd=ROOT, check=True, text=True,
                          stdout=subprocess.PIPE).stdout.strip()


def load_json(relative):
    return json.loads((ROOT / relative).read_text(encoding="utf-8"))


def native_routes():
    """The routes in packaging/targets.json, keyed by route id."""
    catalog = load_json("packaging/targets.json")
    routes = {}
    for kind in ("apt", "rpm"):
        for group in catalog[kind]:
            for arch in group["architectures"]:
                architecture = arch["architecture"]
                route_id = f"{group['distribution']}-{group['release']}-{architecture}"
                routes[route_id] = {"kind": kind, "distribution": group["distribution"],
                                    "release": group["release"], "architecture": architecture}
    return routes


def matrix():
    builders = {t["id"]: t for t in load_json("release/native-builders/lock.json")["targets"]}
    return [
        {"id": route_id, "runner": builders[route_id]["runner"]}
        for route_id, route in native_routes().items()
        if route["architecture"] in ("amd64", "x86_64")
    ]


def package_version():
    """VERSION without a prerelease suffix: native packages exist only for stable releases."""
    return (ROOT / "VERSION").read_text(encoding="utf-8").strip().split("-", 1)[0]


def construct(output):
    version = package_version()
    signers = load_json("release/trusted-release-signers.json")
    signer = next(s for s in signers["signers"] if s["id"] == signers["active_signer_id"])
    command = [
        *TOOL, "release.construct_core", "--root", str(ROOT), "--output", str(output),
        "--version", version, "--tag", f"v{version}", "--commit", git("rev-parse", "HEAD"),
        "--source-date-epoch", git("log", "-1", "--format=%ct"),
        "--release-kind", "stable",
        "--repository", "yurirocha15/mcp-cpp-sdk",
        "--primary-fingerprint", signer["primary_fingerprint"],
        "--artifact-fingerprint", signer["artifact_subkey_fingerprint"],
        "--cloudsmith-namespace", "mcp-cpp-sdk", "--cloudsmith-repository", "mcp-cpp-sdk",
        "--cloudsmith-repository-gpg-fingerprint", PLACEHOLDER_COMMIT,
    ]
    for prefix, repository in (
        ("homebrew", "yurirocha15/homebrew-mcp-cpp-sdk"),
        ("chocolatey", "yurirocha15/mcp-cpp-sdk-chocolatey-publisher"),
        ("conan-control", "yurirocha15/mcp-cpp-sdk-release-control"),
    ):
        command += [f"--{prefix}-repository", repository,
                    f"--{prefix}-repository-id", PLACEHOLDER_REPOSITORY_ID]
    return subprocess.run(command, cwd=ROOT, check=False).returncode


def build(route_id, core, output):
    routes = native_routes()
    if route_id not in routes:
        sys.exit(f"unknown route {route_id}; known: {', '.join(routes)}")
    route = routes[route_id]
    builder = next(t for t in load_json("release/native-builders/lock.json")["targets"]
                   if t["id"] == route_id)
    metadata = json.loads((core / "core-metadata.json").read_text(encoding="utf-8"))
    version = package_version()
    command = [
        *TOOL, "release.native_builder", "run",
        "--lock", "release/native-builders/lock.json",
        "--image", f"{builder['image']}@{builder['image_digest']}",
        "--kind", route["kind"], "--version", version, "--tag", f"v{version}",
        "--repository", "yurirocha15/mcp-cpp-sdk", "--target-id", route_id,
        "--distribution", route["distribution"], "--release", route["release"],
        "--architecture", route["architecture"],
        "--source-date-epoch", str(metadata["source_date_epoch"]),
        "--core", str(core), "--output", str(output),
    ]
    return subprocess.run(command, cwd=ROOT, check=False).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--matrix", action="store_true", help="print the CI matrix as JSON")
    commands = parser.add_subparsers(dest="command")
    core_parser = commands.add_parser("core", help="construct the release core for HEAD")
    core_parser.add_argument("--output", type=Path, required=True)
    build_parser = commands.add_parser("build", help="build and test one route's packages")
    build_parser.add_argument("route", help="route id, e.g. debian-bookworm-amd64")
    build_parser.add_argument("--core", type=Path, required=True)
    build_parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.matrix:
        print(json.dumps(matrix(), separators=(",", ":")))
        return 0
    if args.command == "core":
        return construct(args.output.resolve())
    if args.command == "build":
        return build(args.route, args.core.resolve(), args.output.resolve())
    parser.error("a command or --matrix is required")
    return 2


if __name__ == "__main__":
    sys.exit(main())
