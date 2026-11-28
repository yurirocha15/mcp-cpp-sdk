#!/usr/bin/env python3
"""Generate docs/feature-matrix.rst from docs/feature_matrix.json.

The manifest lists the 48 non-experimental features of the MCP SDK feature
list, in that list's order. Each row names the API symbols that implement the
feature, the guide section that explains it and the compiled example that
uses it. ``check_feature_matrix.py`` verifies those references and that the
committed page equals what this script writes.

Usage:
    python3 scripts/gen_feature_matrix.py
"""

from __future__ import annotations

import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join("docs", "feature_matrix.json")
PAGE = os.path.join("docs", "feature-matrix.rst")

STATUSES = ("implemented", "not-implemented")

# The features in the order of the official list.
OFFICIAL_FEATURES = (
    "Tools - listing",
    "Tools - calling",
    "Tools - text results",
    "Tools - image results",
    "Tools - audio results",
    "Tools - embedded resources",
    "Tools - error handling",
    "Tools - change notifications",
    "Resources - listing",
    "Resources - reading text",
    "Resources - reading binary",
    "Resources - templates",
    "Resources - template reading",
    "Resources - subscribing",
    "Resources - unsubscribing",
    "Resources - change notifications",
    "Prompts - listing",
    "Prompts - getting simple",
    "Prompts - getting with arguments",
    "Prompts - embedded resources",
    "Prompts - image content",
    "Prompts - change notifications",
    "Sampling - creating messages",
    "Elicitation - form mode",
    "Elicitation - URL mode",
    "Elicitation - schema validation",
    "Elicitation - default values",
    "Elicitation - enum values",
    "Elicitation - complete notification",
    "Roots - listing",
    "Roots - change notifications",
    "Logging - sending log messages",
    "Logging - setting level",
    "Completions - resource argument",
    "Completions - prompt argument",
    "Ping",
    "Streamable HTTP transport (client)",
    "Streamable HTTP transport (server)",
    "SSE transport - legacy (client)",
    "SSE transport - legacy (server)",
    "stdio transport (client)",
    "stdio transport (server)",
    "Progress notifications",
    "Cancellation",
    "Pagination",
    "Capability negotiation",
    "Protocol version negotiation",
    "JSON Schema 2020-12 support",
)

_ADORNMENT = re.compile(r"([=\-~^\"#*+`:.'_])\1{2,}\s*$")


def load_manifest(root: str = REPO) -> dict:
    with open(os.path.join(root, MANIFEST), encoding="utf-8") as handle:
        return json.load(handle)


def make_id(title: str) -> str:
    """The id docutils gives a section with this title."""
    title = title.replace("``", "").lower()
    title = re.sub(r"[^a-z0-9]+", "-", " ".join(title.split()))
    return re.sub(r"^[-0-9]+|-+$", "", title)


def sections(text: str) -> list[dict]:
    """The sections of an RST page: title, id, level, and the text of its body.

    A section's body runs to the next section of the same or a shallower level.
    """
    lines = text.split("\n")
    found: list[dict] = []
    levels: list[str] = []
    seen: dict[str, int] = {}
    for i in range(1, len(lines)):
        line, title = lines[i], lines[i - 1]
        if not title.strip() or title.startswith((" ", ".. ")):
            continue
        if not _ADORNMENT.match(line) or len(line.rstrip()) < len(title.rstrip()):
            continue
        if _ADORNMENT.match(title):
            continue
        char = line[0]
        if char not in levels:
            levels.append(char)
        base = make_id(title)
        count = seen.get(base, 0)
        seen[base] = count + 1
        found.append(
            {
                "title": title.replace("``", ""),
                "id": base if count == 0 else f"{base}-{count}",
                "level": levels.index(char),
                "start": i + 1,
            }
        )
    for index, section in enumerate(found):
        end = len(lines)
        for later in found[index + 1 :]:
            if later["level"] <= section["level"]:
                end = later["start"] - 2
                break
        section["body"] = "\n".join(lines[section["start"] : end])
    return found


def find_section(root: str, guide: str) -> dict | None:
    """The section a guide reference ``page#anchor`` names, or None."""
    page, _, anchor = guide.partition("#")
    path = os.path.join(root, "docs", page + ".rst")
    if not anchor or not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    for section in sections(text):
        if section["id"] == anchor:
            return section
    return None


def render(manifest: dict, root: str = REPO) -> str:
    rows = manifest["features"]
    counts = {status: sum(1 for r in rows if r["status"] == status) for status in STATUSES}
    out = [
        "Feature Matrix",
        "==============",
        "",
        f"The table lists the {len(rows)} non-experimental features of the MCP SDK feature list.",
        "For each feature it names the API that implements it, the section of the",
        "documentation that explains it, and a compiled example that uses it. The table is",
        "generated from ``docs/feature_matrix.json`` by ``scripts/gen_feature_matrix.py``, and",
        "``scripts/check_feature_matrix.py`` checks that every symbol, section and example it",
        "names exists.",
        "",
        f"{counts['implemented']} features are implemented"
        + (f" and {counts['not-implemented']} are not." if counts["not-implemented"] else "."),
        "",
        ".. list-table::",
        "   :header-rows: 1",
        "   :widths: 4 18 16 10 20 16 16",
        "",
        "   * - #",
        "     - Feature",
        "     - Method",
        "     - Status",
        "     - API",
        "     - Guide",
        "     - Example",
    ]
    for row in rows:
        section = find_section(root, row["guide"])
        page = row["guide"].partition("#")[0]
        title = section["title"] if section else row["guide"].partition("#")[2]
        status = "Implemented" if row["status"] == "implemented" else "Not implemented"
        if row["status"] != "implemented":
            status += f": {row['reason']}"
        method = f"``{row['method']}``" if row["method"] else ""
        api = ", ".join(f"``{symbol}``" for symbol in row["api"])
        example = row["example"]["file"] if row["example"] else ""
        out += [
            f"   * - {row['id']}",
            f"     - {row['name']}",
            f"     - {method}",
            f"     - {status}",
            f"     - {api}",
            f"     - :doc:`{page}`, {title}",
            f"     - ``{example}``" if example else "     -",
        ]
    return "\n".join(line.rstrip() for line in out) + "\n"


def main() -> int:
    manifest = load_manifest()
    with open(os.path.join(REPO, PAGE), "w", encoding="utf-8") as handle:
        handle.write(render(manifest))
    print(f"wrote {PAGE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
