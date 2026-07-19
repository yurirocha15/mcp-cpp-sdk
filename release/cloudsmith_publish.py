#!/usr/bin/env python3
"""Validate and publish exact signed native-package routes through Cloudsmith."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from typing import Callable, Mapping, Sequence

from release.artifacts import NativeRoute, load_native_targets
from release.model import ValidationError
from release.publication_contract import load_publication_contract


COORDINATE = re.compile(r"[a-z0-9][a-z0-9-]{0,63}")
SAFE_ROUTE_VALUE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}")
DEFAULT_POLL_ATTEMPTS = 30
DEFAULT_POLL_SECONDS = 10.0


class CloudsmithPublishError(ValueError):
    """Raised when repository or route state is ambiguous or conflicting."""


def _coordinates(namespace: str, repository: str) -> str:
    if COORDINATE.fullmatch(namespace) is None or COORDINATE.fullmatch(repository) is None:
        raise CloudsmithPublishError("Cloudsmith coordinates are missing or invalid")
    return f"{namespace}/{repository}"


def _run(arguments: list[str]) -> str:
    return subprocess.run(
        arguments, check=True, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=120,
    ).stdout


def _listing(text: str) -> list[dict[str, object]]:
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        raise CloudsmithPublishError("Cloudsmith returned malformed JSON") from error
    if not isinstance(value, dict) or not isinstance(value.get("data"), list):
        raise CloudsmithPublishError("Cloudsmith returned an unexpected package-list schema")
    if any(not isinstance(item, dict) for item in value["data"]):
        raise CloudsmithPublishError("Cloudsmith package-list record is malformed")
    return value["data"]


def _response_data(text: str) -> object:
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        raise CloudsmithPublishError("Cloudsmith returned malformed JSON") from error
    if not isinstance(value, dict) or set(value) != {"data"}:
        raise CloudsmithPublishError("Cloudsmith returned an unexpected response envelope")
    return value["data"]


def _verify_service_identity(
    expected_username: str, *, run: Callable[[list[str]], str]
) -> None:
    if COORDINATE.fullmatch(expected_username) is None:
        raise CloudsmithPublishError("expected Cloudsmith service identity is invalid")
    identity = _response_data(run(["cloudsmith", "whoami", "-F", "json"]))
    if (
        not isinstance(identity, dict)
        or identity.get("is_authenticated") is not True
        or identity.get("username") != expected_username
    ):
        raise CloudsmithPublishError(
            "Cloudsmith OIDC service identity differs from configuration"
        )


def preflight(
    namespace: str,
    repository: str,
    *,
    expected_username: str,
    formats: Sequence[str],
    native_targets: Path | None = None,
    embedded_targets: Sequence[object] | None = None,
    run: Callable[[list[str]], str] = _run,
) -> None:
    coordinate = _coordinates(namespace, repository)
    if (
        not formats
        or len(set(formats)) != len(formats)
        or any(value not in {"apt", "rpm"} for value in formats)
    ):
        raise CloudsmithPublishError("selected Cloudsmith formats are invalid")
    _verify_service_identity(expected_username, run=run)

    repositories = _listing(
        run(["cloudsmith", "list", "repos", coordinate, "-F", "json"])
    )
    exact_repositories = [
        item
        for item in repositories
        if item.get("namespace") == namespace and item.get("slug") == repository
    ]
    if len(exact_repositories) != 1:
        raise CloudsmithPublishError("Cloudsmith repository identity is missing or ambiguous")
    if exact_repositories[0].get("repository_type_str") != "Open-Source":
        raise CloudsmithPublishError("Cloudsmith repository is not Open-Source")

    if (native_targets is None) == (embedded_targets is None):
        raise CloudsmithPublishError("exactly one native target policy source is required")
    targets = (
        load_native_targets(native_targets)
        if native_targets is not None
        else tuple(embedded_targets or ())
    )
    selected_formats = set(formats)
    expected_routes: dict[str, set[tuple[str, str]]] = {
        provider: set()
        for package_format, provider in (("apt", "deb"), ("rpm", "rpm"))
        if package_format in selected_formats
    }
    for target in targets:
        package_format = "deb" if target.format == "apt" else "rpm"
        if package_format in expected_routes:
            expected_routes[package_format].add((target.distribution, target.release))
    for package_format, routes in expected_routes.items():
        distros = _listing(
            run(["cloudsmith", "list", "distros", package_format, "-F", "json"])
        )
        available: set[tuple[str, str]] = set()
        for distro in distros:
            slug = distro.get("slug")
            versions = distro.get("versions")
            if not isinstance(slug, str) or not isinstance(versions, list):
                raise CloudsmithPublishError("Cloudsmith distro inventory is malformed")
            for version in versions:
                if not isinstance(version, dict) or not isinstance(version.get("slug"), str):
                    raise CloudsmithPublishError("Cloudsmith distro release is malformed")
                available.add((slug, version["slug"]))
        missing = sorted(routes - available)
        if missing:
            rendered = ", ".join(f"{distro}/{release}" for distro, release in missing)
            raise CloudsmithPublishError(
                f"Cloudsmith does not support required {package_format} routes: {rendered}"
            )


def _route_query(route: NativeRoute, package_format: str) -> str:
    provider_format = "deb" if package_format == "apt" else "rpm"
    return (
        f"filename:{route.asset} AND format:{provider_format} "
        f"AND distribution:{route.distribution}/{route.release}"
    )


def _list_route(
    *,
    coordinate: str,
    route: NativeRoute,
    package_format: str,
    run: Callable[[list[str]], str],
) -> list[dict[str, object]]:
    return _listing(
        run(
            [
                "cloudsmith", "list", "packages", coordinate, "-F", "json",
                "--page-all", "-q", _route_query(route, package_format),
            ]
        )
    )


def _exact_record(
    records: Sequence[Mapping[str, object]], *, route: NativeRoute
) -> Mapping[str, object] | None:
    exact = [item for item in records if item.get("filename") == route.asset]
    if len(exact) > 1:
        raise CloudsmithPublishError("Cloudsmith contains an ambiguous duplicate native identity")
    return exact[0] if exact else None


def _validate_record_identity(
    record: Mapping[str, object],
    *,
    route: NativeRoute,
    package_format: str,
    digest: str,
) -> None:
    distro = record.get("distro")
    distro_version = record.get("distro_version")
    provider_format = "deb" if package_format == "apt" else "rpm"
    if record.get("checksum_sha256") != digest:
        raise CloudsmithPublishError("Cloudsmith native identity has a conflicting SHA-256")
    if record.get("format") != provider_format:
        raise CloudsmithPublishError("Cloudsmith native identity has a conflicting package format")
    if not isinstance(distro, Mapping) or distro.get("slug") != route.distribution:
        raise CloudsmithPublishError("Cloudsmith native identity has a conflicting distribution")
    if (
        not isinstance(distro_version, Mapping)
        or distro_version.get("slug") != route.release
    ):
        raise CloudsmithPublishError("Cloudsmith native identity has a conflicting release route")


def _sync_state(record: Mapping[str, object]) -> str:
    if record.get("is_sync_failed") is True:
        return "failed"
    if record.get("is_sync_completed") is True and record.get("is_sync_failed") is False:
        return "completed"
    return "pending"


def _wait_for_routes(
    *,
    coordinate: str,
    pending: Sequence[tuple[NativeRoute, str]],
    package_format: str,
    run: Callable[[list[str]], str],
    sleep: Callable[[float], None],
    poll_attempts: int,
    poll_seconds: float,
) -> None:
    if type(poll_attempts) is not int or poll_attempts < 1:
        raise CloudsmithPublishError("Cloudsmith poll attempt count must be positive")
    if not isinstance(poll_seconds, (int, float)) or poll_seconds < 0:
        raise CloudsmithPublishError("Cloudsmith poll interval must be non-negative")
    remaining = list(pending)
    for attempt in range(poll_attempts):
        next_remaining: list[tuple[NativeRoute, str]] = []
        for route, digest in remaining:
            record = _exact_record(
                _list_route(
                    coordinate=coordinate,
                    route=route,
                    package_format=package_format,
                    run=run,
                ),
                route=route,
            )
            if record is None:
                next_remaining.append((route, digest))
                continue
            _validate_record_identity(
                record,
                route=route,
                package_format=package_format,
                digest=digest,
            )
            state = _sync_state(record)
            if state == "failed":
                raise CloudsmithPublishError("Cloudsmith package synchronization failed")
            if state != "completed":
                next_remaining.append((route, digest))
        if not next_remaining:
            return
        remaining = next_remaining
        if attempt + 1 < poll_attempts:
            sleep(float(poll_seconds))
    raise CloudsmithPublishError(
        "Cloudsmith package did not reach a verified completed state before timeout"
    )


def publish_routes(
    directory: Path,
    route_file: Path,
    *,
    package_format: str,
    namespace: str,
    repository: str,
    expected_username: str,
    run: Callable[[list[str]], str] = _run,
    sleep: Callable[[float], None] = time.sleep,
    poll_attempts: int = DEFAULT_POLL_ATTEMPTS,
    poll_seconds: float = DEFAULT_POLL_SECONDS,
) -> str:
    if package_format not in {"apt", "rpm"}:
        raise CloudsmithPublishError("unsupported Cloudsmith native format")
    coordinate = _coordinates(namespace, repository)
    value = json.loads(route_file.read_text(encoding="utf-8"))
    if not isinstance(value, list):
        raise CloudsmithPublishError("signed native route inventory is malformed")
    routes = [NativeRoute.from_mapping(item) for item in value]
    selected = [route for route in routes if route.format == package_format]
    if not selected:
        raise CloudsmithPublishError(f"immutable release has no {package_format.upper()} routes")
    if len({route.asset for route in selected}) != len(selected):
        raise CloudsmithPublishError("immutable release contains duplicate Cloudsmith routes")
    local_candidates: list[tuple[NativeRoute, Path, str]] = []
    for route in selected:
        if any(
            SAFE_ROUTE_VALUE.fullmatch(value) is None
            for value in (route.asset, route.distribution, route.release)
        ):
            raise CloudsmithPublishError("native route contains an unsafe coordinate")
        path = directory / route.asset
        if path.is_symlink() or not path.is_file():
            raise CloudsmithPublishError("signed native route asset is missing or unsafe")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        local_candidates.append((route, path, digest))

    # This must be the first authenticated Cloudsmith command. All local signed
    # input validation happens before the identity and provider-operation boundary.
    _verify_service_identity(expected_username, run=run)

    candidates: list[tuple[NativeRoute, Path, str, str]] = []
    for route, path, digest in local_candidates:
        records = _list_route(
            coordinate=coordinate,
            route=route,
            package_format=package_format,
            run=run,
        )
        exact = _exact_record(records, route=route)
        state = "missing"
        if exact is not None:
            _validate_record_identity(
                exact,
                route=route,
                package_format=package_format,
                digest=digest,
            )
            state = _sync_state(exact)
            if state == "failed":
                raise CloudsmithPublishError("Cloudsmith package synchronization failed")
        candidates.append((route, path, digest, state))

    published = False
    pending: list[tuple[NativeRoute, str]] = []
    for route, path, digest, state in candidates:
        if state == "completed":
            continue
        if state == "missing":
            destination = f"{coordinate}/{route.distribution}/{route.release}"
            run(
                [
                    "cloudsmith",
                    "push",
                    "deb" if package_format == "apt" else "rpm",
                    destination,
                    str(path),
                ]
            )
            published = True
        pending.append((route, digest))

    if pending:
        _wait_for_routes(
            coordinate=coordinate,
            pending=pending,
            package_format=package_format,
            run=run,
            sleep=sleep,
            poll_attempts=poll_attempts,
            poll_seconds=poll_seconds,
        )
    return "PUBLISHED" if published else "SKIPPED_ALREADY_IDENTICAL"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("preflight")
    for subparser in (check,):
        subparser.add_argument("--namespace", required=True)
        subparser.add_argument("--repository", required=True)
        subparser.add_argument("--expected-username", required=True)
        subparser.add_argument("--targets", type=Path, required=True)
        subparser.add_argument("--apt-selected", choices=("true", "false"), required=True)
        subparser.add_argument("--rpm-selected", choices=("true", "false"), required=True)
        subparser.add_argument("--signed-contract", type=Path)
        subparser.add_argument(
            "--policy-source", choices=("current", "embedded"), default="current"
        )
    publish = commands.add_parser("publish")
    publish.add_argument("--directory", type=Path, required=True)
    publish.add_argument("--routes", type=Path, required=True)
    publish.add_argument("--format", choices=("apt", "rpm"), required=True)
    publish.add_argument("--namespace", required=True)
    publish.add_argument("--repository", required=True)
    publish.add_argument("--expected-username", required=True)
    publish.add_argument("--github-output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "preflight":
            if args.policy_source == "embedded" and args.signed_contract is None:
                raise CloudsmithPublishError(
                    "Cloudsmith policy source and signed contract disagree"
                )
            preflight(
                args.namespace,
                args.repository,
                expected_username=args.expected_username,
                formats=tuple(
                    value
                    for value, selected in (
                        ("apt", args.apt_selected),
                        ("rpm", args.rpm_selected),
                    )
                    if selected == "true"
                ),
                native_targets=(
                    args.targets if args.policy_source == "current" else None
                ),
                embedded_targets=(
                    load_publication_contract(args.signed_contract).native_targets
                    if args.policy_source == "embedded"
                    else None
                ),
            )
        else:
            result = publish_routes(
                args.directory, args.routes, package_format=args.format,
                namespace=args.namespace, repository=args.repository,
                expected_username=args.expected_username,
            )
            with args.github_output.open("a", encoding="ascii") as output:
                output.write(f"result={result}\n")
    except (
        CloudsmithPublishError, OSError, UnicodeError, json.JSONDecodeError,
        subprocess.SubprocessError, ValidationError,
    ) as error:
        print(f"cloudsmith-publish: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
