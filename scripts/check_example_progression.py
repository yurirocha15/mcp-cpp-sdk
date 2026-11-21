#!/usr/bin/env python3
"""Check that each sync-mode add-on grows its base program without rewriting it.

examples/sync/progression.json names the base programs and, for each, the add-ons that add one
feature to it. An add-on keeps every line of its base, in order, except the lines the manifest
lists as the ones it replaces, and the manifest lists only lines it does replace. A combined
program ("all") holds every line its add-ons add and every base line none of them replaces.
Blank lines, comment lines and indentation are not compared.

README.md shows the two base programs as its quick start, and its cpp blocks must hold them as
they are.

With --write-diffs DIR, the diff of each add-on against its base is written to
DIR/<add-on stem>.diff, with repo-relative file names and without the files' opening comments,
for the documentation to include.
"""

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXAMPLES = REPO / "examples" / "sync"
MANIFEST = EXAMPLES / "progression.json"
README = REPO / "README.md"
README_PROGRAMS = ("server_base.cpp", "client_base.cpp")


def code_lines(text):
    """The lines that are compared: stripped, without blank and comment-only lines."""
    lines = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("//"):
            lines.append(stripped)
    return lines


def compare(base, add_on):
    """Return (base lines the add-on removes, lines it adds), both in order."""
    matcher = difflib.SequenceMatcher(None, base, add_on, autojunk=False)
    removed = []
    added = []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag in ("delete", "replace"):
            removed.extend(base[i1:i2])
        if tag in ("insert", "replace"):
            added.extend(add_on[j1:j2])
    return removed, added


def check_add_on(name, base, add_on, replaces):
    """Return the problems of one add-on, and the lines it adds."""
    removed, added = compare(base, add_on)
    problems = []
    for line in removed:
        if line not in replaces:
            problems.append(f"{name}: drops or changes the base line `{line}`, which the manifest "
                            "does not list as one it replaces")
    for line in replaces:
        if line not in removed:
            problems.append(f"{name}: the manifest lists `{line}` as replaced, but the add-on "
                            "keeps it or the base has no such line")
    return problems, added


def check_all(name, base, combined, add_ons):
    """Return the problems of a combined program; add_ons maps each to (replaces, added)."""
    replaced = set()
    required = []
    for replaces, added in add_ons.values():
        replaced.update(replaces)
        required.extend(added)
    required.extend(line for line in base if line not in replaced)
    present = set(combined)
    problems = []
    for line in dict.fromkeys(required):
        if line not in present:
            problems.append(f"{name}: lacks the line `{line}`")
    return problems


def check(manifest, read):
    """Check every family of @p manifest, reading files through @p read; return the problems."""
    problems = []
    for base_name, family in manifest.items():
        base = code_lines(read(base_name))
        results = {}
        for add_on_name, replaces in family["add_ons"].items():
            add_on_problems, added = check_add_on(add_on_name, base, code_lines(read(add_on_name)),
                                                  replaces)
            problems.extend(add_on_problems)
            results[add_on_name] = (replaces, added)
        combined = family.get("all")
        if combined:
            unknown = [name for name in combined["combines"] if name not in results]
            problems.extend(f"{combined['file']}: combines {name}, which is not an add-on of "
                            f"{base_name}" for name in unknown)
            problems.extend(check_all(combined["file"], base, code_lines(read(combined["file"])),
                                      {name: results[name] for name in combined["combines"]
                                       if name in results}))
    return problems


def unlisted_problems(manifest, program_names):
    """The programs of @p program_names that the manifest does not name, which go unchecked."""
    listed = set()
    for base_name, family in manifest.items():
        listed.add(base_name)
        listed.update(family["add_ons"])
        if family.get("all"):
            listed.add(family["all"]["file"])
    return [f"{name}: not named in {MANIFEST.name}" for name in sorted(program_names)
            if name not in listed]


def program_names():
    """The file names of the programs in examples/sync."""
    return [path.name for path in EXAMPLES.glob("*.cpp")]


def readme_problems(readme_text, read):
    """The base programs that no cpp block of the README shows as they are."""
    blocks = [code_lines(block) for block in re.findall(r"```cpp\n(.*?)```", readme_text, re.S)]
    return [f"README.md: no cpp block holds {name} as it is" for name in README_PROGRAMS
            if code_lines(read(name)) not in blocks]


def without_opening_comment(text):
    """@p text without the comment block and blank lines it opens with."""
    lines = text.splitlines(keepends=True)
    index = 0
    while index < len(lines) and (not lines[index].strip() or
                                  lines[index].lstrip().startswith("//")):
        index += 1
    return lines[index:]


def write_diffs(manifest, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    for base_name, family in manifest.items():
        base = without_opening_comment((EXAMPLES / base_name).read_text(encoding="utf-8"))
        names = list(family["add_ons"])
        if family.get("all"):
            names.append(family["all"]["file"])
        for name in names:
            add_on = without_opening_comment((EXAMPLES / name).read_text(encoding="utf-8"))
            diff = difflib.unified_diff(base, add_on, fromfile=f"examples/sync/{base_name}",
                                        tofile=f"examples/sync/{name}")
            (out_dir / f"{Path(name).stem}.diff").write_text("".join(diff), encoding="utf-8")


def write_checked_diffs(out_dir):
    """Check the progression and write its diffs to @p out_dir, as the docs build does.

    @throws RuntimeError Listing the problems, when there are any.
    """
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    problems = check(manifest, lambda name: (EXAMPLES / name).read_text(encoding="utf-8"))
    problems += unlisted_problems(manifest, program_names())
    if problems:
        raise RuntimeError("The sync-mode examples do not grow from their bases:\n" +
                           "\n".join(problems))
    write_diffs(manifest, Path(out_dir))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write-diffs", type=Path, default=None,
                        help="Directory to write each add-on's diff against its base to")
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    def read(name):
        return (EXAMPLES / name).read_text(encoding="utf-8")

    problems = (check(manifest, read) + unlisted_problems(manifest, program_names()) +
                readme_problems(README.read_text(encoding="utf-8"), read))
    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} progression problem(s)")
        return 1
    if args.write_diffs:
        write_diffs(manifest, args.write_diffs)
    count = sum(len(family["add_ons"]) + (1 if family.get("all") else 0)
                for family in manifest.values())
    print(f"Checked {count} sync-mode programs against their bases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
