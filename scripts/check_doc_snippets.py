#!/usr/bin/env python3
"""
Compile the complete C++ snippets embedded in the reStructuredText docs.

Each ``.. code-block:: cpp`` block under docs/ that contains ``int main(`` is
compiled exactly the way scripts/check_readme_snippets.py compiles README
blocks: written out as a standalone translation unit and fed to the compiler
that built the SDK, with the SDK's own include paths and language standard.

Blocks without ``int main(`` are fragments, so they are reported as skipped.
Blocks that pull code in from a file (``.. literalinclude::``) are not
``code-block`` directives and are not looked at.

Like the README check, this needs a configured build tree with examples.
"""
import argparse
import re
import sys
from pathlib import Path

from check_readme_snippets import compile_flags, compile_snippets, find_build_dir

DIRECTIVE_RE = re.compile(r"^(?P<indent>[ \t]*)\.\.\s+code-block::\s*cpp\s*$")
OPTION_RE = re.compile(r"^\s*:[^:\s][^:]*:")


def _indent_of(line):
    return len(line) - len(line.lstrip(" \t"))


def snippets_in_text(text):
    """Return (line_no, body) for each cpp code-block; line_no is the directive's line."""
    lines = text.expandtabs(8).split("\n")
    snippets = []
    index = 0
    while index < len(lines):
        match = DIRECTIVE_RE.match(lines[index])
        if not match:
            index += 1
            continue
        directive_line = index + 1
        base = len(match.group("indent"))
        index += 1

        # Options sit directly under the directive, indented deeper than it.
        while (index < len(lines) and lines[index].strip()
               and _indent_of(lines[index]) > base and OPTION_RE.match(lines[index])):
            index += 1

        # The body is the indented text that follows, up to the first non-blank
        # line that is not indented deeper than the directive.
        body = []
        while index < len(lines):
            line = lines[index]
            if line.strip() and _indent_of(line) <= base:
                break
            body.append(line)
            index += 1

        indents = [_indent_of(line) for line in body if line.strip()]
        if indents:
            strip = min(indents)
            body = [line[strip:] if line.strip() else "" for line in body]
        while body and not body[0]:
            body.pop(0)
        while body and not body[-1]:
            body.pop()
        snippets.append((directive_line, "\n".join(body) + "\n"))
    return snippets


def extract_snippets(rst_path):
    """Return snippets_in_text() for an rst file."""
    return snippets_in_text(Path(rst_path).read_text(encoding="utf-8"))


def collect(docs_dir):
    """Return [(label, line_no, body)] for every rst file under docs_dir."""
    found = []
    for path in sorted(Path(docs_dir).rglob("*.rst")):
        for line_no, body in extract_snippets(path):
            found.append((path, line_no, body))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=None,
                        help="Configured build directory holding compile_commands.json "
                             "(default: build/release, or the first build/* that has one)")
    parser.add_argument("--docs-dir", default="docs", help="Directory searched for *.rst files")
    args = parser.parse_args()

    found = collect(args.docs_dir)
    if not found:
        print(f"No cpp code-blocks found under {args.docs_dir}")
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

    snippets = [(f"{path}:{line_no}", body) for path, line_no, body in found]
    return compile_snippets(snippets, compiler, flags, None, "documentation")


if __name__ == "__main__":
    sys.exit(main())
