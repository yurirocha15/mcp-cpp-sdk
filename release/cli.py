"""Offline command-line entry point for release construction."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Sequence

from .artifacts import (
    SourceInventory,
    build_release_manifest,
    build_sboms,
    build_sha256sums,
    build_source_archives,
    canonical_json_bytes,
    write_atomic,
)
from .model import DispatchRequest, SemVer, ValidationError
from .templates import render_file


def _json_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValidationError(f"{path} must contain a JSON object")
    return value


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Construct and validate mcp-cpp-sdk release data offline")
    subcommands = parser.add_subparsers(dest="command", required=True)

    version = subcommands.add_parser("validate-version")
    version.add_argument("value")
    version.add_argument("--tag", action="store_true")
    version.add_argument("--stable-only", action="store_true")

    dispatch = subcommands.add_parser("validate-dispatch")
    dispatch.add_argument("request", type=Path)

    archives = subcommands.add_parser("source-archives")
    archives.add_argument("--root", type=Path, required=True)
    archives.add_argument("--inventory", type=Path, required=True)
    archives.add_argument("--version", required=True)
    archives.add_argument("--output-dir", type=Path, required=True)
    archives.add_argument("--source-date-epoch", type=int, required=True)

    checksums = subcommands.add_parser("checksums")
    checksums.add_argument("--output", type=Path, required=True)
    checksums.add_argument("assets", type=Path, nargs="+")

    sbom = subcommands.add_parser("sbom")
    sbom.add_argument("--root", type=Path, required=True)
    sbom.add_argument("--inventory", type=Path, required=True)
    sbom.add_argument("--version", required=True)
    sbom.add_argument("--output-dir", type=Path, required=True)
    sbom.add_argument("--source-date-epoch", type=int, required=True)
    sbom.add_argument("--namespace-base", required=True)

    manifest = subcommands.add_parser("manifest")
    manifest.add_argument("--spec", type=Path, required=True)
    manifest.add_argument("--output", type=Path, required=True)

    render = subcommands.add_parser("render")
    render.add_argument("--template", type=Path, required=True)
    render.add_argument("--values", type=Path, required=True)
    render.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "validate-version":
            version = SemVer.from_tag(args.value, stable_only=args.stable_only) if args.tag else SemVer.parse(args.value, stable_only=args.stable_only)
            print(version)
        elif args.command == "validate-dispatch":
            request = DispatchRequest.from_mapping(_json_object(args.request))
            print(json.dumps(request.__dict__, sort_keys=True))
        elif args.command == "source-archives":
            paths = build_source_archives(
                root=args.root,
                inventory=SourceInventory.from_file(args.inventory),
                version=SemVer.parse(args.version),
                output_dir=args.output_dir,
                source_date_epoch=args.source_date_epoch,
            )
            print("\n".join(str(path) for path in paths))
        elif args.command == "checksums":
            write_atomic(args.output, build_sha256sums(args.assets))
            print(args.output)
        elif args.command == "sbom":
            inventory = SourceInventory.from_file(args.inventory)
            version = SemVer.parse(args.version)
            spdx, cyclonedx = build_sboms(
                version=version,
                files=inventory.expand(args.root),
                root=args.root,
                source_date_epoch=args.source_date_epoch,
                namespace_base=args.namespace_base,
            )
            spdx_path = args.output_dir / f"mcp-cpp-sdk-{version}.spdx.json"
            cyclonedx_path = args.output_dir / f"mcp-cpp-sdk-{version}.cdx.json"
            write_atomic(spdx_path, canonical_json_bytes(spdx))
            write_atomic(cyclonedx_path, canonical_json_bytes(cyclonedx))
            print(f"{spdx_path}\n{cyclonedx_path}")
        elif args.command == "manifest":
            spec = _json_object(args.spec)
            payloads = spec.pop("payloads")
            from .artifacts import ArtifactRecord

            records = [ArtifactRecord(**record) for record in payloads]
            spec["version"] = SemVer.parse(spec["version"])
            write_atomic(args.output, canonical_json_bytes(build_release_manifest(payloads=records, **spec)))
            print(args.output)
        elif args.command == "render":
            values = _json_object(args.values)
            if not all(isinstance(key, str) and isinstance(value, str) for key, value in values.items()):
                raise ValidationError("template values must be strings")
            render_file(args.template, args.output, values)
            print(args.output)
        else:
            raise AssertionError(f"unhandled command: {args.command}")
    except (OSError, json.JSONDecodeError, TypeError, KeyError, ValidationError) as error:
        print(f"release-tool: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
