#!/usr/bin/env python3
"""Set the release version everywhere a release names it, or check that it is set.

``prepare VERSION`` writes ``VERSION``, dates the changelog's Unreleased entries
as the VERSION section, and, for a stable version, updates the documentation
that names the latest release. ``check`` fails when that documentation names
another version than ``VERSION``. Run both from the repository root:

    python3 -m release.version_bump prepare 0.4.0
    python3 -m release.version_bump check
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path
import re
import sys
from typing import Sequence

from .model import SemVer, ValidationError
from .release_notes import release_notes

VERSION_TOKEN = r"[0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[0-9]+)?"
SOVERSION_TOKEN = r"[0-9]+(?:\.[0-9]+\.[0-9]+)?"
UNRELEASED = "## [Unreleased]"
SECTION = re.compile(rf"(?m)^## \[({VERSION_TOKEN})\](?: - ([0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}}))?$")


@dataclass(frozen=True)
class Pin:
    """Text in a file that names the latest stable release."""

    path: str
    before: str
    after: str
    kind: str  # "version" or "soversion"
    count: int

    def pattern(self) -> re.Pattern[str]:
        token = VERSION_TOKEN if self.kind == "version" else SOVERSION_TOKEN
        return re.compile(f"({self.before})({token})({self.after})", re.MULTILINE)


# The runtime package names carry the shared library's SOVERSION (CMakeLists.txt).
PINS = (
    Pin("README.md", r"GIT_TAG v", r"$", "version", 1),
    Pin("docs/getting-started.rst", r"GIT_TAG v", r"$", "version", 1),
    Pin("docs/installation.rst", r"find_package\(mcp-cpp-sdk ", r" EXACT", "version", 1),
    Pin("docs/installation.rst", r"libmcp-cpp-sdk", r"(?=``|$)", "soversion", 2),
    Pin("docs/installation.rst", r"(?<!lib)mcp-cpp-sdk", r"-libs$", "soversion", 1),
    Pin(
        "docs/installation.rst",
        r"/releases/download/v",
        r"/release-signing-key\.asc$",
        "version",
        1,
    ),
    Pin("docs/installation.rst", r"choco install mcp-cpp-sdk --version=", r"$", "version", 1),
    Pin(
        ".github/ISSUE_TEMPLATE/bug_report.yml",
        r"placeholder: v",
        r" or a full commit SHA$",
        "version",
        1,
    ),
)


class VersionBumpError(ValueError):
    """Raised when the repository cannot take or does not carry the version."""


def soversion(version: SemVer) -> str:
    """Return the shared library's SOVERSION: the core version before 1.0, else the major."""

    return version.core if version.major == 0 else str(version.major)


def _expected(pin: Pin, version: SemVer) -> str:
    return str(version) if pin.kind == "version" else soversion(version)


def _read(root: Path, relative: str) -> str:
    try:
        return (root / relative).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise VersionBumpError(f"cannot read {relative}: {error}") from error


def read_version(root: Path) -> SemVer:
    try:
        return SemVer.parse(_read(root, "VERSION").strip())
    except ValidationError as error:
        raise VersionBumpError(f"VERSION is not a release version: {error}") from error


def check_pins(root: Path, version: SemVer) -> list[str]:
    """Return one message per pin that does not name ``version``."""

    problems: list[str] = []
    for pin in PINS:
        matches = [match.group(2) for match in pin.pattern().finditer(_read(root, pin.path))]
        if len(matches) != pin.count:
            problems.append(
                f"{pin.path}: expected {pin.count} match(es) of {pin.pattern().pattern!r}, "
                f"found {len(matches)}"
            )
            continue
        expected = _expected(pin, version)
        problems.extend(
            f"{pin.path}: names {found} where VERSION gives {expected} "
            f"({pin.pattern().pattern!r})"
            for found in matches
            if found != expected
        )
    return problems


def _rewrite_pins(texts: dict[str, str], version: SemVer) -> None:
    for pin in PINS:
        text = texts.setdefault(pin.path, "")
        replacement = _expected(pin, version)
        texts[pin.path], count = pin.pattern().subn(
            lambda match: f"{match.group(1)}{replacement}{match.group(3)}", text
        )
        if count != pin.count:
            raise VersionBumpError(
                f"{pin.path}: expected {pin.count} match(es) of {pin.pattern().pattern!r}, "
                f"found {count}"
            )


def cut_changelog(text: str, version: SemVer, release_date: date) -> str:
    """Turn the Unreleased entries into the dated ``version`` section."""

    if text.count(f"\n{UNRELEASED}\n") != 1:
        raise VersionBumpError("CHANGELOG.md must contain exactly one '## [Unreleased]' heading")
    start = text.index(f"\n{UNRELEASED}\n") + 1
    body_start = start + len(UNRELEASED) + 1
    if any(match.group(1) == str(version) for match in SECTION.finditer(text)):
        raise VersionBumpError(f"CHANGELOG.md already has a section for {version}")
    heading = f"## [{version}] - {release_date.isoformat()}"
    cut = f"{text[:start]}{UNRELEASED}\n\n{heading}\n{text[body_start:]}"
    try:
        # The release workflow writes the GitHub Release notes from the section's highlights.
        release_notes(cut, version, repository="owner/repository")
    except ValidationError as error:
        raise VersionBumpError(
            f"{error}: add it to the Unreleased entries, a few lines for the release page"
        ) from error
    return cut


def _ordering(version: SemVer) -> tuple[int, int, int, int]:
    # A release candidate sorts before the final release of the same core version.
    rc = version.rc if version.rc is not None else sys.maxsize
    return (version.major, version.minor, version.patch, rc)


def prepare(root: Path, version: SemVer, release_date: date) -> list[str]:
    """Write the release version and return the paths it changed."""

    current = read_version(root)
    if _ordering(version) <= _ordering(current):
        raise VersionBumpError(f"{version} does not follow the current VERSION {current}")
    texts = {
        "VERSION": f"{version}\n",
        "CHANGELOG.md": cut_changelog(_read(root, "CHANGELOG.md"), version, release_date),
    }
    if not version.is_prerelease:
        # The documentation names the latest stable release, so a candidate leaves it as is.
        for pin in PINS:
            texts.setdefault(pin.path, _read(root, pin.path))
        _rewrite_pins(texts, version)
    for relative, text in texts.items():
        (root / relative).write_text(text, encoding="utf-8")
    return sorted(texts)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--root", type=Path, default=Path("."), help="repository root")
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_command = commands.add_parser("prepare", help="set a new release version")
    prepare_command.add_argument("version", help="the new version, such as 0.4.0 or 0.4.0-rc.1")
    prepare_command.add_argument(
        "--date",
        type=date.fromisoformat,
        default=None,
        help="the changelog date (default: today in UTC)",
    )
    commands.add_parser("check", help="check that the documentation names VERSION")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "prepare":
            version = SemVer.parse(args.version)
            release_date = args.date or datetime.now(timezone.utc).date()
            for path in prepare(args.root, version, release_date):
                print(f"updated {path}")
        else:
            version = read_version(args.root)
            if version.is_prerelease:
                print(f"VERSION {version} is a release candidate; the documentation keeps the last stable release")
                return 0
            problems = check_pins(args.root, version)
            for problem in problems:
                print(f"version-bump: {problem}", file=sys.stderr)
            if problems:
                print(
                    f"version-bump: run 'python3 -m release.version_bump prepare' "
                    f"or fix the text above to name {version}",
                    file=sys.stderr,
                )
                return 1
            print(f"every release pin names {version}")
    except (ValidationError, VersionBumpError) as error:
        raise SystemExit(f"version-bump: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
