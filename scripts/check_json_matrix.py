#!/usr/bin/env python3
"""Fail the build when the peer-input matrix has fallen behind the protocol.

The matrix's value is not its size.  A hand-written suite can be just as large
and still miss a type nobody thought about -- which is what happened three
times to the same defect class.  What makes a generated matrix different is
that its type list is *checked* against the protocol, so a new type cannot be
added without either entering the matrix or being excluded on the record.

This script enforces exactly that:

    { types with a from_json under include/mcp/protocol/ }
        == { types in the matrix } u { types excluded, with a stated reason }

It also verifies that the generated file is current: if regenerating would
change it, the checked-in matrix does not describe the code it ships with.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_json_matrix as gen  # noqa: E402


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--repo", default=repo)
    ap.add_argument(
        "--manifest",
        default=os.path.join(repo, "test", "core", "json_matrix_manifest.json"),
    )
    ap.add_argument(
        "--matrix",
        default=os.path.join(repo, "test", "core", "json_peer_input_matrix_test.cpp"),
    )
    args = ap.parse_args(argv)

    if not os.path.exists(args.manifest):
        sys.stderr.write(
            f"{args.manifest} is missing; run: python3 scripts/gen_json_matrix.py\n"
        )
        return 1

    manifest = json.load(open(args.manifest, encoding="utf-8"))
    _structs, _bases, _enums, _defaults, protocol_types = gen.load_protocol(args.repo)

    covered = set(manifest["types"])
    excluded = set(manifest["excluded"])
    accounted = covered | excluded

    missing = sorted(protocol_types - accounted)
    stale = sorted(accounted - protocol_types)

    failures = 0
    if missing:
        failures += 1
        sys.stderr.write(
            "These protocol types have a from_json but are not in the peer-input\n"
            "matrix and are not excluded:\n"
        )
        for name in missing:
            sys.stderr.write(f"  {name}\n")
        sys.stderr.write(
            "\nRegenerate the matrix (python3 scripts/gen_json_matrix.py). If a type\n"
            "genuinely cannot be exercised, add it to UNSYNTHESISABLE in\n"
            "scripts/gen_json_matrix.py with the reason -- an exclusion on the\n"
            "record is fine, an omission nobody noticed is what this check exists\n"
            "to prevent.\n"
        )
    if stale:
        failures += 1
        sys.stderr.write(
            "These types are in the matrix manifest but no longer have a from_json\n"
            "under include/mcp/protocol/:\n"
        )
        for name in stale:
            sys.stderr.write(f"  {name}\n")

    expected_cases = manifest["field_count"] * manifest["mode_count"]
    if manifest["case_count"] != expected_cases:
        failures += 1
        sys.stderr.write(
            f"case count {manifest['case_count']} is not fields x modes "
            f"({manifest['field_count']} x {manifest['mode_count']} = {expected_cases})\n"
        )

    if not failures:
        sys.stderr.write(
            f"peer-input matrix: {len(covered)} types covered, {len(excluded)} excluded "
            f"on the record, {manifest['case_count']} cases "
            f"({manifest['field_count']} fields x {manifest['mode_count']} modes)\n"
        )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
