"""Build and validate native APT/RPM release packages outside workflow YAML."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from typing import Iterable, Sequence

from .aur_validation import validate_elf_dependencies, write_consumer
from .model import SemVer, ValidationError
from .templates import render_file


_MAINTAINER = "mcp-cpp-sdk maintainers <releases@yurirocha.com>"


def _run(arguments: Sequence[str], *, cwd: Path | None = None) -> str:
    return subprocess.run(
        list(arguments),
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout


def privileged_command(*arguments: str) -> list[str]:
    """Use direct package-manager execution in root containers, sudo otherwise."""

    return list(arguments) if os.geteuid() == 0 else ["sudo", *arguments]


def _reset_directory(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def validate_installed_prefix(prefix: Path, *, workspace: Path, version: SemVer) -> None:
    """Compile and run shared/static consumers against one installed prefix."""

    if workspace.exists():
        shutil.rmtree(workspace)
    source = workspace / "consumer"
    build = workspace / "consumer-build"
    write_consumer(source, str(version))
    _run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-G",
            "Ninja",
            f"-DCMAKE_PREFIX_PATH={prefix}",
        ]
    )
    _run(["cmake", "--build", str(build)])
    shared_readelf = workspace / "shared.readelf"
    static_readelf = workspace / "static.readelf"
    shared_readelf.write_text(
        _run(["readelf", "--dynamic", "--wide", str(build / "shared-consumer")]),
        encoding="utf-8",
    )
    static_readelf.write_text(
        _run(["readelf", "--dynamic", "--wide", str(build / "static-consumer")]),
        encoding="utf-8",
    )
    validate_elf_dependencies(
        shared_readelf=shared_readelf,
        static_readelf=static_readelf,
        version_text=str(version),
    )
    library_paths = []
    if prefix.resolve() != Path("/usr"):
        library_paths = sorted(
            {str(path.parent) for path in prefix.rglob("libmcp-cpp-sdk*.so*") if path.is_file()}
        )
    environment = os.environ.copy()
    if library_paths:
        current = environment.get("LD_LIBRARY_PATH", "")
        environment["LD_LIBRARY_PATH"] = ":".join(library_paths + ([current] if current else []))
    for name in ("shared-consumer", "static-consumer"):
        subprocess.run([str(build / name)], check=True, env=environment)


def _collect_identity(kind: str, target: str, output: Path) -> None:
    _run(
        [
            "python3",
            "-I",
            "-S",
            "scripts/release/collect_build_identity.py",
            "--kind",
            kind,
            "--target",
            target,
            "--output",
            str(output),
        ]
    )


def _write_routes(path: Path, routes: Iterable[dict[str, str]]) -> None:
    path.write_text(
        json.dumps(list(routes), sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _render_debian(*, version: SemVer, release: str, source_date_epoch: int) -> None:
    source = Path("packaging/debian")
    target = Path("debian")
    for template in source.rglob("*.in"):
        relative = template.relative_to(source)
        output = target / Path(str(relative).removesuffix(".in"))
        if template.name == "libmcp-cpp-sdk.install.in":
            output = target / f"libmcp-cpp-sdk{version.abi_version}.install"
        values: dict[str, str] = {}
        if template.name == "control.in":
            values = {"ABI_VERSION": version.abi_version, "MAINTAINER": _MAINTAINER}
        elif template.name == "libmcp-cpp-sdk.install.in":
            values = {"ABI_VERSION": version.abi_version}
        elif template.name == "copyright.in":
            values = {"COPYRIGHT": "mcp-cpp-sdk contributors"}
        elif template.name == "changelog.in":
            values = {
                "DEBIAN_VERSION": version.debian_version,
                "DEBIAN_DISTRIBUTION": release,
                "MAINTAINER": _MAINTAINER,
                "RFC2822_DATE": datetime.fromtimestamp(
                    source_date_epoch, timezone.utc
                ).strftime("%a, %d %b %Y %H:%M:%S +0000"),
            }
        render_file(template, output, values)


def build_apt(args: argparse.Namespace) -> None:
    version = SemVer.parse(args.version, stable_only=True)
    architecture = _run(["dpkg", "--print-architecture"]).strip()
    if architecture != args.architecture:
        raise ValidationError(f"runner architecture {architecture!r} != {args.architecture!r}")
    if list(Path("..").glob("*.deb")):
        raise ValidationError("parent directory contains stale Debian packages")
    _reset_directory(Path("debian"))
    _reset_directory(args.output)
    _collect_identity("apt", args.route_id, args.output / f"build-identity-{args.route_id}.json")
    _render_debian(version=version, release=args.release, source_date_epoch=args.source_date_epoch)
    _run(["dpkg-buildpackage", "--build=binary", "--no-sign"])
    packages = sorted(Path("..").glob("*.deb"))
    if not packages:
        raise ValidationError("Debian build produced no .deb packages")
    routes: list[dict[str, str]] = []
    binary_assets: list[Path] = []
    for package in packages:
        fields = [
            _run(["dpkg-deb", "--field", str(package), field]).strip()
            for field in ("Package", "Version", "Architecture")
        ]
        if any(not field or "\n" in field for field in fields):
            raise ValidationError("Debian package metadata is incomplete")
        asset = args.output / f"{args.route_id}--{package.name}"
        package.replace(asset)
        binary_assets.append(asset)
        routes.append(
            {
                "asset": asset.name,
                "format": "apt",
                "route_id": args.route_id,
                "distribution": args.distribution,
                "release": args.release,
                "target_architecture": args.architecture,
                "package_name": fields[0],
                "package_version": fields[1],
                "package_architecture": fields[2],
                "build_tuple": f"apt-{args.route_id}",
                "identity_asset": f"build-identity-{args.route_id}.json",
            }
        )
    install_root = Path("package-install-root")
    _reset_directory(install_root)
    for package in binary_assets:
        _run(["dpkg-deb", "--extract", str(package), str(install_root)])
    validate_installed_prefix(
        install_root / "usr", workspace=Path("extracted-package-test"), version=version
    )
    _run(
        privileged_command(
            "apt-get",
            "--no-download",
            "--no-install-recommends",
            "install",
            "-y",
            *[str(asset.resolve()) for asset in binary_assets],
        )
    )
    for route in routes:
        installed = _run(
            [
                "dpkg-query",
                "--show",
                "--showformat=${Version} ${Architecture}",
                route["package_name"],
            ]
        ).strip()
        if installed != f"{route['package_version']} {route['package_architecture']}":
            raise ValidationError("installed Debian package identity differs from the build")
    validate_installed_prefix(Path("/usr"), workspace=Path("installed-package-test"), version=version)
    _write_routes(args.output / f"route-{args.route_id}.json", routes)


def _render_rpm(args: argparse.Namespace, version: SemVer) -> None:
    rpm_version, rpm_release = version.rpm_version_release
    archive = args.core / f"mcp-cpp-sdk-{version}.tar.gz"
    if not archive.is_file():
        raise ValidationError(f"canonical source archive is missing: {archive}")
    shutil.copyfile(archive, Path("rpmbuild/SOURCES") / archive.name)
    render_file(
        Path("packaging/rpm/mcp-cpp-sdk.spec.in"),
        Path("rpmbuild/SPECS/mcp-cpp-sdk.spec"),
        {
            "ABI_VERSION": version.abi_version,
            "RPM_VERSION": rpm_version,
            "RPM_RELEASE": rpm_release,
            "RPM_CHANGELOG_DATE": datetime.fromtimestamp(
                args.source_date_epoch, timezone.utc
            ).strftime("%a %b %d %Y"),
            "SOURCE_URL": (
                f"https://github.com/{args.repository}/releases/download/{args.tag}/{archive.name}"
            ),
            "SOURCE_SHA256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        },
    )


def _extract_rpm(package: Path, destination: Path) -> None:
    source = subprocess.Popen(["rpm2cpio", str(package)], stdout=subprocess.PIPE)
    assert source.stdout is not None
    extracted = subprocess.run(
        ["cpio", "--extract", "--make-directories", "--quiet"],
        cwd=destination,
        stdin=source.stdout,
        check=False,
    )
    source.stdout.close()
    source_status = source.wait()
    if source_status or extracted.returncode:
        raise subprocess.CalledProcessError(source_status or extracted.returncode, "rpm2cpio | cpio")


def build_rpm(args: argparse.Namespace) -> None:
    version = SemVer.parse(args.version, stable_only=True)
    architecture = _run(["uname", "-m"]).strip()
    if architecture != args.architecture:
        raise ValidationError(f"runner architecture {architecture!r} != {args.architecture!r}")
    _reset_directory(Path("rpmbuild"))
    for directory in ("BUILD", "BUILDROOT", "RPMS", "SOURCES", "SPECS", "SRPMS"):
        (Path("rpmbuild") / directory).mkdir()
    _reset_directory(args.output)
    _collect_identity("rpm", args.route_id, args.output / f"build-identity-{args.route_id}.json")
    _render_rpm(args, version)
    build_mode = "-ba" if args.architecture == "x86_64" else "-bb"
    _run(
        [
            "rpmbuild",
            "--define",
            f"_topdir {Path.cwd() / 'rpmbuild'}",
            "--define",
            "_enable_debug_packages 0",
            "--define",
            "debug_package %{nil}",
            build_mode,
            "rpmbuild/SPECS/mcp-cpp-sdk.spec",
        ]
    )
    packages = sorted([*Path("rpmbuild/RPMS").rglob("*.rpm"), *Path("rpmbuild/SRPMS").glob("*.rpm")])
    if not packages:
        raise ValidationError("RPM build produced no packages")
    routes: list[dict[str, str]] = []
    binary_assets: list[Path] = []
    for package in packages:
        fields = _run(
            ["rpm", "-qp", "--qf", "%{NAME}\n%{VERSION}-%{RELEASE}\n%{ARCH}\n", str(package)]
        ).splitlines()
        if len(fields) != 3 or any(not field for field in fields):
            raise ValidationError("RPM package metadata is incomplete")
        asset = args.output / f"{args.route_id}--{package.name}"
        shutil.copyfile(package, asset)
        if fields[2] != "src":
            binary_assets.append(asset)
        routes.append(
            {
                "asset": asset.name,
                "format": "rpm",
                "route_id": args.route_id,
                "distribution": args.distribution,
                "release": args.release,
                "target_architecture": args.architecture,
                "package_name": fields[0],
                "package_version": fields[1],
                "package_architecture": fields[2],
                "build_tuple": f"rpm-{args.route_id}",
                "identity_asset": f"build-identity-{args.route_id}.json",
            }
        )
    install_root = Path("package-install-root")
    _reset_directory(install_root)
    for package in binary_assets:
        _extract_rpm(package, install_root)
    validate_installed_prefix(
        install_root / "usr", workspace=Path("extracted-package-test"), version=version
    )
    _run(
        privileged_command(
            "dnf",
            "--disablerepo=*",
            "--setopt=install_weak_deps=False",
            "install",
            "-y",
            *[str(asset.resolve()) for asset in binary_assets],
        )
    )
    for route in routes:
        if route["package_architecture"] == "src":
            continue
        installed = _run(
            ["rpm", "-q", "--qf", "%{VERSION}-%{RELEASE} %{ARCH}", route["package_name"]]
        ).strip()
        if installed != f"{route['package_version']} {route['package_architecture']}":
            raise ValidationError("installed RPM package identity differs from the build")
    validate_installed_prefix(Path("/usr"), workspace=Path("installed-package-test"), version=version)
    _write_routes(args.output / f"route-{args.route_id}.json", routes)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="kind", required=True)
    for kind in ("apt", "rpm"):
        command = subcommands.add_parser(kind)
        command.add_argument("--version", required=True)
        command.add_argument("--route-id", required=True)
        command.add_argument("--distribution", required=True)
        command.add_argument("--release", required=True)
        command.add_argument("--architecture", required=True)
        command.add_argument("--source-date-epoch", type=int, required=True)
        command.add_argument("--output", type=Path, default=Path("out"))
    rpm = subcommands.choices["rpm"]
    rpm.add_argument("--tag", required=True)
    rpm.add_argument("--repository", required=True)
    rpm.add_argument("--core", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.kind == "apt":
            build_apt(args)
        else:
            build_rpm(args)
    except (OSError, subprocess.CalledProcessError, ValidationError) as error:
        raise SystemExit(f"native-build: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
