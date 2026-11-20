#!/usr/bin/env python3
"""Produce ``test/data/json-schema-meta`` from pinned json-schema-spec commits.

The schema tests check the SDK's dialect checking against the metaschemas of the
dialects the SDK accepts: the root metaschema of each and, for 2020-12 and
2019-09, the vocabulary metaschemas it is composed of. They are test data only:
nothing under ``test/data`` is built into the library or installed.

Each file is downloaded from the commit pinned for it in FILES and must match
the SHA-256 recorded there, so a change upstream cannot change the data
silently. ``--check`` needs no network: it compares the committed tree with
those digests.

Usage:
  python3 scripts/vendor_json_schema_metaschemas.py            download and write the data
  python3 scripts/vendor_json_schema_metaschemas.py --check    fail if the committed data differs
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT_DIR = REPO_ROOT / "test" / "data" / "json-schema-meta"
REPOSITORY = "json-schema-org/json-schema-spec"
RAW_URL = "https://raw.githubusercontent.com/" + REPOSITORY + "/{commit}/{path}"

# The heads of the repository's branch for each dialect, and of its default branch for LICENSE,
# which the dialect branches do not carry.
COMMIT_2020_12 = "601a66c8b0f25246bf0e1fb488c5b5f030a79b72"
COMMIT_2019_09 = "c8eb3d320f60eca7cfb18da25337a426ceb40eaa"
COMMIT_DRAFT_07 = "20a3fee852db88519dcb3bb329b7a872ebad8953"
COMMIT_DRAFT_06 = "59ed5f6fc6f6386e23ca51d7f31d7fe9cf696713"
COMMIT_DRAFT_04 = "955d185db846cfca84269d9d711b10f4f3353d38"
COMMIT_LICENSE = "6c537d36f4781f48c27015e7f2bddaf7bcac0308"

# Output path: (commit, path in the repository, SHA-256).
FILES: dict[str, tuple[str, str, str]] = {
    "LICENSE": (COMMIT_LICENSE, "LICENSE", "909b25a80d4945b21d3adb2fb17f90bf592e0274bdf117c069c088a8e44dc7b6"),
    "draft2020-12/schema.json": (
        COMMIT_2020_12,
        "schema.json",
        "41da76f5afb7ce062d248f762463a92f7ca47e4e0f905b224ba6afeef91ded0f",
    ),
    "draft2020-12/meta/applicator.json": (
        COMMIT_2020_12,
        "meta/applicator.json",
        "bf273b26f9f735b93ece78f2b61b36676e1d122ce78ab37ad5a2e45dfa1ca2b1",
    ),
    "draft2020-12/meta/content.json": (
        COMMIT_2020_12,
        "meta/content.json",
        "a10456605b2b5bb12a1b4dcfc0300f02f54d3e8bb3646bed7724583866627682",
    ),
    "draft2020-12/meta/core.json": (
        COMMIT_2020_12,
        "meta/core.json",
        "21f79d143fab1f180245c331e5657057045b36794d41fe151e6e4fed65035299",
    ),
    "draft2020-12/meta/format-annotation.json": (
        COMMIT_2020_12,
        "meta/format-annotation.json",
        "5c79404f831dd905c0f40fefac7c6f3e51bf3729b4a876a5c2020178d97f3bcc",
    ),
    "draft2020-12/meta/format-assertion.json": (
        COMMIT_2020_12,
        "meta/format-assertion.json",
        "6a5a8e13c605e3eff51f9bf8da18078880d81ff1634e391760ccc2e16ee2146f",
    ),
    "draft2020-12/meta/meta-data.json": (
        COMMIT_2020_12,
        "meta/meta-data.json",
        "c664d438a84d58889c8edecd248ce2f945a4bc0e3b087323b11303dc136abfbe",
    ),
    "draft2020-12/meta/unevaluated.json": (
        COMMIT_2020_12,
        "meta/unevaluated.json",
        "fc99f32188da41689a9382af174dd42e8b255e4374965c157b8286556b4ab2bc",
    ),
    "draft2020-12/meta/validation.json": (
        COMMIT_2020_12,
        "meta/validation.json",
        "e921c5b79264d3689af01c1af1ffdf692e09f1c45df90a0f08eb7288c9acdeab",
    ),
    "draft2019-09/schema.json": (
        COMMIT_2019_09,
        "schema.json",
        "7b761b3e121f0a0ca1ea2a0b8e2cc856bcf801604b8268d71bacaa202bc0c13b",
    ),
    "draft2019-09/meta/applicator.json": (
        COMMIT_2019_09,
        "meta/applicator.json",
        "6b5f4e2fbc326d63cef6f6a3fa53bbfd3da39323116bc2eee866d716afc0e52c",
    ),
    "draft2019-09/meta/content.json": (
        COMMIT_2019_09,
        "meta/content.json",
        "9054737ada8e2ffbd9c9487ea406a86af8b57dcfd269543f04aba782936e3385",
    ),
    "draft2019-09/meta/core.json": (
        COMMIT_2019_09,
        "meta/core.json",
        "a6751523406d9ca767032ae97eb1c00c2de6d3cac9fc7d619babb460131cc1cb",
    ),
    "draft2019-09/meta/format.json": (
        COMMIT_2019_09,
        "meta/format.json",
        "a53cf235aab26c7639e70e94656136b20bcc364379c6a129cc06377982f91e96",
    ),
    "draft2019-09/meta/meta-data.json": (
        COMMIT_2019_09,
        "meta/meta-data.json",
        "ebcce1cded976590c75b4e5ee18ac471ffd8d21575a41d3ba05f59946fcbb5ce",
    ),
    "draft2019-09/meta/validation.json": (
        COMMIT_2019_09,
        "meta/validation.json",
        "c83c5121d0afd313846002429cd16b08ab77be022f423c70f0cfb64adc1069b7",
    ),
    "draft7/schema.json": (
        COMMIT_DRAFT_07,
        "schema.json",
        "692e1d165e47afcb5f11b2ce1c639635ffa834035d6ecb6bcf3087481dae8404",
    ),
    "draft6/schema.json": (
        COMMIT_DRAFT_06,
        "schema.json",
        "0064e880ba214bff1cee511e678fbe48c9826dd359e91e2423fc36de67b2ebe1",
    ),
    "draft4/schema.json": (
        COMMIT_DRAFT_04,
        "schema.json",
        "e1489d0b4755f02793302591d3fcb8f07b6893a82a94f24895f8e4edf11b82e2",
    ),
}


def provenance() -> bytes:
    lines = [
        f"Metaschemas from https://github.com/{REPOSITORY}, copied unchanged; test data only.",
        "LICENSE is the repository's licence, taken from its default branch.",
        "output path <- commit:path sha256",
    ]
    for output, (commit, path, digest) in sorted(FILES.items()):
        lines.append(f"{output} <- {commit}:{path} {digest}")
    return ("\n".join(lines) + "\n").encode()


def download() -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for output, (commit, path, digest) in FILES.items():
        url = RAW_URL.format(commit=commit, path=path)
        with urllib.request.urlopen(url, timeout=120) as response:  # noqa: S310
            data = response.read()
        actual = hashlib.sha256(data).hexdigest()
        if actual != digest:
            raise SystemExit(f"{output}: SHA-256 mismatch: got {actual}, expected {digest} ({url})")
        files[output] = data
    return files


def check() -> list[str]:
    """The differences between the committed tree and FILES; empty when they match."""
    expected = {output: digest for output, (_, _, digest) in FILES.items()}
    actual = {
        path.relative_to(OUTPUT_DIR).as_posix(): path
        for path in (OUTPUT_DIR.rglob("*") if OUTPUT_DIR.exists() else [])
        if path.is_file()
    }
    problems = [f"missing: {name}" for name in expected.keys() - actual.keys() - {"PROVENANCE"}]
    problems += [f"unexpected: {name}" for name in actual.keys() - expected.keys() - {"PROVENANCE"}]
    for name in expected.keys() & actual.keys():
        if hashlib.sha256(actual[name].read_bytes()).hexdigest() != expected[name]:
            problems.append(f"differs: {name}")
    if "PROVENANCE" not in actual:
        problems.append("missing: PROVENANCE")
    elif actual["PROVENANCE"].read_bytes() != provenance():
        problems.append("differs: PROVENANCE")
    return sorted(problems)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="compare instead of writing")
    args = parser.parse_args()

    if args.check:
        problems = check()
        if problems:
            print("test/data/json-schema-meta does not match the pinned metaschemas:")
            for problem in problems:
                print(f"  {problem}")
            print("Regenerate it with: python3 scripts/vendor_json_schema_metaschemas.py")
            return 1
        print("test/data/json-schema-meta matches the pinned metaschemas")
        return 0

    files = download()
    if OUTPUT_DIR.exists():
        shutil.rmtree(OUTPUT_DIR)
    for output, data in sorted(files.items()):
        target = OUTPUT_DIR / output
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (OUTPUT_DIR / "PROVENANCE").write_bytes(provenance())
    print(f"wrote {len(files)} files to {OUTPUT_DIR.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
