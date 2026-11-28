#!/usr/bin/env python3
"""Fail when the feature matrix no longer matches the repository.

``docs/feature_matrix.json`` lists the 48 non-experimental features of the MCP
SDK feature list. This check fails when:

* the rows are not exactly the 48 official features, ids 1 to 48, in the
  official order, or a row has a malformed field;
* the committed ``docs/feature-matrix.rst`` differs from what
  ``gen_feature_matrix.py`` writes;
* the guide of a row names a page or a section that does not exist;
* an ``implemented`` row lacks an API symbol that appears under ``include/``,
  or its example file is missing, does not contain the row's marker, or is not
  registered in ``CMakeLists.txt``;
* a ``not-implemented`` row has no reason, or its guide section does not state
  the reason;
* the number of implemented rows differs from EXPECTED_IMPLEMENTED.

Usage:
    python3 scripts/check_feature_matrix.py
"""

from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_feature_matrix as gen  # noqa: E402

REPO = gen.REPO

# Every official feature is implemented; a row that is not must be counted here.
EXPECTED_IMPLEMENTED = 48

_ROW_KEYS = {"id", "name", "method", "status", "api", "guide", "example", "reason"}


def _read(root: str, relative: str) -> str | None:
    path = os.path.join(root, relative)
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def _header_text(root: str) -> str:
    """The text of every header under include/."""
    parts = []
    for folder, _, names in os.walk(os.path.join(root, "include")):
        for name in sorted(names):
            if name.endswith((".hpp", ".h")):
                with open(os.path.join(folder, name), encoding="utf-8") as handle:
                    parts.append(handle.read())
    return "\n".join(parts)


def _registered(cmake: str, example: str) -> bool:
    """Whether CMakeLists.txt builds the example file."""
    if example in cmake:
        return True
    folder, _, name = example.rpartition("/")
    stem = name.rpartition(".")[0]
    # A loop over example names, as the sync and async programs are registered.
    return f"{folder}/${{" in cmake and re.search(rf"\b{re.escape(stem)}\b", cmake) is not None


def _check_row(row: dict, position: int, root: str, headers: str, cmake: str) -> list[str]:
    label = f"row {row.get('id', position)} ({row.get('name')})"
    problems: list[str] = []

    if set(row) != _ROW_KEYS:
        return [f"{label}: keys are {sorted(row)}, expected {sorted(_ROW_KEYS)}"]
    if row["id"] != position:
        problems.append(f"{label}: id is {row['id']}, expected {position}")
    if row["name"] != gen.OFFICIAL_FEATURES[position - 1]:
        problems.append(f"{label}: name should be {gen.OFFICIAL_FEATURES[position - 1]!r}")
    if row["status"] not in gen.STATUSES:
        problems.append(f"{label}: status {row['status']!r} is not one of {gen.STATUSES}")
        return problems
    if not isinstance(row["api"], list) or not all(isinstance(s, str) for s in row["api"]):
        problems.append(f"{label}: api is not a list of strings")
        return problems

    section = gen.find_section(root, row["guide"])
    if section is None:
        problems.append(f"{label}: guide {row['guide']!r} names no page or section")

    if row["status"] == "implemented":
        if row["reason"] is not None:
            problems.append(f"{label}: an implemented row has a reason")
        if not row["api"]:
            problems.append(f"{label}: no API symbol")
        for symbol in row["api"]:
            if not re.search(rf"\b{re.escape(symbol)}\b", headers):
                problems.append(f"{label}: symbol {symbol!r} is in no header under include/")
        example = row["example"]
        if not isinstance(example, dict) or set(example) != {"file", "marker"}:
            problems.append(f"{label}: no example with a file and a marker")
            return problems
        text = _read(root, example["file"])
        if text is None:
            problems.append(f"{label}: example file {example['file']} does not exist")
        elif not example["marker"] or example["marker"] not in text:
            problems.append(f"{label}: marker {example['marker']!r} is not in {example['file']}")
        if not _registered(cmake, example["file"]):
            problems.append(f"{label}: {example['file']} is not registered in CMakeLists.txt")
    else:
        reason = row["reason"]
        if not isinstance(reason, str) or not reason.strip():
            problems.append(f"{label}: a not-implemented row needs a reason")
        elif section is not None and reason not in section["body"]:
            problems.append(f"{label}: the guide section does not state the reason")
    return problems


def check(manifest: dict, committed_page: str | None, root: str = REPO) -> list[str]:
    """The problems found; an empty list means the matrix is current."""
    rows = manifest.get("features")
    if not isinstance(rows, list):
        return ["the manifest has no 'features' list"]
    problems: list[str] = []
    if len(rows) != len(gen.OFFICIAL_FEATURES):
        problems.append(f"{len(rows)} rows, expected {len(gen.OFFICIAL_FEATURES)}")

    headers = _header_text(root)
    cmake = _read(root, "CMakeLists.txt") or ""
    for position, row in enumerate(rows, start=1):
        if not isinstance(row, dict):
            problems.append(f"row {position} is not an object")
            continue
        problems += _check_row(row, position, root, headers, cmake)

    implemented = sum(1 for r in rows if isinstance(r, dict) and r.get("status") == "implemented")
    if implemented != EXPECTED_IMPLEMENTED:
        problems.append(f"{implemented} implemented rows, expected {EXPECTED_IMPLEMENTED}")

    if not problems:
        if committed_page is None:
            problems.append(f"{gen.PAGE} does not exist; run scripts/gen_feature_matrix.py")
        elif committed_page != gen.render(manifest, root):
            problems.append(f"{gen.PAGE} is stale; run scripts/gen_feature_matrix.py")
    return problems


def main() -> int:
    manifest = gen.load_manifest()
    problems = check(manifest, _read(REPO, gen.PAGE))
    if problems:
        print("The feature matrix is out of date:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    rows = manifest["features"]
    implemented = sum(1 for r in rows if r["status"] == "implemented")
    print(f"feature matrix ok: {len(rows)} rows, {implemented} implemented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
