#!/usr/bin/env python3
"""Generate the peer-input test matrix from the census.

For every type a peer can steer a document into, for every field of that type,
the matrix exercises four modes:

    absent      the key is not in the document
    null        the key is present and explicitly null
    wrong_type  the key holds a value of the wrong JSON type
    oversized   the key holds a very large value of the right JSON type

Each case asserts what ``scripts/json_census.py`` predicts for that field and
mode.  The suite is therefore a differential oracle rather than a restatement:
where the code and the census disagree, one of them is wrong and the build says
so.  A hand-written suite of the same size asserts only what its author already
believed, which is how the same defect class survived three reviews.

The generated file also pins the matrix's type list.  ``check_json_matrix.py``
compares that list against the types with a ``from_json`` under
``include/mcp/protocol/`` and fails the build when a new protocol type has not
been added, so the matrix cannot silently fall behind the protocol.

Regenerating cannot launder a regression either.  When a row would move from
tolerating an explicit null or an absent key to throwing on it, nothing is
written unless that row is named with ``--accept-regression Type.key`` (or a
bare ``Type``), and the reason belongs in the commit message.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json_census as census  # noqa: E402

PROTOCOL_DIR = os.path.join("include", "mcp", "protocol")
MODES = ("absent", "null", "wrong_type", "oversized")

# Enum serialisers give the census a valid string for an enum-typed field.
ENUM_MACRO = re.compile(
    r"NLOHMANN_JSON_SERIALIZE_ENUM\s*\(\s*([\w:]+)\s*,\s*\{(.*?)\}\s*\)\s*$",
    re.DOTALL | re.MULTILINE,
)

SCALARS = {
    "bool": True,
    "int": 1,
    "int64_t": 1,
    "std::int64_t": 1,
    "uint64_t": 1,
    "size_t": 1,
    "double": 1.0,
    "float": 1.0,
    "std::string": "x",
    "std::string_view": "x",
}

# Types with a hand-written from_json that dispatches on the document's shape
# rather than reading a fixed field set.  A synthesised baseline cannot pick
# the right arm, so they are excluded by name, with the reason recorded here
# and reprinted in the generated file.  check_json_matrix.py counts them as
# covered so the build stays honest about what the matrix does not reach.
UNSYNTHESISABLE = {
    "JSONRPCResponse": "std::variant; from_json dispatches on which key is present",
    "JSONRPCMessage": "std::variant; from_json dispatches on which key is present",
    "ContentBlock": "std::variant; from_json dispatches on the 'type' discriminator",
    "ResourceContents": "std::variant; from_json dispatches on text vs blob",
    "CompleteReference": "std::variant; from_json dispatches on the 'type' discriminator",
    "ElicitRequestParams": "std::variant; from_json dispatches on mode",
    "SamplingMessageContent": "std::variant; from_json accepts a block or a list of blocks",
    "PrimitiveSchemaDefinition": "base of EnumSchema; exercised through its derived type",
    "RequestId": "std::variant of string and integer; it has no fields to vary",
}


def enum_values(text: str) -> dict[str, str]:
    """Enum type -> a valid wire string for it."""
    out: dict[str, str] = {}
    for m in ENUM_MACRO.finditer(text):
        name = m.group(1).split("::")[-1]
        first = re.search(r'"([^"]+)"', m.group(2))
        if first:
            out[name] = first.group(1)
    return out


MEMBER_WITH_DEFAULT = re.compile(
    r"^\s*(?:const\s+)?[\w:]+(?:\s*<[^;]*>)?\s+(\w+)\s*=\s*([^;]+);\s*$",
    re.MULTILINE,
)


def member_defaults(text: str, root) -> dict[tuple[str, str], object]:
    """(type, member) -> the literal the member is declared with.

    A member declared `std::string type = "audio";` is a discriminator, and
    `std::string jsonrpc = "2.0";` is checked by a validator.  Synthesising
    "x" for either produces a baseline the serialiser rejects outright, so
    every mode of every field of that type fails for a reason that has nothing
    to do with the mode.  The declared default is the value the type expects.
    """
    out: dict[tuple[str, str], object] = {}

    def walk(blk) -> None:
        m = census.STRUCT_DECL.search(blk.header + "{")
        if m:
            flat = census.strip_nested(text[blk.start : blk.end])
            for member, literal in MEMBER_WITH_DEFAULT.findall(flat):
                lit = literal.strip()
                if lit.startswith('"') and lit.endswith('"'):
                    out[(m.group(1), member)] = lit[1:-1]
                elif re.fullmatch(r"-?\d+", lit):
                    out[(m.group(1), member)] = int(lit)
                elif lit in ("true", "false"):
                    out[(m.group(1), member)] = lit == "true"
        for child in blk.children:
            walk(child)

    walk(root)
    return out


def load_protocol(base: str):
    """Struct members, enum values, from_json targets, and declared defaults."""
    structs: dict[str, list[tuple[str, str]]] = {}
    bases: dict[str, list[str]] = {}
    enums: dict[str, str] = {}
    defaults: dict[tuple[str, str], object] = {}
    from_json_types: set[str] = set()
    top = os.path.join(base, PROTOCOL_DIR)
    for name in sorted(os.listdir(top)):
        if not name.endswith(".hpp"):
            continue
        with open(os.path.join(top, name), encoding="utf-8") as fh:
            raw = fh.read()
        text = census.strip_comments(raw)
        root = census.parse_blocks(text)
        for k, v in census.parse_structs(text, root).items():
            structs.setdefault(k, v)
        for k, v in census.parse_bases(text, root).items():
            bases.setdefault(k, v)
        enums.update(enum_values(text))
        defaults.update(member_defaults(text, root))
        # Keep the qualification: the census keys a nested capability by
        # `ClientCapabilities::RootsCapability`, and shortening it here left
        # twenty types looking unsynthesisable when they were simply not being
        # looked up under the name they were filed under.
        for m in census.FROM_JSON_SIG.finditer(text):
            from_json_types.add(m.group(2))
        for m in census.MACRO_DEFINE.finditer(text):
            from_json_types.add(m.group(3))
    return structs, bases, enums, defaults, from_json_types


def unwrap(ctype: str) -> tuple[str, str]:
    """Strip one layer of optional/vector/map.  Returns (wrapper, inner)."""
    c = ctype.strip().removeprefix("const ").strip()
    for wrapper in ("std::optional", "std::vector", "std::map", "std::unordered_map"):
        m = re.match(rf"{re.escape(wrapper)}\s*<(.+)>$", c)
        if m:
            return wrapper, m.group(1).strip()
    return "", c


# Set once per run by render(); sample() needs the census view to build a
# nested type's baseline and is called from helpers that do not carry it.
_GOVERNED: dict = {}
_DEFAULTS: dict = {}
_BASES: dict = {}


def inherited_fields(type_name: str, governed, depth: int = 0):
    """This type's fields plus every field its bases contribute."""
    seen = [(k, r) for (t, k), r in governed.items() if t == type_name]
    if depth < 4:
        for parent in _BASES.get(type_name.split("::")[-1], []):
            have = {k for k, _ in seen}
            seen += [
                (k, r) for k, r in inherited_fields(parent, governed, depth + 1) if k not in have
            ]
    return seen


def sample(ctype: str, structs, enums, depth: int = 0):
    """A JSON value that deserialises into ``ctype``, or None if unreachable."""
    return sample_for(ctype, structs, enums, _GOVERNED, depth)


def field_type(type_name: str, row, structs) -> str:
    """The declared C++ type behind a wire key."""
    if row.member:
        for ctype, mname in structs.get(type_name.split("::")[-1], []):
            if mname == row.member:
                return ctype
    return ",".join(row.target_types)


def baseline(type_name: str, structs, enums, governed, depth: int = 0):
    """A minimal document that deserialises into ``type_name``.

    Keys come from the census, not from member names.  They differ: `Icon`
    reads its `source` member from `"src"`.  A baseline built from member
    names omitted that key, and every case for the type then threw on a
    missing `"src"` rather than on the mode it was meant to exercise -- 17
    cases that looked like findings and were nothing but a bad fixture.
    """
    if depth > 4:
        return None
    keys = inherited_fields(type_name, governed)
    if not keys:
        return None
    doc: dict = {}
    for key, row in keys:
        if row.absent != census.THROW:
            continue
        ctype = field_type(row.owner, row, structs)
        value = _DEFAULTS.get((row.owner.split("::")[-1], row.member))
        if value is None:
            value = sample_for(ctype, structs, enums, governed, depth + 1)
        if value is None:
            return None
        doc[key] = value
    return doc


def sample_for(ctype: str, structs, enums, governed, depth: int = 0):
    """Like ``sample``, but recursing through census-derived baselines."""
    if depth > 4 or not ctype:
        return None
    wrapper, inner = unwrap(ctype)
    if wrapper == "std::optional":
        return sample_for(inner, structs, enums, governed, depth + 1)
    if wrapper == "std::vector":
        item = sample_for(inner, structs, enums, governed, depth + 1)
        return [] if item is None else [item]
    if wrapper in ("std::map", "std::unordered_map"):
        return {}
    bare = inner.split("::")[-1]
    if inner in SCALARS:
        return SCALARS[inner]
    if bare in SCALARS:
        return SCALARS[bare]
    if "nlohmann::json" in inner or inner == "json":
        return {}
    if bare in enums:
        return enums[bare]
    if bare in ("RequestId", "ProgressToken"):
        return 1
    return baseline(bare, structs, enums, governed, depth + 1)


# Mutations are applied in C++, not baked into the file.  Spelling out a
# 64 KiB string and a 512-element array for each of 1268 cases produced a 17 MB
# source file -- a generated suite has to stay readable and reviewable, or it
# is just a binary blob that happens to compile.  The generator emits the
# baseline and a per-field hint; the mutation helpers in the test do the rest.
WRONG_FROM_VALUE = 0  # derive the counter-example from the value's JSON type
WRONG_FORCE_OBJECT = 1  # for variant fields that accept both string and number


def wrong_hint(ctype: str) -> int:
    """How the test should pick a value the field does not accept.

    A ``RequestId`` is a variant of string and integer, so "the other scalar
    type" is still a value it accepts.  Deriving the counter-example from the
    sample would generate a case that passes for the wrong reason.
    """
    _, inner = unwrap(ctype)
    if inner.split("::")[-1] in ("RequestId", "ProgressToken"):
        return WRONG_FORCE_OBJECT
    return WRONG_FROM_VALUE


def field_rows(rows):
    """(type, field) -> the merged verdict for that field.

    A field can be touched by several sites -- a validator that rejects a
    shape, then the accessor that reads it.  The field's verdict is the
    strictest of them: if any site throws on a null, the peer sending a null
    gets a throw.  Taking the first row instead let a permissive accessor mask
    a validator two lines above it.
    """
    out: dict[tuple[str, str], census.Row] = {}
    for r in rows:
        if r.site_kind in ("delegate", "envelope_presence"):
            continue
        if not r.owner or r.owner.startswith("(") or r.field_name.startswith("<"):
            continue
        key = (r.owner, r.field_name)
        prev = out.get(key)
        if prev is None:
            out[key] = r
            continue
        for axis in ("absent", "null", "wrong_type"):
            if getattr(r, axis) == census.THROW:
                setattr(prev, axis, census.THROW)
        if prev.constrained == census.NA and r.constrained != census.NA:
            prev.constrained = r.constrained
        if not prev.member and r.member:
            prev.member = r.member
        if not prev.target_types and r.target_types:
            prev.target_types = r.target_types
    return out


def predict(row: census.Row, mode: str) -> bool:
    """Does the census say this mode throws?"""
    if mode == "absent":
        return row.absent == census.THROW
    if mode == "null":
        return row.null == census.THROW
    if mode == "wrong_type":
        return row.wrong_type == census.THROW
    # Oversized input is accepted -- no site in this codebase bounds a value's
    # size -- unless the field has a value domain, in which case an arbitrary
    # value of the right JSON type is outside it.  That is a domain rejection,
    # not a size limit, and the two must not be confused: reading it as a size
    # limit would report a ceiling that does not exist.
    return row.constrained != census.NA


def cpp_string(doc) -> str:
    return json.dumps(doc, separators=(",", ":"))


HEADER = '''// Generated by scripts/gen_json_matrix.py from scripts/json_census.py.
// Do not edit by hand: regenerate with `python3 scripts/gen_json_matrix.py`.
//
// Every peer-deserialisable protocol type, every field, four modes: the key
// absent, the key present and explicitly null, the key holding the wrong JSON
// type, and the key holding an oversized value of the right type.
//
// Each case asserts what the census predicts, so a disagreement between the
// code and the census fails here rather than in a user\'s session. Serialising
// an absent optional as an explicit null is the default for Go\'s
// encoding/json without omitempty and for a naively dumped Python dataclass,
// which puts the null column within reach of a careless peer rather than only
// a hostile one.
//
// scripts/check_json_matrix.py runs as a build step. It fails when a protocol
// type has a from_json but is neither in this matrix nor excluded on the
// record, so the matrix cannot quietly fall behind the protocol, and when this
// file differs from what the generator produces from the current sources.
//
// WHAT THIS CORPUS IS, AND WHAT IT IS NOT
//
// It is a consistency oracle, not a correctness one. Every expectation is
// derived by static analysis of the guard construct at the decode site, and
// the test then runs the decoder. A case fails when runtime and static
// analysis disagree. It cannot fail because a field is wrong per the MCP
// spec. Nothing here is produced by executing the SDK, so the two sides are
// independent -- but agreement means consistency, not conformance.
//
// It follows that once a decode site is fixed and this file is regenerated,
// the matching cases pass BY CONSTRUCTION: the census re-reads the fixed
// source and predicts the new behaviour. Green here is not evidence that a
// field decodes correctly. That evidence lives in the hand-written,
// red-first tests in test/core/protocol_test.cpp and
// test/server/server_handlers_test.cpp.
//
// A TRUE null_throws ON AN OPTIONAL MEMBER RECORDS A DEFECT, NOT A SPEC
//
// The tuple (absent=false, null=true, wrong=true, oversized=false) describes
// a member that tolerates being absent but throws when a peer sends it as an
// explicit null. That is the defect class this project has repeatedly been
// caught by, and scripts/json_census_dispositions.json dispositions it "fix".
// {null_fragile} of the {field_total} field entries below still carry it; there were 87 before the
// explicit-null decoding fixes. Those rows pin behaviour as it is so the
// suite stays green. They do not endorse it. Fixing one makes the build check
// report this file stale until it is regenerated with the fix. The check reads
// the other direction as a REGRESSION: a row whose decoder now throws on an
// explicit null or on an absent key, where this file says it tolerates it.
// The generator refuses to write such a row unless it is named with
// --accept-regression. Never relax a fix to satisfy this file.
//
// This column is also blind to the other half of that class. A member that
// decodes an explicit null into an ENGAGED optional throws nothing, so it is
// recorded as null_throws = false whether or not it then re-encodes a member
// the peer never sent. _meta and annotations were corrupt in exactly that way
// and not one case below changed when they were fixed. Seeing that class
// needs a round-trip check, which this corpus does not have.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <mcp/protocol/protocol.hpp>

#include <string>

namespace {

using nlohmann::json;

// How to pick a value a field does not accept. A variant of string and integer
// needs an object: "the other scalar type" is still something it accepts.
enum WrongHint { kWrongFromValue = 0, kWrongForceObject = 1 };

json wrong_typed(const json& value, int hint) {
    if (hint == kWrongForceObject) {
        return json{{"neither", "a string nor an integer"}};
    }
    if (value.is_boolean()) {
        return "not-a-bool";
    }
    if (value.is_number()) {
        return "not-a-number";
    }
    if (value.is_string()) {
        return 12345;
    }
    if (value.is_array()) {
        return json{{"not", "an array"}};
    }
    return json::array({"not an object"});
}

// A very large value that still has the type the field accepts. An oversized
// array stays an array *of its own element type*: filling it with strings
// would make it a wrong-type case wearing an oversized label, and the throw
// would then be read as a size limit that does not exist.
json oversized(const json& value) {
    if (value.is_boolean()) {
        return value;
    }
    if (value.is_number()) {
        return json(1000000000000000000LL);
    }
    if (value.is_string()) {
        return std::string(65536, \'A\');
    }
    if (value.is_array()) {
        if (value.empty()) {
            return value;
        }
        json grown = json::array();
        for (int i = 0; i < 512; ++i) {
            grown.push_back(value.front());
        }
        return grown;
    }
    // An object grows by keys the serialiser ignores, so its shape is intact.
    json grown = value.is_object() ? value : json::object();
    for (int i = 0; i < 256; ++i) {
        grown["_pad" + std::to_string(i)] = std::string(256, \'A\');
    }
    return grown;
}

// Parses `doc` into T and reports whether that threw.
template <typename T>
bool throws_on(const json& doc) {
    try {
        static_cast<void>(doc.get<T>());
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

struct FieldExpect {
    const char* key;
    const char* present;  // a value the field accepts, as JSON text
    int wrong_hint;
    bool absent_throws;
    bool null_throws;
    bool wrong_throws;
    bool oversized_throws;
};

// Applies all four modes to one field and checks each against the census.
template <typename T>
void check_field(const json& baseline, const FieldExpect& f) {
    const json present = json::parse(f.present);

    json absent = baseline;
    absent.erase(f.key);
    {
        SCOPED_TRACE("absent");
        EXPECT_EQ(throws_on<T>(absent), f.absent_throws);
    }
    json with_null = baseline;
    with_null[f.key] = nullptr;
    {
        SCOPED_TRACE("null");
        EXPECT_EQ(throws_on<T>(with_null), f.null_throws);
    }
    json with_wrong = baseline;
    with_wrong[f.key] = wrong_typed(present, f.wrong_hint);
    {
        SCOPED_TRACE("wrong_type");
        EXPECT_EQ(throws_on<T>(with_wrong), f.wrong_throws);
    }
    json with_big = baseline;
    with_big[f.key] = oversized(present);
    {
        SCOPED_TRACE("oversized");
        EXPECT_EQ(throws_on<T>(with_big), f.oversized_throws);
    }
}

}  // namespace
'''


def render(base: str) -> tuple[str, dict]:
    """The matrix source and its manifest, unformatted and without writing."""
    rows = census.build_rows(base)

    structs, bases, enums, defaults, from_json_types = load_protocol(base)
    global _DEFAULTS, _BASES
    _DEFAULTS = defaults
    _BASES = bases
    governed = field_rows(rows)
    global _GOVERNED
    _GOVERNED = governed

    covered: list[tuple[str, dict, list]] = []
    skipped: dict[str, str] = dict(UNSYNTHESISABLE)

    for type_name in sorted(from_json_types):
        if type_name in skipped:
            continue
        base_doc = baseline(type_name, structs, enums, governed)
        if base_doc is None:
            skipped[type_name] = "no baseline document could be synthesised"
            continue
        cases = []
        for key, row in sorted(inherited_fields(type_name, governed)):
            ctype = field_type(row.owner, row, structs)
            present = _DEFAULTS.get((row.owner.split("::")[-1], row.member))
            if present is None:
                present = sample_for(ctype, structs, enums, governed)
            if present is None:
                continue
            cases.append(
                (
                    key,
                    cpp_string(present),
                    wrong_hint(ctype),
                    predict(row, "absent"),
                    predict(row, "null"),
                    predict(row, "wrong_type"),
                    predict(row, "oversized"),
                )
            )
        if not cases:
            skipped[type_name] = "no census row governs any of its fields"
            continue
        covered.append((type_name, base_doc, cases))

    null_fragile = sum(1 for _, _, c in covered for case in c if not case[3] and case[4])
    field_total = sum(len(c) for _, _, c in covered)
    body = [
        HEADER.replace("{null_fragile}", str(null_fragile)).replace(
            "{field_total}", str(field_total)
        )
    ]
    for type_name, base_doc, cases in covered:
        test_name = type_name.replace("::", "_")
        body.append(f"\nTEST(JsonPeerInputMatrix, {test_name}) {{")
        body.append(
            f'    const json baseline = json::parse(R"json({cpp_string(base_doc)})json");'
        )
        body.append("    static const FieldExpect fields[] = {")
        for key, present, hint, a, n, w, o in cases:
            body.append(
                f'        {{"{key}", R"json({present})json", {hint}, '
                f"{str(a).lower()}, {str(n).lower()}, {str(w).lower()}, {str(o).lower()}}},"
            )
        body.append("    };")
        body.append("    for (const auto& f : fields) {")
        body.append(f'        SCOPED_TRACE(std::string("{type_name}.") + f.key);')
        body.append(f"        check_field<mcp::{type_name}>(baseline, f);")
        body.append("    }")
        body.append("}")

    total_cases = sum(len(c) for _, _, c in covered) * len(MODES)
    body.append(
        f"""
// The matrix's own arithmetic.  `types x fields x modes` is what a generated
// suite is worth; a hand-written suite of the same size is only worth what its
// author thought to write down.
TEST(JsonPeerInputMatrix, CaseCountIsDerived) {{
    constexpr int types = {len(covered)};
    constexpr int fields = {total_cases // len(MODES)};
    constexpr int modes = {len(MODES)};
    constexpr int generated_cases = {total_cases};
    EXPECT_EQ(generated_cases, fields * modes);
    EXPECT_GT(types, 0);
}}
"""
    )

    body.append("// Types in the matrix (checked against the protocol headers at build time):")
    for type_name, _, _ in covered:
        body.append(f"//   {type_name}")
    body.append("// Types deliberately not in the matrix, and why:")
    for type_name, reason in sorted(skipped.items()):
        body.append(f"//   {type_name}: {reason}")

    manifest = {
        "types": [t for t, _, _ in covered],
        "excluded": skipped,
        "case_count": total_cases,
        "field_count": total_cases // len(MODES),
        "mode_count": len(MODES),
        "gtest_count": len(covered) + 1,
    }
    return "\n".join(body) + "\n", manifest


# The reader for the file render() writes.  clang-format wraps some rows and
# baselines across lines, so every pattern allows whitespace anywhere, and all
# comparisons are on whitespace-stripped text.
_TEST_RE = re.compile(r"TEST\(\s*JsonPeerInputMatrix\s*,\s*(\w+)\s*\)\s*\{")
_BASELINE_RE = re.compile(
    r'const\s+json\s+baseline\s*=\s*json::parse\(\s*R"json\((.*?)\)json"\s*\)\s*;',
    re.DOTALL,
)
_ROW_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*R"json\((.*?)\)json"\s*,\s*(\d+)\s*,'
    r"\s*(true|false)\s*,\s*(true|false)\s*,\s*(true|false)\s*,\s*(true|false)\s*\}",
    re.DOTALL,
)
_CONST_RE = re.compile(r"constexpr\s+int\s+(\w+)\s*=\s*(\d+)\s*;")
_TAIL_MARKER = "// Types in the matrix"
_HEADER_COUNT_RE = re.compile(r"\d+\s+of\s+the\s+\d+\s+field\s+entries")
COLUMNS = ("absent", "null", "wrong_type", "oversized")

HARNESS_DIFFERS = (
    "harness text outside the case rows differs "
    "(hand edit, or a template change in scripts/gen_json_matrix.py)"
)


def strip_ws(text: str) -> str:
    return re.sub(r"\s+", "", text)


def parse_matrix(text: str) -> dict:
    """Reads a matrix back into its tests, rows and harness, however wrapped."""
    cut = text.find(_TAIL_MARKER)
    body, tail = (text, "") if cut < 0 else (text[:cut], text[cut:])
    heads = list(_TEST_RE.finditer(body))
    tests: dict[str, dict] = {}
    for i, head in enumerate(heads):
        end = heads[i + 1].start() if i + 1 < len(heads) else len(body)
        block = body[head.start() : end]
        base = _BASELINE_RE.search(block)
        rows = {}
        for r in _ROW_RE.finditer(block):
            rows[r.group(1)] = {
                "present": strip_ws(r.group(2)),
                "hint": int(r.group(3)),
                "absent": r.group(4) == "true",
                "null": r.group(5) == "true",
                "wrong_type": r.group(6) == "true",
                "oversized": r.group(7) == "true",
            }
        # What is left once the generated values are taken out: the part of the
        # block every TEST shares, so a hand edit to it is still seen.
        skeleton = _ROW_RE.sub("", _BASELINE_RE.sub("", block))
        skeleton = _CONST_RE.sub(lambda c: c.group(1), skeleton)
        tests[head.group(1)] = {
            "baseline": strip_ws(base.group(1)) if base else None,
            "rows": rows,
            "consts": {c.group(1): int(c.group(2)) for c in _CONST_RE.finditer(block)},
            "skeleton": strip_ws(skeleton),
        }
    prefix = body[: heads[0].start()] if heads else body
    # The header's null-fragile count follows from the rows, which are compared
    # one by one; a hand edit to it still fails the whitespace-stripped match.
    prefix = _HEADER_COUNT_RE.sub("of the field entries", prefix)
    return {"prefix": strip_ws(prefix), "tests": tests, "tail": strip_ws(tail)}


def _null_fragile(row: dict) -> bool:
    return not row["absent"] and row["null"]


def classify(committed: dict, generated: dict) -> tuple[list, list]:
    """Sorts every difference by direction, committed -> generated.

    Returns (regressions, stale).  A regression is a (test, key, line, kind)
    for a member that now throws on input the committed matrix says it
    tolerates: an explicit null on an optional member (kind "null"), or an
    absent key (kind "absent").  A row that becomes null-fragile counts as a
    whole, whichever column moved -- a required member made optional while
    still throwing on null is one -- and so does a new row that is already
    null-fragile, since that is how the defect class keeps arriving.
    Everything else -- a fix, stricter type or domain validation, rows or
    tests coming and going, harness edits -- is stale.
    """
    regressions: list[tuple[str, str, str, str]] = []
    stale: list[str] = []
    old_tests, new_tests = committed["tests"], generated["tests"]

    def where(test: str) -> str:
        return f"TEST(JsonPeerInputMatrix, {test})"

    harness = committed["prefix"] != generated["prefix"]
    if set(old_tests) == set(new_tests) and committed["tail"] != generated["tail"]:
        harness = True

    for test in old_tests:
        if test not in new_tests:
            stale.append(f"{where(test)}: only in the committed file")
    for test, new in new_tests.items():
        old = old_tests.get(test)
        if old is None:
            stale.append(f"{where(test)}: only in the generated file")
            added = new["rows"]
        else:
            added = {k: r for k, r in new["rows"].items() if k not in old["rows"]}
            harness = harness or old["skeleton"] != new["skeleton"]
            if old["baseline"] != new["baseline"]:
                stale.append(f"{where(test)}: baseline changed")
            for name in sorted(set(old["consts"]) | set(new["consts"])):
                was, now = old["consts"].get(name), new["consts"].get(name)
                if was != now:
                    stale.append(f"{where(test)}: {name} committed {was}, generated {now}")
            for key in old["rows"]:
                if key not in new["rows"]:
                    stale.append(f"{where(test)}: field '{key}' removed")
            for key, row in added.items():
                if not _null_fragile(row):
                    stale.append(f"{where(test)}: field '{key}' added")
            for key, prev in old["rows"].items():
                row = new["rows"].get(key)
                if row is None:
                    continue
                if prev["present"] != row["present"]:
                    stale.append(f"{where(test)}: {key} present value changed")
                if prev["hint"] != row["hint"]:
                    stale.append(f"{where(test)}: {key} wrong_hint {prev['hint']}->{row['hint']}")
                became_fragile = _null_fragile(row) and not _null_fragile(prev)
                for col in COLUMNS:
                    was, now = prev[col], row[col]
                    if was == now:
                        continue
                    move = f"{where(test)}: {key} {col} {str(was).lower()}->{str(now).lower()}"
                    if col == "absent" and became_fragile:
                        # Optional now, but still throwing on null: not a fix.
                        # When null moved too, its own line names the row.
                        if prev["null"]:
                            regressions.append(
                                (
                                    test,
                                    key,
                                    f"{where(test)}: {key} became optional but throws "
                                    "on explicit null",
                                    "null",
                                )
                            )
                    elif not now:
                        stale.append(f"{move} (now tolerated)")
                    elif col == "absent":
                        regressions.append(
                            (test, key, f"{move} (member is now required)", "absent")
                        )
                    elif col == "null" and not row["absent"]:
                        regressions.append(
                            (
                                test,
                                key,
                                f"{move} (optional member now throws on explicit null)",
                                "null",
                            )
                        )
                    elif col == "null":
                        stale.append(f"{move} (required member)")
                    else:
                        stale.append(f"{move} (stricter validation)")
        for key, row in added.items():
            if _null_fragile(row):
                regressions.append(
                    (
                        test,
                        key,
                        f"{where(test)}: field '{key}' added as null-fragile "
                        "(optional member throws on explicit null)",
                        "null",
                    )
                )

    if harness:
        stale.append(HARNESS_DIFFERS)
    return regressions, stale


def regression_block(regressions: list) -> list[str]:
    lines = [
        "peer-input matrix REGRESSION: the decoders now throw where the committed "
        "matrix says they tolerate the input."
    ]
    lines += [f"  {line}" for _, _, line, _ in regressions]
    lines.append(
        "Do not regenerate to make this pass; that records the regression as expected behaviour."
    )
    kinds = {kind for _, _, _, kind in regressions}
    if "null" in kinds:
        lines.append("For a null row: make the decoder treat an explicit null as absent.")
    if "absent" in kinds:
        lines.append(
            "For an absent row: the member became required, which breaks every peer that "
            "omits it. Check whether the spec now requires it."
        )
    lines.append(
        "If the stricter behaviour is intended, regenerate with --accept-regression "
        "Type.key for each row and say why in the commit message."
    )
    return lines


def committed_matrix(base: str) -> str | None:
    """The matrix as committed at git HEAD, or None when git cannot say."""
    try:
        proc = subprocess.run(
            ["git", "show", "HEAD:./test/core/json_peer_input_matrix_test.cpp"],
            cwd=base,
            capture_output=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.decode("utf-8", errors="replace")


def guard_baseline(out_text: str | None, head_text: str | None) -> str | None:
    """The matrix a render is guarded against.

    The output file when it holds a matrix; otherwise the one committed at
    HEAD, so deleting or emptying the file, or pointing --out somewhere new,
    does not leave nothing to compare with.  None when neither has a TEST.
    """
    for text in (out_text, head_text):
        if text is not None and parse_matrix(text)["tests"]:
            return text
    return None


def guard(committed_text: str, rendered_text: str, accepted: list[str]) -> tuple[int, list[str]]:
    """Refuses a render that would record a regression; returns (exit code, lines).

    Regenerating makes the build check pass by definition, so the check alone
    cannot stop a regression from being written down as the expected
    behaviour.  This can: each regressed row must be named, as ``Type.key`` or
    as a bare ``Type`` (the TEST name, with ``::`` spelled ``_``), before it
    is written.
    """
    committed = parse_matrix(committed_text)
    regressions = classify(committed, parse_matrix(rendered_text))[0] if committed["tests"] else []
    names = {name: name.replace("::", "_") for name in accepted}
    lines = [
        f"--accept-regression {name} matches no regressed row"
        for name, norm in names.items()
        if not any(norm in (test, f"{test}.{key}") for test, key, _, _ in regressions)
    ]
    blocked = [
        r
        for r in regressions
        if r[0] not in names.values() and f"{r[0]}.{r[1]}" not in names.values()
    ]
    if blocked:
        return 2, lines + regression_block(blocked)
    return (1 if lines else 0), lines


def write(text: str, out_path: str) -> None:
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write(text)

    # Format on the way out, so regenerating is idempotent against the repo's
    # pre-commit hook rather than producing a diff every time.
    formatter = shutil.which("clang-format")
    if formatter:
        subprocess.run([formatter, "-i", out_path], check=False)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--repo", default=repo)
    ap.add_argument(
        "--out",
        default=os.path.join(repo, "test", "core", "json_peer_input_matrix_test.cpp"),
    )
    ap.add_argument("--manifest", default=os.path.join(repo, "test", "core", "json_matrix_manifest.json"))
    ap.add_argument(
        "--accept-regression",
        action="append",
        default=[],
        metavar="Type[.key]",
        help="write a row that regressed toward throwing, named by its TEST name "
        "(the type, with :: as _) or TEST.key; say why in the commit message",
    )
    ap.add_argument(
        "--no-baseline",
        action="store_true",
        help="write even when there is no committed matrix to check for regressions",
    )
    args = ap.parse_args(argv)

    text, manifest = render(args.repo)
    out_text = None
    if os.path.exists(args.out):
        with open(args.out, encoding="utf-8") as fh:
            out_text = fh.read()
    committed_text = guard_baseline(out_text, committed_matrix(args.repo))
    if committed_text is None and not args.no_baseline:
        sys.stderr.write(
            f"{args.out} is missing or has no TEST(JsonPeerInputMatrix, ...), and no "
            "matrix is committed at git HEAD, so the new rows cannot be checked for "
            "regressions. Restore it (git checkout -- "
            "test/core/json_peer_input_matrix_test.cpp), or pass --no-baseline.\n"
        )
        return 1
    code, lines = guard(committed_text or "", text, args.accept_regression)
    for line in lines:
        sys.stderr.write(line + "\n")
    if code:
        return code

    write(text, args.out)
    with open(args.manifest, "w", encoding="utf-8") as fh:
        # Four-space indent to match the repo's pretty-format-json hook, so
        # regenerating does not leave a diff behind.
        json.dump(manifest, fh, indent=4, sort_keys=True)
        fh.write("\n")

    sys.stderr.write(
        f"types={len(manifest['types'])} excluded={len(manifest['excluded'])} "
        f"fields={manifest['field_count']} modes={manifest['mode_count']} "
        f"cases={manifest['case_count']} gtests={manifest['gtest_count']}\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
