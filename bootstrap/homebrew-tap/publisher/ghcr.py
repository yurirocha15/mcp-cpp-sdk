#!/usr/bin/env python3
"""Resolve public GHCR bottle tags and bind them to exact manifest digests."""

from __future__ import annotations

import argparse
import http.client
import json
from pathlib import Path
import re
import sys
import time
import urllib.parse
from collections.abc import Callable, Mapping


PACKAGE = "yurirocha15/mcp-cpp-sdk/mcp-cpp-sdk"
DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
VERSION = re.compile(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
TAG = re.compile(r"[a-z0-9_]+")
ACCEPT = ", ".join(
    (
        "application/vnd.oci.image.index.v1+json",
        "application/vnd.oci.image.manifest.v1+json",
        "application/vnd.docker.distribution.manifest.list.v2+json",
        "application/vnd.docker.distribution.manifest.v2+json",
    )
)


class GhcrError(ValueError):
    """Raised when an exact public GHCR identity cannot be established."""


class Client:
    def _get(self, path: str, headers: Mapping[str, str]) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPSConnection("ghcr.io", timeout=20)
        try:
            connection.request("GET", path, headers=dict(headers))
            response = connection.getresponse()
            body = response.read(65537)
            if len(body) > 65536 or 300 <= response.status < 400:
                raise GhcrError("GHCR response is oversized or redirected")
            return (
                response.status,
                {name.lower(): value for name, value in response.getheaders()},
                body,
            )
        finally:
            connection.close()

    def token(self) -> str:
        query = urllib.parse.urlencode(
            {"scope": f"repository:{PACKAGE}:pull", "service": "ghcr.io"}
        )
        status, _, body = self._get(
            f"/token?{query}", {"User-Agent": "mcp-cpp-sdk-homebrew-finalizer/2"}
        )
        if status != 200:
            raise GhcrError("anonymous GHCR token request failed")
        try:
            token = json.loads(body).get("token")
        except (UnicodeError, json.JSONDecodeError) as error:
            raise GhcrError("anonymous GHCR token response is malformed") from error
        if not isinstance(token, str) or not token or len(token) > 8192:
            raise GhcrError("anonymous GHCR token is malformed")
        return token

    def _manifest(self, reference: str, token: str) -> tuple[str, object]:
        status, headers, body = self._get(
            f"/v2/{PACKAGE}/manifests/{urllib.parse.quote(reference, safe=':')}",
            {
                "Accept": ACCEPT,
                "Authorization": f"Bearer {token}",
                "User-Agent": "mcp-cpp-sdk-homebrew-finalizer/2",
            },
        )
        digest = headers.get("docker-content-digest", "")
        if status != 200 or DIGEST.fullmatch(digest) is None:
            raise GhcrError(f"public GHCR manifest is unavailable: {reference}")
        try:
            value = json.loads(body)
        except (UnicodeError, json.JSONDecodeError) as error:
            raise GhcrError("public GHCR manifest is not JSON") from error
        return digest, value

    def resolve(self, tag: str, bottle_sha256: str) -> str:
        if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+\.[a-z0-9_]+", tag) is None:
            raise GhcrError("GHCR bottle tag is malformed")
        if re.fullmatch(r"[0-9a-f]{64}", bottle_sha256) is None:
            raise GhcrError("bottle archive digest is malformed")
        token = self.token()
        digest, manifest = self._manifest(tag, token)
        annotations = []
        if isinstance(manifest, Mapping):
            top = manifest.get("annotations")
            if isinstance(top, Mapping):
                annotations.append(top)
            descriptors = manifest.get("manifests")
            if isinstance(descriptors, list):
                for descriptor in descriptors:
                    descriptor_annotations = (
                        descriptor.get("annotations") if isinstance(descriptor, Mapping) else None
                    )
                    if (
                        isinstance(descriptor_annotations, Mapping)
                        and descriptor_annotations.get("org.opencontainers.image.ref.name") == tag
                    ):
                        annotations.append(descriptor_annotations)
        matches = [
            item
            for item in annotations
            if item.get("sh.brew.bottle.digest") == bottle_sha256
        ]
        if len(matches) != 1:
            raise GhcrError("GHCR manifest does not bind the exact bottle archive digest")
        digest_reference, _ = self._manifest(digest, token)
        if digest_reference != digest:
            raise GhcrError("GHCR tag and digest references disagree")
        return digest


def expected_bottles(ledger: object, source_tag: str) -> dict[str, str]:
    if VERSION.fullmatch(source_tag) is None:
        raise GhcrError("source version is not canonical stable SemVer")
    version = source_tag.removeprefix("v")
    if not isinstance(ledger, Mapping) or ledger.get("schema_version") != 2:
        raise GhcrError("bottle ledger is malformed")
    bottles = ledger.get("bottles")
    if not isinstance(bottles, list) or len(bottles) != 4:
        raise GhcrError("bottle ledger does not contain exactly four bottles")
    tags: dict[str, str] = {}
    for bottle in bottles:
        tag = bottle.get("bottle_tag") if isinstance(bottle, Mapping) else None
        bottle_sha256 = bottle.get("bottle_sha256") if isinstance(bottle, Mapping) else None
        if (
            not isinstance(tag, str)
            or TAG.fullmatch(tag) is None
            or not isinstance(bottle_sha256, str)
            or re.fullmatch(r"[0-9a-f]{64}", bottle_sha256) is None
        ):
            raise GhcrError("bottle ledger contains a malformed tag")
        full_tag = f"{version}.{tag}"
        if full_tag in tags:
            raise GhcrError("bottle ledger contains duplicate tags")
        tags[full_tag] = bottle_sha256
    if len(tags) != 4:
        raise GhcrError("bottle ledger contains duplicate tags")
    return dict(sorted(tags.items()))


def expected_tags(ledger: object, source_tag: str) -> list[str]:
    return list(expected_bottles(ledger, source_tag))


def record(
    ledger: object,
    source_tag: str,
    resolver: Callable[[str, str], str],
) -> dict[str, object]:
    bottles = expected_bottles(ledger, source_tag)
    manifests = []
    for tag, bottle_sha256 in bottles.items():
        digest = resolver(tag, bottle_sha256)
        if DIGEST.fullmatch(digest) is None:
            raise GhcrError(f"resolver returned a malformed digest for {tag}")
        manifests.append(
            {
                "package": PACKAGE,
                "tag": tag,
                "digest": digest,
                "bottle_sha256": bottle_sha256,
            }
        )
    return {**ledger, "source_tag": source_tag, "ghcr_manifests": manifests}


def verify_recorded(ledger: object, resolver: Callable[[str, str], str]) -> None:
    if not isinstance(ledger, Mapping):
        raise GhcrError("finalization ledger is malformed")
    entries = ledger.get("ghcr_manifests")
    if not isinstance(entries, list) or len(entries) != 4:
        raise GhcrError("finalization ledger does not contain four GHCR manifests")
    source_tag = ledger.get("source_tag")
    if not isinstance(source_tag, str):
        raise GhcrError("finalization ledger source tag is missing")
    expected = set(expected_tags(ledger, source_tag))
    seen = set()
    for entry in entries:
        if not isinstance(entry, Mapping) or set(entry) != {
            "package", "tag", "digest", "bottle_sha256"
        }:
            raise GhcrError("GHCR manifest identity is malformed")
        tag, digest, bottle_sha256 = entry["tag"], entry["digest"], entry["bottle_sha256"]
        if (
            entry["package"] != PACKAGE
            or not isinstance(tag, str)
            or tag in seen
            or not isinstance(digest, str)
            or DIGEST.fullmatch(digest) is None
            or not isinstance(bottle_sha256, str)
            or re.fullmatch(r"[0-9a-f]{64}", bottle_sha256) is None
        ):
            raise GhcrError("GHCR manifest identity is noncanonical")
        seen.add(tag)
        if resolver(tag, bottle_sha256) != digest:
            raise GhcrError(f"GHCR tag changed digest before merge: {tag}")
    if seen != expected:
        raise GhcrError("recorded GHCR tags do not match the bottle matrix")


def record_with_retry(
    ledger: object,
    source_tag: str,
    resolver: Callable[[str, str], str],
    *,
    timeout_seconds: int,
    sleep: Callable[[float], None] = time.sleep,
) -> dict[str, object]:
    if not 0 <= timeout_seconds <= 600:
        raise GhcrError("GHCR propagation timeout is out of bounds")
    expected_tags(ledger, source_tag)
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            return record(ledger, source_tag, resolver)
        except GhcrError:
            if time.monotonic() >= deadline:
                raise
            sleep(min(10, max(0, deadline - time.monotonic())))


def _load(path: Path) -> object:
    return json.loads(path.read_text(encoding="ascii"))


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    record_parser = commands.add_parser("record")
    record_parser.add_argument("--ledger", type=Path, required=True)
    record_parser.add_argument("--source-tag", required=True)
    record_parser.add_argument("--output", type=Path, required=True)
    record_parser.add_argument("--timeout-seconds", type=int, default=600)
    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--ledger", type=Path, required=True)
    args = parser.parse_args()
    try:
        client = Client()
        if args.command == "record":
            value = record_with_retry(
                _load(args.ledger),
                args.source_tag,
                client.resolve,
                timeout_seconds=args.timeout_seconds,
            )
            args.output.write_text(
                json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="ascii",
            )
        else:
            verify_recorded(_load(args.ledger), client.resolve)
    except (GhcrError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"ghcr: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
