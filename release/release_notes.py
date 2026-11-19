#!/usr/bin/env python3
"""Write the GitHub Release notes of a version from its changelog section.

A release section of CHANGELOG.md opens with a ``### Highlights`` part: the
few entries a reader of the release page needs. The notes are those
highlights, a pointer to the section's migration notes when it has them, and
links to the full section and the installation guide at the tag.
"""

from __future__ import annotations

import re

from .model import SemVer, ValidationError

# GitHub refuses release notes longer than this.
MAX_NOTES_CHARACTERS = 125_000
HIGHLIGHTS = "### Highlights"


def github_anchor(heading: str) -> str:
    """Return the fragment GitHub gives a Markdown heading's text."""

    return re.sub(r"[^\w\- ]", "", heading.strip().lower()).replace(" ", "-")


def changelog_section(changelog: str, version: SemVer) -> tuple[str, str]:
    """Return the heading text and the body of the dated section for ``version``."""

    heading = re.search(
        rf"(?m)^## (\[{re.escape(str(version))}\] - [0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}})$", changelog
    )
    if heading is None:
        raise ValidationError(f"CHANGELOG.md has no dated section for {version}")
    following = re.search(r"(?m)^## ", changelog[heading.end() :])
    end = heading.end() + following.start() if following else len(changelog)
    return heading.group(1), changelog[heading.end() : end]


def _parts(body: str) -> dict[str, str]:
    """Split a section body into its ``### `` parts, keyed by heading text."""

    parts: dict[str, str] = {}
    matches = list(re.finditer(r"(?m)^### (.+)$", body))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(body)
        parts[match.group(1).strip()] = body[match.end() : end].strip()
    return parts


def release_notes(changelog: str, version: SemVer, *, repository: str) -> str:
    """Return the release notes of ``version`` from the text of CHANGELOG.md."""

    heading, body = changelog_section(changelog, version)
    parts = _parts(body)
    highlights = parts.get(HIGHLIGHTS.removeprefix("### "), "")
    if not highlights:
        raise ValidationError(
            f"the CHANGELOG.md section for {version} needs a '{HIGHLIGHTS}' part for the release notes"
        )
    tree = f"https://github.com/{repository}/blob/{version.tag}"
    lines = ["## Highlights", "", highlights, ""]
    for name in parts:
        previous = re.fullmatch(r"Migrating from (\S+)", name)
        if previous:
            lines += [
                f"## Upgrading from {previous.group(1)}",
                "",
                f"Code written for {previous.group(1)} needs changes: see "
                f"[{name}]({tree}/CHANGELOG.md#{github_anchor(name)}).",
                "",
            ]
    lines += [
        "## More",
        "",
        f"- [Full {version} changelog]({tree}/CHANGELOG.md#{github_anchor(heading)})",
        f"- [Installation]({tree}/docs/installation.rst), including how to check the "
        "signatures of the release assets",
        "",
    ]
    notes = "\n".join(lines)
    if len(notes) > MAX_NOTES_CHARACTERS:
        raise ValidationError(
            f"the release notes of {version} exceed GitHub's {MAX_NOTES_CHARACTERS} characters"
        )
    return notes
