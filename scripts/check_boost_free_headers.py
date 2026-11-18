#!/usr/bin/env python3
"""Check that the public headers an application starts from include no Boost header.

Every header under include/mcp is compiled on its own, with the same compiler and flags as an
example in compile_commands.json, and the headers it pulls in are listed. A header outside
BOOST_HEADERS must compile and must reach no file under a boost/ directory, so that
<mcp/mcp.hpp> and the headers next to it need no Boost in the application's code. Like the
README check, this needs a configured build tree with examples.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from check_readme_snippets import compile_flags, find_build_dir

# Headers that define Boost-based types or are only used by the library itself.
BOOST_HEADERS = frozenset({
    "mcp/core/async.hpp",
    "mcp/core/signal.hpp",
    "mcp/detail/runtime_access.hpp",
    "mcp/detail/serialized_transport_writer.hpp",
    "mcp/detail/signal.hpp",
    "mcp/transport/http_beast.hpp",
    "mcp/transport/http_session_manager.hpp",
    "mcp/transport/websocket.hpp",
})


def included_files(compiler, flags, header, workdir):
    """Compile a file holding only `#include <header>`; return the headers it reached."""
    source = Path(workdir) / "header.cpp"
    depfile = Path(workdir) / "header.d"
    source.write_text(f"#include <{header}>\n", encoding="utf-8")
    command = [compiler, *flags, "-fsyntax-only", "-MD", "-MF", str(depfile), str(source)]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        return None, result.stderr
    text = depfile.read_text(encoding="utf-8").replace("\\\n", " ")
    _, _, deps = text.partition(":")
    return [Path(dep) for dep in deps.split()], ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default=None,
                        help="Build tree with compile_commands.json (default: build/release, "
                             "else the first build/* tree with an example)")
    parser.add_argument("--include-dir", default="include", help="Root of the public headers")
    args = parser.parse_args()

    compiler, flags = compile_flags(find_build_dir(args.build_dir))
    include_dir = Path(args.include_dir)
    headers = sorted(path.relative_to(include_dir).as_posix()
                     for path in include_dir.rglob("*.hpp"))

    failures = 0
    checked = 0
    with tempfile.TemporaryDirectory() as workdir:
        for header in headers:
            if header in BOOST_HEADERS:
                continue
            checked += 1
            deps, error = included_files(compiler, flags, header, workdir)
            if deps is None:
                print(f"{header}: does not compile on its own\n{error}")
                failures += 1
                continue
            boost = [dep for dep in deps if "boost" in dep.parts]
            if boost:
                print(f"{header}: includes Boost, first through {boost[0]}")
                failures += 1

    missing = sorted(name for name in BOOST_HEADERS if not (include_dir / name).is_file())
    for name in missing:
        print(f"{name}: listed in BOOST_HEADERS but not found")
    failures += len(missing)

    print(f"Checked {checked} headers, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
