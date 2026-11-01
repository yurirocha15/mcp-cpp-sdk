#!/usr/bin/env python3
"""Produce ``test/data/json-schema`` from a pinned JSON-Schema-Test-Suite commit.

The SDK's schema tests run the suite's required cases (``tests/<dialect>/*.json``,
not ``optional/``) for the dialects listed in DIALECTS. The files are copied
unchanged with the suite's LICENSE; this script downloads the pinned commit's
tarball and verifies its SHA-256 first.

Usage:
  python3 scripts/vendor_json_schema_test_suite.py            regenerate the data
  python3 scripts/vendor_json_schema_test_suite.py --check    fail if the committed data differs
  --tarball PATH                                              use a local copy of the tarball
"""

from __future__ import annotations

import argparse
import hashlib
import io
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT_DIR = REPO_ROOT / "test" / "data" / "json-schema"

COMMIT = "55719286c33ae0d1823bfb65fea10338a19bc37c"
TARBALL_URL = f"https://github.com/json-schema-org/JSON-Schema-Test-Suite/archive/{COMMIT}.tar.gz"
TARBALL_SHA256 = "c65cf5b4152b9edfc71644252249e9402d3563ddaeb6f0fa80aeef2a85cefcf7"
DIALECTS = ("draft2020-12", "draft7")


def fetch_tarball(path: Path | None) -> bytes:
    if path is not None:
        data = path.read_bytes()
    else:
        with urllib.request.urlopen(TARBALL_URL, timeout=120) as response:  # noqa: S310
            data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != TARBALL_SHA256:
        raise SystemExit(f"test suite tarball SHA-256 mismatch: got {digest}, expected {TARBALL_SHA256}")
    return data


def read_suite(data: bytes) -> dict[str, bytes]:
    """Return {output path: bytes} for the selected files."""
    files: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            parts = member.name.split("/")
            if len(parts) < 2 or ".." in parts:
                continue
            relative = parts[1:]
            extracted = archive.extractfile(member)
            if extracted is None:
                continue
            if relative == ["LICENSE"]:
                files["LICENSE"] = extracted.read()
            elif (
                len(relative) == 3
                and relative[0] == "tests"
                and relative[1] in DIALECTS
                and relative[2].endswith(".json")
            ):
                files[f"{relative[1]}/{relative[2]}"] = extracted.read()
    if "LICENSE" not in files:
        raise SystemExit("test suite tarball has no LICENSE file")
    return files


def provenance() -> bytes:
    return (
        f"JSON-Schema-Test-Suite {COMMIT}\n"
        f"source: {TARBALL_URL}\n"
        f"sha256: {TARBALL_SHA256}\n"
        f"files: tests/{{{','.join(DIALECTS)}}}/*.json (required cases only), LICENSE; copied unchanged\n"
    ).encode()


def write_tree(root: Path, files: dict[str, bytes]) -> None:
    for relative, data in sorted(files.items()):
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (root / "PROVENANCE").write_bytes(provenance())


def tree_files(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="compare instead of writing")
    parser.add_argument("--tarball", type=Path, help="local copy of the pinned commit's tarball")
    args = parser.parse_args()

    files = read_suite(fetch_tarball(args.tarball))
    if not args.check:
        if OUTPUT_DIR.exists():
            shutil.rmtree(OUTPUT_DIR)
        write_tree(OUTPUT_DIR, files)
        print(f"wrote {len(files)} files to {OUTPUT_DIR.relative_to(REPO_ROOT)}")
        return 0

    with tempfile.TemporaryDirectory() as scratch:
        expected_root = Path(scratch)
        write_tree(expected_root, files)
        expected = tree_files(expected_root)
    actual = tree_files(OUTPUT_DIR) if OUTPUT_DIR.exists() else {}
    problems = sorted(
        [f"missing: {name}" for name in expected.keys() - actual.keys()]
        + [f"unexpected: {name}" for name in actual.keys() - expected.keys()]
        + [f"differs: {name}" for name in expected.keys() & actual.keys() if expected[name] != actual[name]]
    )
    if problems:
        print("test/data/json-schema does not match the pinned test suite commit:")
        for problem in problems:
            print(f"  {problem}")
        print("Regenerate it with: python3 scripts/vendor_json_schema_test_suite.py")
        return 1
    print(f"test/data/json-schema matches JSON-Schema-Test-Suite {COMMIT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
