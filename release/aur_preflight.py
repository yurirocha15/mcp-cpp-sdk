#!/usr/bin/env python3
"""Read-only AUR availability preflight with first-publication support."""

from __future__ import annotations

import argparse
import json
import re
import ssl
import subprocess
import sys
from typing import Mapping, Sequence
from urllib.parse import quote
from urllib.error import HTTPError, URLError
from urllib.request import HTTPSHandler, HTTPRedirectHandler, Request, build_opener


_PACKAGE = re.compile(r"[a-z0-9][a-z0-9+._-]{0,79}")
_REF = re.compile(r"([0-9a-f]{40})\trefs/heads/master")
_MAX_RPC_BYTES = 65536


class AurPreflightError(ValueError):
    """Raised when public AUR views are malformed or disagree."""


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001
        return None


def verify_public_state(package: str, ls_remote: str, rpc_value: object) -> str:
    """Return ``absent`` or ``existing`` only when Git and RPC agree exactly."""

    if _PACKAGE.fullmatch(package) is None:
        raise AurPreflightError("AUR package base is malformed")
    lines = [line for line in ls_remote.splitlines() if line]
    if len(lines) > 1 or (lines and _REF.fullmatch(lines[0]) is None):
        raise AurPreflightError("AUR master ref response is malformed or ambiguous")
    if not isinstance(rpc_value, Mapping) or set(rpc_value) != {
        "version", "type", "resultcount", "results"
    }:
        raise AurPreflightError("AUR RPC response shape is not exact")
    result_count = rpc_value["resultcount"]
    results = rpc_value["results"]
    if (
        rpc_value["version"] != 5
        or rpc_value["type"] != "multiinfo"
        or type(result_count) is not int
        or result_count not in {0, 1}
        or not isinstance(results, list)
        or len(results) != result_count
    ):
        raise AurPreflightError("AUR RPC result is malformed or ambiguous")
    rpc_exists = result_count == 1
    if rpc_exists:
        record = results[0]
        if (
            not isinstance(record, Mapping)
            or record.get("Name") != package
            or record.get("PackageBase") != package
        ):
            raise AurPreflightError("AUR RPC package identity differs from the requested base")
    git_exists = bool(lines)
    if git_exists != rpc_exists:
        raise AurPreflightError("AUR Git and RPC views disagree")
    return "existing" if git_exists else "absent"


def probe_public_state(package: str) -> str:
    if _PACKAGE.fullmatch(package) is None:
        raise AurPreflightError("AUR package base is malformed")
    remote = f"https://aur.archlinux.org/{package}.git"
    git = subprocess.run(
        ["git", "ls-remote", "--refs", remote, "refs/heads/master"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    if git.returncode != 0 or len(git.stdout.encode("utf-8")) > 4096:
        raise AurPreflightError("public AUR Git query failed")
    url = f"https://aur.archlinux.org/rpc/v5/info?arg%5B%5D={quote(package, safe='')}"
    opener = build_opener(_NoRedirect(), HTTPSHandler(context=ssl.create_default_context()))
    try:
        with opener.open(
            Request(url, headers={"Accept": "application/json", "User-Agent": "mcp-cpp-sdk-release-preflight/1"}),
            timeout=30,
        ) as response:
            if response.status != 200 or response.geturl() != url:
                raise AurPreflightError("AUR RPC returned an unexpected response")
            raw = response.read(_MAX_RPC_BYTES + 1)
    except AurPreflightError:
        raise
    except (HTTPError, URLError, TimeoutError, ssl.SSLError, OSError) as error:
        raise AurPreflightError("public AUR RPC query failed") from error
    if not raw or len(raw) > _MAX_RPC_BYTES:
        raise AurPreflightError("AUR RPC response size is invalid")
    try:
        rpc_value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AurPreflightError("AUR RPC response is not UTF-8 JSON") from error
    return verify_public_state(package, git.stdout, rpc_value)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True)
    args = parser.parse_args(argv)
    try:
        print(probe_public_state(args.package))
    except (AurPreflightError, OSError, subprocess.SubprocessError) as error:
        print(f"aur-preflight: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
