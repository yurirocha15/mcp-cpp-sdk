#!/usr/bin/env python3
"""Produce ``third_party/jsoncons`` from a pinned jsoncons release.

The SDK compiles jsoncons's JSON Schema extension into one private translation
unit. The vendored tree is never edited by hand: this script downloads the
pinned release tarball, verifies its SHA-256, copies the headers the extension
needs and applies three recorded transformations:

1. ``std::regex`` is replaced with ``mcp::detail::Regex``. libstdc++'s
   ``std::regex`` recurses once per input character, so a schema ``pattern``
   matched against a long string overflows the stack; ``mcp::detail::Regex``
   runs in linear time without recursion.
2. ``eval_context``, which jsoncons creates for every keyword it evaluates,
   enforces a limit on the evaluation depth and on the evaluation work in one
   validation, where a step costs one more than the length of its evaluation
   path. Without them a reference cycle that never descends into the value
   (``{"$ref": "#"}``) recurses until the stack overflows, and branching
   applicators over recursive references take time exponential in the value's
   depth.
3. The ``jsoncons`` namespace becomes ``mcp_detail_jsoncons`` and the
   ``JSONCONS_`` macro prefix becomes ``MCP_DETAIL_JSONCONS_``, so the copy
   compiled into the SDK (a static archive included) cannot collide with a
   jsoncons the application uses itself.

Usage:
  python3 scripts/vendor_jsoncons.py            regenerate third_party/jsoncons
  python3 scripts/vendor_jsoncons.py --check    fail if the committed tree differs
  --tarball PATH                                use a local copy of the tarball
"""

from __future__ import annotations

import argparse
import hashlib
import io
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OUTPUT_DIR = REPO_ROOT / "third_party" / "jsoncons"

VERSION = "1.10.0"
TARBALL_URL = f"https://github.com/danielaparker/jsoncons/archive/refs/tags/v{VERSION}.tar.gz"
TARBALL_SHA256 = "c829350a2eece662beb378143e2ab1c1cbb07d703687bf168626665a8a7d8bc1"
# Bump when the transformation below changes, so PROVENANCE records which one produced the tree.
TRANSFORM_VERSION = 2

# Header directories, relative to the release's include/, that the extension needs.
SUBSET = ("jsoncons", "jsoncons_ext/jsonschema", "jsoncons_ext/jsonpointer")

# (old, new, expected occurrences across SUBSET). A count that no longer matches means the
# upstream code moved and the replacement must be reviewed, so the script fails instead of
# producing a tree that still calls std::regex somewhere.
REGEX_REPLACEMENTS = (
    ("#include <regex>", "#include <mcp/detail/regex.hpp>", 7),
    (
        "std::vector<std::pair<std::regex, schema_validator_ptr_type>>",
        "std::vector<std::pair<::mcp::detail::Regex, schema_validator_ptr_type>>",
        3,
    ),
    ("std::regex(prop.key(), std::regex::ECMAScript)", "::mcp::detail::Regex(prop.key())", 1),
    (
        "auto regex = std::regex(pattern_string, std::regex::ECMAScript);",
        "auto regex = ::mcp::detail::Regex(pattern_string);",
        1,
    ),
    (
        "std::regex re(sv.data(), sv.size(), std::regex::ECMAScript);",
        "::mcp::detail::Regex re(std::string_view(sv.data(), sv.size()));",
        1,
    ),
    ("std::regex regex_;", "::mcp::detail::Regex regex_;", 1),
    (
        "const std::string& pattern_string, const std::regex& regex)",
        "const std::string& pattern_string, const ::mcp::detail::Regex& regex)",
        1,
    ),
    (
        "if (!std::regex_search(instance.as_cstring(), regex_))",
        "if (!regex_.search(instance.as_string_view()))",
        1,
    ),
    (
        "if (std::regex_search(prop.key(), schema_pp.first))",
        "if (schema_pp.first.search(prop.key()))",
        2,
    ),
)

EVAL_CONTEXT_PATH = "jsoncons_ext/jsonschema/common/eval_context.hpp"

EVALUATION_LIMITS = """// Added by mcp-cpp-sdk (scripts/vendor_jsoncons.py): limits on one validation. A reference
// cycle that never descends into the value makes evaluation recurse without end, and branching
// applicators over recursive references can make the number of evaluation steps exponential in
// the value's depth. eval_context is created for every keyword evaluated, so it enforces both.
// Creating one copies the evaluation path and the dynamic scope, so a step costs work in
// proportion to the path's length, and the budget is charged accordingly.
class evaluation_limit_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct evaluation_limits
{
    // Longest evaluation path, counted in keywords and array indexes.
    static constexpr std::size_t max_depth = 192;

    // Evaluation work left for the validation running on this thread. A step with an evaluation
    // path of n tokens costs n + 1. The caller sets it before each validation; the initial value
    // does not limit anything.
    static std::size_t& work_left()
    {
        static thread_local std::size_t work = static_cast<std::size_t>(-1);
        return work;
    }

    static void enter(std::size_t depth)
    {
        if (depth > max_depth)
        {
            throw evaluation_limit_error("schema evaluation nests too deeply");
        }
        std::size_t& work = work_left();
        if (work <= depth)
        {
            throw evaluation_limit_error("schema evaluation takes too many steps");
        }
        work -= depth + 1;
    }
};

template <typename Json>
class eval_context
{"""

# The four eval_context constructors that extend the evaluation path.
EVAL_PATH_CONSTRUCTOR = re.compile(
    r"(eval_path_\(parent\.eval_path\(\) / (?:name|index)\),\s*flags_\((?:parent\.flags_|flags)\)\s*)\{\s*\}"
)

# `jsoncons` as an identifier, not as part of an include path (`jsoncons/`) or of a longer
# name (`jsoncons_ext`).
NAMESPACE_PATTERN = re.compile(r"(?<![\w/])jsoncons(?![\w/])")
MACRO_PATTERN = re.compile(r"\bJSONCONS_")


def fetch_tarball(path: Path | None) -> bytes:
    if path is not None:
        data = path.read_bytes()
    else:
        with urllib.request.urlopen(TARBALL_URL, timeout=120) as response:  # noqa: S310
            data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != TARBALL_SHA256:
        raise SystemExit(f"jsoncons tarball SHA-256 mismatch: got {digest}, expected {TARBALL_SHA256}")
    return data


def read_release(data: bytes) -> tuple[dict[str, str], str]:
    """Return {path under include/: text} for SUBSET, and the LICENSE text."""
    headers: dict[str, str] = {}
    license_text = ""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            parts = member.name.split("/", 1)
            if len(parts) != 2:
                continue
            relative = parts[1]
            extracted = archive.extractfile(member)
            if extracted is None:
                continue
            if relative == "LICENSE":
                license_text = extracted.read().decode("utf-8")
            elif relative.startswith("include/"):
                inner = relative[len("include/") :]
                if any(inner.startswith(prefix + "/") for prefix in SUBSET):
                    if ".." in Path(inner).parts:
                        raise SystemExit(f"refusing unsafe path in tarball: {member.name}")
                    headers[inner] = extracted.read().decode("utf-8")
    if not license_text:
        raise SystemExit("jsoncons tarball has no LICENSE file")
    return headers, license_text


def transform(headers: dict[str, str]) -> dict[str, str]:
    counts = {old: 0 for old, _, _ in REGEX_REPLACEMENTS}
    result: dict[str, str] = {}
    for path, text in headers.items():
        for old, new, _ in REGEX_REPLACEMENTS:
            counts[old] += text.count(old)
            text = text.replace(old, new)
        if "std::regex" in text:
            raise SystemExit(f"std::regex left in {path} after the replacements")
        if path == EVAL_CONTEXT_PATH:
            text = add_evaluation_limits(text)
        text = NAMESPACE_PATTERN.sub("mcp_detail_jsoncons", text)
        text = MACRO_PATTERN.sub("MCP_DETAIL_JSONCONS_", text)
        result[path] = text
    for old, _, expected in REGEX_REPLACEMENTS:
        if counts[old] != expected:
            raise SystemExit(f"expected {expected} occurrence(s) of {old!r}, found {counts[old]}")
    return result


def add_evaluation_limits(text: str) -> str:
    anchor = "template <typename Json>\nclass eval_context\n{"
    if text.count(anchor) != 1 or text.count("#include <vector>\n") != 1:
        raise SystemExit(f"{EVAL_CONTEXT_PATH} changed upstream; review the evaluation limits patch")
    text = text.replace("#include <vector>\n", "#include <stdexcept>\n#include <vector>\n")
    text = text.replace(anchor, EVALUATION_LIMITS)
    text, count = EVAL_PATH_CONSTRUCTOR.subn(
        lambda match: match.group(1) + "{\n        evaluation_limits::enter(eval_path_.tokens().size());\n    }",
        text,
    )
    if count != 4:
        raise SystemExit(f"expected 4 eval_context constructors that extend the path, found {count}")
    return text


def provenance() -> str:
    return (
        f"jsoncons {VERSION}\n"
        f"source: {TARBALL_URL}\n"
        f"sha256: {TARBALL_SHA256}\n"
        f"subset: {', '.join(SUBSET)}\n"
        f"transform: {TRANSFORM_VERSION} (scripts/vendor_jsoncons.py: std::regex replaced with "
        "mcp::detail::Regex; evaluation depth and work limits added to eval_context; namespace "
        "jsoncons renamed mcp_detail_jsoncons; macro prefix JSONCONS_ renamed MCP_DETAIL_JSONCONS_)\n"
    )


def write_tree(root: Path, headers: dict[str, str], license_text: str) -> None:
    for path, text in sorted(headers.items()):
        target = root / "include" / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")
    (root / "LICENSE").write_text(license_text, encoding="utf-8", newline="\n")
    (root / "PROVENANCE").write_text(provenance(), encoding="utf-8", newline="\n")


def tree_files(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="compare instead of writing")
    parser.add_argument("--tarball", type=Path, help="local copy of the pinned release tarball")
    args = parser.parse_args()

    headers, license_text = read_release(fetch_tarball(args.tarball))
    transformed = transform(headers)

    if not args.check:
        readme = OUTPUT_DIR / "README.md"
        readme_text = readme.read_bytes() if readme.exists() else None
        if OUTPUT_DIR.exists():
            shutil.rmtree(OUTPUT_DIR)
        write_tree(OUTPUT_DIR, transformed, license_text)
        if readme_text is not None:
            readme.write_bytes(readme_text)
        print(f"wrote {len(transformed)} headers to {OUTPUT_DIR.relative_to(REPO_ROOT)}")
        return 0

    with tempfile.TemporaryDirectory() as scratch:
        expected_root = Path(scratch)
        write_tree(expected_root, transformed, license_text)
        expected = tree_files(expected_root)
    actual = tree_files(OUTPUT_DIR) if OUTPUT_DIR.exists() else {}
    # README.md is written by hand next to the generated files.
    actual.pop("README.md", None)
    problems = sorted(
        [f"missing: {name}" for name in expected.keys() - actual.keys()]
        + [f"unexpected: {name}" for name in actual.keys() - expected.keys()]
        + [f"differs: {name}" for name in expected.keys() & actual.keys() if expected[name] != actual[name]]
    )
    spec = (REPO_ROOT / "packaging" / "rpm" / "mcp-cpp-sdk.spec.in").read_text(encoding="utf-8")
    if f"Provides:       bundled(jsoncons) = {VERSION}\n" not in spec:
        problems.append(f"packaging/rpm/mcp-cpp-sdk.spec.in does not declare bundled(jsoncons) = {VERSION}")
    if problems:
        print("third_party/jsoncons does not match the pinned release plus the recorded transformation:")
        for problem in problems:
            print(f"  {problem}")
        print("Regenerate it with: python3 scripts/vendor_jsoncons.py")
        return 1
    print(f"third_party/jsoncons matches jsoncons {VERSION} plus transform {TRANSFORM_VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
