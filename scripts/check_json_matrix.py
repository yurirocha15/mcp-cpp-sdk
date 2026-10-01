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

It also verifies that the generated file is current.  The checked-in matrix
and manifest must equal what ``gen_json_matrix.render()`` produces from the
current sources, compared with all whitespace removed so clang-format's line
wrapping does not count.  On a mismatch the rows on both sides are parsed to
say which way each one moved:

    stale       a decoder fix landed, validation got stricter, rows or types
                came or went, or the harness text was edited -- regenerate
                (exit 1)
    REGRESSION  a member now throws on an explicit null while it may be
                absent, or has become required, where the committed matrix
                says it tolerated the input -- fix the decoder (exit 2)
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_json_matrix as gen  # noqa: E402


MATRIX = "test/core/json_peer_input_matrix_test.cpp"
REGENERATE = "    python3 scripts/gen_json_matrix.py"


def check(
    committed_text: str,
    committed_manifest: dict,
    rendered_text: str,
    rendered_manifest: dict,
) -> tuple[int, list[str]]:
    """Compares the committed matrix with a fresh render; returns (exit code, lines)."""
    if (
        gen.strip_ws(committed_text) == gen.strip_ws(rendered_text)
        and committed_manifest == rendered_manifest
    ):
        m = committed_manifest
        return 0, [
            f"peer-input matrix: {len(m['types'])} types covered, {len(m['excluded'])} "
            f"excluded on the record, {m['case_count']} cases "
            f"({m['field_count']} fields x {m['mode_count']} modes); "
            "matches scripts/gen_json_matrix.py"
        ]

    committed = gen.parse_matrix(committed_text)
    if not committed["tests"]:
        return 1, [
            f"{MATRIX} has no TEST(JsonPeerInputMatrix, ...) to compare. Restore it "
            "from git before regenerating, so the new matrix is compared with the old:",
            f"    git checkout -- {MATRIX}",
        ]
    regressions, stale = gen.classify(committed, gen.parse_matrix(rendered_text))
    # Text that differs with no difference the parse can name is harness text.
    if not (regressions or stale) and gen.strip_ws(committed_text) != gen.strip_ws(rendered_text):
        stale.append(gen.HARNESS_DIFFERS)
    for key in sorted(set(committed_manifest) | set(rendered_manifest)):
        was, now = committed_manifest.get(key), rendered_manifest.get(key)
        if was == now:
            continue
        if key not in rendered_manifest:
            stale.append(f"manifest key '{key}': only in the committed file")
        elif key not in committed_manifest:
            stale.append(f"manifest key '{key}': only in the generated file")
        elif isinstance(was, (list, dict)) or isinstance(now, (list, dict)):
            stale.append(f"manifest key '{key}': committed and generated differ")
        else:
            stale.append(f"manifest key '{key}': committed {was}, generated {now}")

    lines: list[str] = []
    if regressions:
        lines += gen.regression_block(regressions)
    if stale:
        lines.append(
            f"peer-input matrix is stale: {MATRIX} does not match what "
            "scripts/gen_json_matrix.py generates from the current sources."
        )
        lines += [f"  {line}" for line in stale]
        if not regressions and any(line.endswith("(now tolerated)") for line in stale):
            lines += [
                "A row moving toward tolerance means a decoder fix landed. "
                "Regenerate and commit the result with the fix:",
                REGENERATE,
            ]
        elif not regressions:
            lines += [
                "The generated matrix changed (template, rows added or removed, or stricter "
                "validation). Review the diff, then regenerate:",
                REGENERATE,
            ]
    return (2 if regressions else 1), lines


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

    with open(args.manifest, encoding="utf-8") as fh:
        manifest = json.load(fh)
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

    if failures:
        return 1

    if not os.path.exists(args.matrix):
        sys.stderr.write(
            f"{args.matrix} is missing. Restore it from git before regenerating, so "
            f"the new matrix is compared with the old:\n    git checkout -- {MATRIX}\n"
        )
        return 1
    with open(args.matrix, encoding="utf-8") as fh:
        committed_text = fh.read()
    rendered_text, rendered_manifest = gen.render(args.repo)
    code, lines = check(committed_text, manifest, rendered_text, rendered_manifest)
    for line in lines:
        sys.stderr.write(line + "\n")
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
