#!/usr/bin/env python3
"""
Compile the complete C++ snippets embedded in README.md.

The README is the first thing a new user copies, so it is compiled the same way
a user would compile it: each fenced ``cpp`` block that contains ``int main(``
is written out as a standalone translation unit and fed to the compiler that
built the SDK, with the SDK's own include paths and language standard.

Fenced blocks without ``int main(`` are fragments that only make sense inside a
larger program, so they are reported as skipped rather than compiled.

Compiler flags are taken from ``compile_commands.json`` in the build directory,
so this script needs a configured build tree. With no --build-dir it looks for
one under build/, preferring build/release, which is what scripts/build.py
produces.
"""
import argparse
import json
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

FENCE_RE = re.compile(r"^```cpp\s*$(.*?)^```\s*$", re.MULTILINE | re.DOTALL)

# Flags that carry the information a snippet needs: where the headers are, which
# language standard applies, and which macros the SDK's own headers expect.
PASSTHROUGH_PREFIXES = ("-I", "-D", "-std=", "-m")
PASSTHROUGH_PAIRS = ("-isystem", "-include", "-imacros")


def extract_snippets(readme_path):
    text = readme_path.read_text(encoding="utf-8")
    snippets = []
    for match in FENCE_RE.finditer(text):
        body = match.group(1)
        line_no = text.count("\n", 0, match.start()) + 1
        snippets.append((line_no, body))
    return snippets


def has_example_entry(database):
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return any("/examples/" in item["file"].replace("\\", "/") for item in entries)


def find_build_dir(explicit):
    if explicit:
        return Path(explicit)
    preferred = Path("build/release")
    candidates = [preferred / "compile_commands.json"]
    candidates += sorted(Path("build").glob("*/compile_commands.json"))
    for database in candidates:
        if database.is_file() and has_example_entry(database):
            return database.parent
    return preferred


def compile_flags(build_dir):
    database = Path(build_dir) / "compile_commands.json"
    if not database.is_file():
        raise FileNotFoundError(
            f"{database} not found. Configure a build first, "
            f"for example: python scripts/build.py --examples"
        )

    entries = json.loads(database.read_text(encoding="utf-8"))
    # Only an example's compile line will do. A snippet is consumer code, and the
    # library's own translation units are compiled with export-side macros that a
    # consumer must never see.
    entry = next(
        (item for item in entries if "/examples/" in item["file"].replace("\\", "/")),
        None,
    )
    if entry is None:
        raise RuntimeError(
            f"{database} holds no compile command for an example, so no consumer-side "
            f"flags are available. Build with examples first, "
            f"for example: python scripts/build.py --examples"
        )

    argv = entry.get("arguments") or shlex.split(entry["command"])
    compiler = argv[0]

    flags = []
    index = 1
    while index < len(argv):
        arg = argv[index]
        if arg in PASSTHROUGH_PAIRS and index + 1 < len(argv):
            flags.extend([arg, argv[index + 1]])
            index += 2
            continue
        if arg.startswith(PASSTHROUGH_PREFIXES):
            flags.append(arg)
        index += 1

    return compiler, flags


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=None,
                        help="Configured build directory holding compile_commands.json "
                             "(default: build/release, or the first build/* that has one)")
    parser.add_argument("--readme", default="README.md", help="Markdown file to check")
    args = parser.parse_args()

    readme_path = Path(args.readme)
    snippets = extract_snippets(readme_path)
    if not snippets:
        print(f"No cpp snippets found in {readme_path}")
        return 1

    build_dir = find_build_dir(args.build_dir)
    try:
        compiler, flags = compile_flags(build_dir)
    except (FileNotFoundError, RuntimeError) as error:
        print(f"error: {error}")
        return 1
    print(f"Build directory: {build_dir}")
    print(f"Compiler: {compiler}")
    print(f"Flags: {' '.join(flags)}\n")

    compiled = 0
    skipped = 0
    failed = 0

    with tempfile.TemporaryDirectory() as tmp_dir:
        for line_no, body in snippets:
            label = f"{readme_path}:{line_no}"
            if "int main(" not in body:
                print(f"{label}: SKIP (fragment, no main)")
                skipped += 1
                continue

            source = Path(tmp_dir) / f"readme_{line_no}.cpp"
            source.write_text(body, encoding="utf-8")

            print(f"{label}: compiling...", end=" ", flush=True)
            result = subprocess.run(
                [compiler, "-fsyntax-only", *flags, str(source)],
                capture_output=True,
                text=True,
            )
            if result.returncode == 0:
                print("PASS")
                compiled += 1
            else:
                print("FAIL")
                print("--- snippet ---")
                print(body.rstrip())
                if result.stdout:
                    print("--- stdout ---")
                    print(result.stdout.rstrip())
                if result.stderr:
                    print("--- stderr ---")
                    print(result.stderr.rstrip())
                failed += 1

    print(f"\n{'=' * 50}")
    print(f"Results: {compiled} compiled, {skipped} skipped, {failed} failed")

    if failed:
        print("\nSome README snippets do not compile!")
        return 1
    if compiled == 0:
        print("\nNo complete README snippet was compiled; the check proved nothing.")
        return 1
    print("\nAll README snippets compiled.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
