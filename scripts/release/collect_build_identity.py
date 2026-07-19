#!/usr/bin/env python3
"""Collect a minimal technical builder identity and validate it fail closed."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from release.build_identity import (  # noqa: E402
    BuildIdentityError,
    validate_apt_build_identity,
    validate_aur_build_identity,
    validate_rpm_build_identity,
    validate_windows_build_identity,
)
from release.native_builder import bind_container_identity  # noqa: E402
from release.model import ValidationError  # noqa: E402


class CollectionError(ValueError):
    """Raised when host identity evidence cannot be collected exactly."""


def parse_os_release(path: Path = Path("/etc/os-release")) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if not raw_line or raw_line.startswith("#"):
            continue
        key, separator, raw_value = raw_line.partition("=")
        if not separator or key in values or not key.replace("_", "").isalnum():
            raise CollectionError("/etc/os-release is malformed or contains duplicate fields")
        parsed = shlex.split(raw_value, posix=True)
        if len(parsed) > 1:
            raise CollectionError("/etc/os-release contains a malformed value")
        values[key] = parsed[0] if parsed else ""
    return values


def command(*arguments: str) -> str:
    return subprocess.run(
        list(arguments), check=True, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=30,
    ).stdout.strip()


def collect(kind: str, target: str) -> dict[str, object]:
    os_release = parse_os_release()
    if kind == "apt":
        identity = validate_apt_build_identity(
            target,
            {
                "os_id": os_release.get("ID", ""),
                "os_version_id": os_release.get("VERSION_ID", ""),
                "os_version_codename": os_release.get("VERSION_CODENAME", ""),
                "dpkg_architecture": command("dpkg", "--print-architecture"),
                "uname_machine": command("uname", "-m"),
            },
        )
    elif kind == "rpm":
        identity = validate_rpm_build_identity(
            target,
            {
                "os_id": os_release.get("ID", ""),
                "os_version_id": os_release.get("VERSION_ID", ""),
                "rpm_fedora": command("rpm", "--eval", "%{?fedora}"),
                "rpm_rhel": command("rpm", "--eval", "%{?rhel}"),
                "rpm_dist": command("rpm", "--eval", "%{?dist}"),
                "rpm_architecture": command("rpm", "--eval", "%{_arch}"),
                "uname_machine": command("uname", "-m"),
            },
        )
    elif kind == "aur":
        identity = validate_aur_build_identity(
            target,
            {
                "os_id": os_release.get("ID", ""),
                "arch_release_present": str(Path("/etc/arch-release").is_file()).lower(),
                "uname_machine": command("uname", "-m"),
            },
        )
    else:
        raise CollectionError("unsupported builder identity kind")
    return bind_container_identity(
        identity,
        image=os.environ.get("MCP_RELEASE_BUILDER_IMAGE", ""),
        image_id=os.environ.get("MCP_RELEASE_BUILDER_IMAGE_ID", ""),
    )


def collect_windows(facts_path: Path, abi_version: str) -> dict[str, object]:
    facts = json.loads(facts_path.read_text(encoding="utf-8"))
    return validate_windows_build_identity(facts, expected_abi_version=abi_version)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=("apt", "rpm", "aur", "windows"), required=True)
    parser.add_argument("--target")
    parser.add_argument("--facts", type=Path)
    parser.add_argument("--abi-version")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.kind == "windows":
            if args.target is not None or args.facts is None or args.abi_version is None:
                raise CollectionError("Windows identity requires --facts and --abi-version only")
            identity = collect_windows(args.facts, args.abi_version)
        else:
            if args.target is None or args.facts is not None or args.abi_version is not None:
                raise CollectionError("native identity requires --target only")
            identity = collect(args.kind, args.target)
        args.output.write_text(
            json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
            encoding="ascii",
        )
    except (
        BuildIdentityError, CollectionError, json.JSONDecodeError, OSError, UnicodeError, ValidationError,
        subprocess.SubprocessError,
    ) as error:
        print(f"collect-build-identity: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
