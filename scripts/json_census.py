#!/usr/bin/env python3
"""Enumerate every deserialization of peer-supplied JSON in the shipped surface.

The census is regenerated from source on every run.  It answers, for each site
where a peer-controlled ``nlohmann::json`` document is read, four questions:

    absent      -- is a missing key tolerated?
    null        -- is a key that is present and explicitly ``null`` tolerated?
    wrong_type  -- is a key whose value has the wrong JSON type tolerated?
    oversized   -- is the value's size bounded before it is materialised?

and one more that decides how much a "no" costs:

    fatality    -- does the resulting throw cost one message, one session, or
                   the process?

Usage
-----
    scripts/json_census.py                      # census of the working tree
    scripts/json_census.py --rev a173cf82       # census of a git revision
    scripts/json_census.py --format json        # machine-readable rows
    scripts/json_census.py --only-flagged       # rows that tolerate less than everything
    scripts/json_census.py --require FILE:LINE  # exit non-zero unless that site is flagged

``--require`` is the self-test: it asserts that the census rediscovers a site
known to be defective.  A census that cannot find a known defect in known
defective code has not been shown to find anything.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import asdict, dataclass, field
from typing import Iterator, Optional

# --------------------------------------------------------------------------
# Tolerance vocabulary
# --------------------------------------------------------------------------

OK = "ok"  # input in this mode is accepted
THROW = "throw"  # input in this mode raises
UNBOUNDED = "unbounded"  # no size ceiling is applied before materialising
BOUNDED = "bounded"
NA = "-"

# Functions that establish a null-tolerant presence test.  ``has_json_value``
# is the helper added in include/mcp/protocol/base.hpp; the census recognises
# it so that it keeps reporting correctly once that helper is in use.
NULL_SAFE_HELPERS = ("has_json_value",)

# --------------------------------------------------------------------------
# Exception barriers
#
# A throw is only as expensive as the nearest handler that catches it.  These
# tables name the handlers the census knows about; every other function is
# resolved by walking the static call graph.  Entries are keyed by the name of
# the function whose body contains the catch.
# --------------------------------------------------------------------------

# Catch sites that turn a throw into a JSON-RPC error response and keep going.
PER_MESSAGE_CATCH_MARKERS = (
    "make_error_wire",
    "make_error_response",
    "send_error",
    "McpError",
    "g_INVALID_PARAMS",
    "g_INVALID_REQUEST",
    "g_PARSE_ERROR",
    "g_INTERNAL_ERROR",
)

# Catch sites that tear the session down: the read loop exits, pending requests
# are failed, the transport is closed.
SESSION_FATAL_CATCH_MARKERS = (
    "fail_pending_requests",
    "transport->close()",
    "closed.store",
    "g_CONNECTION_CLOSED",
)

# Reader loops.  A throw that reaches one of these ends the session even when
# the loop catches it, because the loop does not resume.
SESSION_LOOP_FUNCTIONS = ("read_loop", "run_read_loop", "receive_loop", "message_loop")

FATAL_SESSION = "session"
FATAL_MESSAGE = "message"
FATAL_PROCESS = "process"
FATAL_UNKNOWN = "unknown"

# --------------------------------------------------------------------------
# Envelope keys
#
# A JSON-RPC envelope is routed by presence tests, not by get<T>(), so the
# accessor census does not see them.  They fail differently too: a presence
# test that misreads an explicit null does not throw, it routes the message
# somewhere wrong -- usually into a silent drop, which is harder to notice
# than a crash.
#
# Null-tolerance is NOT uniform across these keys, and treating them alike is
# its own bug:
#
#   "result": null  is a legitimate empty result.  Presence is the correct
#                   test; a null check here would reject valid traffic.
#   "error":  null  is not an error.  A peer that serialises absent optionals
#                   as null -- Go without omitempty, a dumped dataclass --
#                   sends this routinely, and a presence test misroutes it.
#   "id":     null  is what JSON-RPC prescribes when the id cannot be
#                   determined; it is not an id.
#   "params"/"_meta": null and absent mean the same thing to a peer.
#
# `verdict` is what a *present and null* value does to the routing decision.
# --------------------------------------------------------------------------

MISROUTE = "misroute"

ENVELOPE_KEYS = {
    # Keys whose presence test decides where the message goes.
    "error": (MISROUTE, "'error': null is not an error but tests as present"),
    "id": (MISROUTE, "'id': null is JSON-RPC for an undeterminable id, not an id"),
    "result": ("ok", "null is a legitimate empty result; presence is the correct test"),
    "method": ("ok", "a null method is malformed whichever way it is tested"),
    "jsonrpc": ("ok", "a null jsonrpc is malformed whichever way it is tested"),
    # Payload keys.  A present null here is stored, not misrouted: the member
    # is an optional<json> and ends up engaged holding null.  Where the
    # guarded block instead calls get<ConcreteType>() the throw is real, and
    # the accessor row for that line already carries it -- recording it twice
    # would inflate the count without adding a site.
    "params": ("ok", "a present null is stored as an engaged optional, not misrouted"),
    "_meta": ("ok", "a present null is stored as an engaged optional, not misrouted"),
}

# --------------------------------------------------------------------------
# Source preparation
# --------------------------------------------------------------------------

SCAN_ROOTS = ("include", "src")
SOURCE_SUFFIXES = (".hpp", ".cpp", ".h", ".cc")


def strip_comments(text: str) -> str:
    """Blank out comments while preserving every byte offset and line break.

    Offsets must survive so that reported line numbers match the original file.
    String literals are preserved because the JSON keys live in them.
    """
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or ch == "'":
            quote = ch
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "/" and i + 1 < n:
            if text[i + 1] == "/":
                while i < n and text[i] != "\n":
                    out[i] = " "
                    i += 1
                continue
            if text[i + 1] == "*":
                out[i] = out[i + 1] = " "
                i += 2
                while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                    if text[i] != "\n":
                        out[i] = " "
                    i += 1
                if i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                continue
        i += 1
    return "".join(out)


def line_index(text: str) -> list[int]:
    """Offsets at which each line starts, so an offset maps to a line number."""
    starts = [0]
    for m in re.finditer("\n", text):
        starts.append(m.end())
    return starts


def line_of(starts: list[int], offset: int) -> int:
    lo, hi = 0, len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= offset:
            lo = mid
        else:
            hi = mid - 1
    return lo + 1


# --------------------------------------------------------------------------
# Block structure
# --------------------------------------------------------------------------


@dataclass
class Block:
    """One brace-delimited scope, with the text that introduced it."""

    header: str
    start: int  # offset just after '{'
    end: int  # offset of the matching '}'
    kind: str  # function | if | else | try | catch | loop | other
    parent: Optional["Block"] = None
    children: list["Block"] = field(default_factory=list)


def classify_header(header: str) -> str:
    h = header.strip()
    if re.search(r"\bcatch\s*\(", h):
        return "catch"
    if re.search(r"\btry\s*$", h):
        return "try"
    if re.search(r"\belse\s+if\s*\(", h):
        return "if"
    if re.search(r"\belse\s*$", h):
        return "else"
    if re.search(r"^\s*if\s*\(|[});]\s*if\s*\(", h) or re.match(r"^if\s*\(", h):
        return "if"
    if re.search(r"\b(for|while|switch)\s*\(", h):
        return "loop"
    if re.search(r"\b(\w+)\s*\([^;]*\)\s*(const\s*)?(noexcept\s*)?(->[^{]*)?$", h) and (
        "=" not in h.split("(")[0]
    ):
        return "function"
    return "other"


def parse_blocks(text: str) -> Block:
    """Build the brace tree.  ``text`` must already have comments blanked."""
    root = Block(header="", start=0, end=len(text), kind="file")
    stack = [root]
    seg_start = 0  # start of the header accumulating for the next '{'
    paren = 0  # a ';' inside parentheses is `for (;;)`, not a statement end
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "(":
            paren += 1
        elif ch == ")":
            paren = max(0, paren - 1)
        if ch == '"' or ch == "'":
            quote = ch
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "{":
            header = text[seg_start:i]
            blk = Block(
                header=header,
                start=i + 1,
                end=n,
                kind=classify_header(header),
                parent=stack[-1],
            )
            stack[-1].children.append(blk)
            stack.append(blk)
            seg_start = i + 1
            paren = 0
        elif ch == "}":
            if len(stack) > 1:
                blk = stack.pop()
                blk.end = i
            seg_start = i + 1
            paren = 0
        elif ch == ";" and paren == 0:
            seg_start = i + 1
        i += 1
    return root


def enclosing_chain(root: Block, offset: int) -> list[Block]:
    """Innermost-last list of blocks containing ``offset``."""
    chain: list[Block] = []
    node = root
    while True:
        for child in node.children:
            if child.start <= offset < child.end:
                chain.append(child)
                node = child
                break
        else:
            return chain


# --------------------------------------------------------------------------
# Guard recognition
# --------------------------------------------------------------------------

KEY = r'"((?:[^"\\]|\\.)*)"'


def presence_guards(expr: str) -> set[tuple[str, str]]:
    """(doc, key) pairs the expression asserts are *present* (may still be null)."""
    found = set()
    for m in re.finditer(rf"(!\s*)?([\w\->\.\(\)]*?)\.contains\(\s*{KEY}\s*\)", expr):
        if m.group(1):  # a negated contains() asserts absence, not presence
            continue
        found.add((normalise_doc(m.group(2)), m.group(3)))
    for m in re.finditer(rf"(?:^|[^\w])(\w*::)?find\(\s*{KEY}\s*\)\s*!=", expr):
        found.add(("*", m.group(2)))
    return found


def null_safe_guards(expr: str) -> set[tuple[str, str]]:
    """(doc, key) pairs the expression asserts are present *and not null*."""
    found = set()
    # !doc.at("k").is_null()  /  !doc["k"].is_null()
    for m in re.finditer(rf"!\s*([\w\->\.\(\)]*?)(?:\.at\(\s*{KEY}\s*\)|\[\s*{KEY}\s*\])\s*\.is_null\(\)", expr):
        found.add((normalise_doc(m.group(1)), m.group(2) or m.group(3)))
    # doc.at("k").is_string() / is_object() / is_number...() -- a positive type
    # test implies not-null as a side effect.
    for m in re.finditer(
        rf"([\w\->\.\(\)]*?)(?:\.at\(\s*{KEY}\s*\)|\[\s*{KEY}\s*\])\s*\.is_(string|object|array|number|number_integer|number_float|number_unsigned|boolean|binary)\(\)",
        expr,
    ):
        found.add((normalise_doc(m.group(1)), m.group(2) or m.group(3)))
    for helper in NULL_SAFE_HELPERS:
        for m in re.finditer(rf"{helper}\(\s*([\w\->\.\(\)]+)\s*,\s*{KEY}\s*\)", expr):
            found.add((normalise_doc(m.group(1)), m.group(2)))
    return found


def type_guards(expr: str) -> set[tuple[str, str]]:
    """(doc, key) pairs the expression checks the *type* of before use.

    ``is_null()`` is not among them.  It establishes that a value is not null
    and says nothing about what it is, so reading it as a type check declared
    every null-guarded field type-safe -- and an object arriving where a
    string-or-integer id was expected still threw.
    """
    found = set()
    for m in re.finditer(
        rf"([\w\->\.\(\)]*?)(?:\.at\(\s*{KEY}\s*\)|\[\s*{KEY}\s*\])\s*\.is_(?!null\b)\w+\(\)",
        expr,
    ):
        found.add((normalise_doc(m.group(1)), m.group(2) or m.group(3)))
    return found


BOOL_ALIAS = re.compile(
    r"(?:const\s+)?bool\s+(\w+)\s*=\s*([^;]+);"
)


def bool_aliases(fn_body: str) -> dict[str, str]:
    """Locals that stand in for a presence or type test.

    ``const bool has_error = json.contains("error");`` moves the guard out of
    the ``if`` header.  Without this substitution the census reads the later
    ``if (has_error)`` as unguarded and misfiles a null-fragile site as a
    required field -- the mistake that let one live site look like a different
    defect class.
    """
    out: dict[str, str] = {}
    for m in BOOL_ALIAS.finditer(fn_body):
        name, expr = m.group(1), m.group(2)
        if ".contains(" in expr or ".is_" in expr or "has_json_value" in expr:
            out[name] = expr
    return out


def expand_aliases(expr: str, aliases: dict[str, str]) -> str:
    """Substitute alias names in a condition with the test they stand for."""
    if not aliases:
        return expr
    for _ in range(3):
        before = expr
        for name, replacement in aliases.items():
            expr = re.sub(rf"\b{re.escape(name)}\b", f"({replacement})", expr)
        if expr == before:
            break
    return expr


def normalise_doc(raw: str) -> str:
    """Reduce a receiver expression to a comparable document name."""
    raw = raw.strip()
    raw = re.sub(r"^[\(\!\*&]+", "", raw)
    raw = raw.replace("->", ".")
    return raw.split(".")[-1] if raw else "*"


def doc_matches(a: str, b: str) -> bool:
    return a == "*" or b == "*" or a == b


# --------------------------------------------------------------------------
# Peer-document roots
# --------------------------------------------------------------------------

JSON_PARAM = re.compile(
    r"(?:const\s+)?(?:nlohmann::)?json\s*(?:&|&&|\s)\s*(\w+)\s*(?:,|\))"
)


def peer_roots_for(block: Block, text: str) -> set[str]:
    """Names in scope that hold a document a peer controls.

    Roots are function parameters of ``nlohmann::json`` type; the set then
    propagates through local bindings initialised from an existing root.  A
    function parameter typed ``nlohmann::json`` is peer-controlled because every
    such parameter in this codebase is reached from ``json::parse`` of bytes
    off the wire; the census states that assumption rather than proving it, and
    ``--list-roots`` prints the set so it can be audited.
    """
    roots: set[str] = set()
    fn = block
    while fn is not None and fn.kind != "function":
        fn = fn.parent
    if fn is None:
        return roots
    for m in JSON_PARAM.finditer(fn.header):
        roots.add(m.group(1))
    if not roots:
        return roots
    body = text[fn.start : fn.end]
    # Propagate through local bindings: `auto x = <expr mentioning a root>`
    for _ in range(4):  # fixed point; depth 4 is past anything in this tree
        before = len(roots)
        pattern = re.compile(
            r"(?:const\s+)?(?:auto|nlohmann::json|json)\s*(?:&|&&)?\s*(\w+)\s*=\s*([^;]+);"
        )
        for m in pattern.finditer(body):
            name, init = m.group(1), m.group(2)
            if name in roots:
                continue
            if any(re.search(rf"\b{re.escape(r)}\b", init) for r in roots):
                roots.add(name)
        if len(roots) == before:
            break
    return roots


# --------------------------------------------------------------------------
# Access sites
# --------------------------------------------------------------------------

ACCESS_PATTERNS = [
    # doc.at("k").get<T>()  /  .get_to(...)  /  .get_ref<...>
    (
        "at_get",
        re.compile(
            rf"([\w\->\.]+?)\.at\(\s*{KEY}\s*\)\s*\.\s*(get|get_to|get_ref|get_ptr)\b"
        ),
    ),
    # doc["k"].get<T>()
    (
        "sub_get",
        re.compile(rf"([\w\->\.]+?)\[\s*{KEY}\s*\]\s*\.\s*(get|get_to|get_ref|get_ptr)\b"),
    ),
    # doc.at("k") used as a value (assignment / argument), no explicit get
    ("at_bare", re.compile(rf"([\w\->\.]+?)\.at\(\s*{KEY}\s*\)")),
    # doc.value("k", default)
    ("value_default", re.compile(rf"([\w\->\.]+?)\.value\(\s*{KEY}\s*,")),
    # doc.get<T>() on the whole document
    ("doc_get", re.compile(r"([\w\->\.]+?)\.get(?:_to)?\s*<")),
]

TYPE_ARG = re.compile(r"\.get(?:_to|_ref|_ptr)?\s*<\s*([^>]+(?:<[^>]*>)?[^>]*)\s*>")

SIZE_LIMIT_TOKENS = (
    ".size() >",
    ".size() >=",
    ".length() >",
    "max_size",
    "max_message",
    "max_body",
    "size_limit",
    "MAX_",
    "g_MAX",
)


@dataclass
class Row:
    file: str
    line: int
    owner: str  # deserialised type, or the enclosing function
    field_name: str
    site_kind: str
    absent: str
    null: str
    wrong_type: str
    oversized: str
    guard: str
    fatality: str
    function: str
    snippet: str
    target_types: tuple[str, ...] = ()
    fatality_basis: str = "lexical"
    target_hint: str = ""     # the get<T> the site names, verbatim
    member: str = ""          # the C++ member the key is read into
    constrained: str = NA     # the field has a value domain, not just a type

    @property
    def flagged(self) -> bool:
        """The site rejects, or misroutes, input a careful peer may legitimately send."""
        return THROW in (self.absent, self.null, self.wrong_type) or self.null == MISROUTE

    @property
    def defect_class(self) -> str:
        """The shape of the site's intolerance, independent of its line number."""
        if self.null == MISROUTE:
            return "misroute"
        if self.absent == OK and self.null == THROW:
            # Absent is handled, present-and-null is not.  This is the class
            # that has outrun prediction: a peer that serialises absent
            # optionals as explicit null reaches it without trying.
            return "null-fragile"
        if self.absent == THROW:
            return "required"
        if self.wrong_type == THROW:
            return "untyped"
        return "tolerant"


def statement_end(text: str, offset: int, limit: int) -> int:
    """Offset of the ';' that ends the statement containing ``offset``.

    A fixed character window is not a statement.  Searching 200 characters
    ahead for a `get<T>` picked up the *next* line's type whenever the current
    line read through `get_to(...)`, which names no type -- so `roots` was
    typed from the `_meta` line below it, and the verdict came out inverted.
    """
    depth = 0
    i = offset
    while i < limit:
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif ch == ";" and depth == 0:
            return i
        elif ch in "{}" and depth == 0:
            return i
        i += 1
    return limit


def statement_prefix(text: str, offset: int, block: Block) -> str:
    """Text of the current statement up to ``offset``, for same-expression guards."""
    start = block.start
    for sep in (";", "{", "}"):
        idx = text.rfind(sep, block.start, offset)
        if idx != -1:
            start = max(start, idx + 1)
    return text[start:offset]


def enclosing_function(chain: list[Block]) -> tuple[str, Optional[Block]]:
    for blk in reversed(chain):
        if blk.kind == "function":
            m = re.search(r"(\w+)\s*\([^()]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->[^{]*)?$", blk.header.strip())
            if m:
                return m.group(1), blk
            return "?", blk
    return "<file-scope>", None


FROM_JSON_SIG = re.compile(
    r"from_json\s*\(\s*const\s+(?:nlohmann::)?json\s*&\s*(\w+)\s*,\s*([\w:]+(?:<[^>]*>)?)\s*&"
)


def deserialised_type(chain: list[Block]) -> Optional[str]:
    for blk in reversed(chain):
        if blk.kind == "function":
            m = FROM_JSON_SIG.search(blk.header)
            if m:
                return m.group(2)
    return None


def size_bounded(fn_body: str) -> bool:
    return any(tok in fn_body for tok in SIZE_LIMIT_TOKENS)


# --------------------------------------------------------------------------
# Fatality resolution
# --------------------------------------------------------------------------


@dataclass
class FunctionInfo:
    name: str
    file: str
    block: Block
    body: str
    calls: set[str]


def unit_key(rel_path: str) -> str:
    """Header and implementation of one component count as one unit.

    `client.hpp` declares what `client.cpp` defines and calls; resolving the
    call graph per-file would sever that pair.
    """
    return os.path.splitext(os.path.basename(rel_path))[0]


def catch_disposition(catch_body: str) -> str:
    if any(tok in catch_body for tok in SESSION_FATAL_CATCH_MARKERS):
        return FATAL_SESSION
    if any(tok in catch_body for tok in PER_MESSAGE_CATCH_MARKERS):
        return FATAL_MESSAGE
    if re.search(r"\bthrow\s*;", catch_body):
        return "rethrow"
    return FATAL_MESSAGE  # a catch that neither tears down nor rethrows contains the throw


def local_barrier(text: str, chain: list[Block]) -> Optional[str]:
    """Disposition of the innermost try/catch lexically containing the site.

    The structural rule outranks anything the catch body says.  A ``try`` that
    *encloses* the read loop cannot resume it: control leaves the loop, no
    further message is read, and the session is over whether or not the handler
    says so.  An empty catch around ``for (;;) { ... }`` is session-fatal.  A
    ``try`` *inside* the loop is a per-message barrier: the next iteration runs.
    """
    for blk in reversed(chain):
        if blk.kind != "try":
            continue
        parent = blk.parent
        if parent is None:
            continue
        dispositions = []
        for sib in parent.children:
            if sib.kind == "catch" and sib.start > blk.end:
                dispositions.append(catch_disposition(text[sib.start : sib.end]))
        if not dispositions or all(d == "rethrow" for d in dispositions):
            continue
        # Is there a loop between this try and the site?  If so the catch is
        # outside the loop and unwinding ends it.
        idx = chain.index(blk)
        if any(b.kind == "loop" for b in chain[idx + 1 :]):
            return FATAL_SESSION
        if FATAL_SESSION in dispositions:
            return FATAL_SESSION
        return FATAL_MESSAGE
    return None


def resolve_fatality(
    text: str,
    chain: list[Block],
    fn_name: str,
    index: dict[str, list[FunctionInfo]],
    current_file: str,
    depth: int = 0,
    seen: Optional[set[str]] = None,
) -> str:
    """Nearest handler on any static path out of the site."""
    local = local_barrier(text, chain)
    if local is not None:
        return local
    if fn_name in SESSION_LOOP_FUNCTIONS:
        return FATAL_SESSION
    seen = seen or set()
    if fn_name in seen or depth > 6:
        return FATAL_UNKNOWN
    seen.add(fn_name)

    # Prefer callers in the same translation unit.  `dispatch_response` exists
    # in both the client and the server; resolving by name alone would let the
    # client's read loop decide the server's blast radius.
    unit = unit_key(current_file)
    same_file = {
        name: [i for i in infos if unit_key(i.file) == unit]
        for name, infos in index.items()
    }
    scope = {n: i for n, i in same_file.items() if i}
    if not any(
        re.search(rf"\b{re.escape(fn_name)}\s*\(", i.body) for l in scope.values() for i in l
    ):
        scope = index

    outcomes = set()
    for caller_name, infos in scope.items():
        for info in infos:
            for m in re.finditer(rf"\b{re.escape(fn_name)}\s*\(", info.body):
                call_off = info.block.start + m.start()
                call_chain = enclosing_chain(info.block, call_off)
                sub = local_barrier(info.body_text, [info.block] + call_chain)  # type: ignore[attr-defined]
                if sub is not None:
                    outcomes.add(sub)
                elif caller_name in SESSION_LOOP_FUNCTIONS:
                    outcomes.add(FATAL_SESSION)
                else:
                    outcomes.add(
                        resolve_fatality(
                            info.body_text,  # type: ignore[attr-defined]
                            [info.block] + call_chain,
                            caller_name,
                            index,
                            info.file,
                            depth + 1,
                            set(seen),
                        )
                    )
    if not outcomes:
        return FATAL_UNKNOWN
    # Report the worst barrier actually found.  A chain that also has an
    # unresolved branch is still reported at its known severity rather than
    # collapsing to "unknown", because one unreachable caller must not erase
    # the evidence from the reachable ones.  `--incomplete-chains` lists the
    # rows where some branch stayed unresolved.
    for level in (FATAL_PROCESS, FATAL_SESSION, FATAL_MESSAGE):
        if level in outcomes:
            return level
    return FATAL_UNKNOWN


# --------------------------------------------------------------------------
# Scanning
# --------------------------------------------------------------------------

ENUM_MACRO = re.compile(
    r"NLOHMANN_JSON_SERIALIZE_ENUM\s*\(\s*([\w:]+)\s*,\s*\{(.*?)\}\s*\)",
    re.DOTALL,
)

# A validator called on a member turns a field into a value constraint: the
# field has a domain, not just a JSON type, so an oversized or arbitrary value
# of the right type is still rejected.  Without this the matrix reads such a
# rejection as a size limit that does not exist.
VALIDATOR_CALL = re.compile(r"\b(?:detail::)?(validate_\w+)\s*\(\s*([\w\.\->]+)")

MACRO_DEFINE = re.compile(
    r"NLOHMANN_DEFINE_TYPE_(NON_INTRUSIVE|INTRUSIVE)(_WITH_DEFAULT)?\s*\(\s*([\w:]+)\s*,\s*([^)]*)\)"
)


# A handler stored in a map and invoked through `iter->second(...)` is an
# indirect call.  No name-based call graph crosses it -- which is why a
# progress callback could sit on the read loop's blast radius while every
# static reading of it said "unknown".  These two patterns recover the edge:
# where a lambda is stored, and where the container that holds it is called.
REGISTRY_LOOKUP = re.compile(
    r"\w*(?:handler|callback)s?\s*\.(?:find|at|contains)\s*\("
)


def indirect_dispatch_radius(
    text: str,
    root: Block,
    index: dict[str, list[FunctionInfo]],
    rel: str,
) -> str:
    """Blast radius of the worst function that dispatches to a stored handler.

    Discovery runs from the dispatch side, not the registration side.  A
    function that looks a key up in a handler registry goes on to call what it
    found -- whether as ``iter->second(...)`` or, as here, by copying the
    callable out under a lock and calling it afterwards.  Registration may be
    several hops away (``on_progress`` -> ``on_notification`` ->
    ``handlers[m] = cb``) and need not be followed: every handler in the
    registry shares the radius of the function that dispatches to it.
    """
    radius = FATAL_UNKNOWN
    seen: set[int] = set()
    for lookup in REGISTRY_LOOKUP.finditer(text):
        chain = enclosing_chain(root, lookup.start())
        if not chain:
            continue
        fn = next((b for b in reversed(chain) if b.kind == "function"), None)
        if fn is None or fn.start in seen:
            continue
        seen.add(fn.start)
        fn_chain = enclosing_chain(root, fn.start)
        fn_name, _ = enclosing_function(fn_chain)
        radius = worst(radius, resolve_fatality(text, fn_chain, fn_name, index, rel))
    return radius


JSON_LAMBDA = re.compile(r"\[[^\]\[]*\]\s*\([^)]*(?:const\s+)?(?:nlohmann::)?json\s*&")


def peer_handler_regions(text: str, root: Block, radius: str) -> list[tuple[int, int, str]]:
    """Offset ranges of lambdas that receive a peer document."""
    if radius == FATAL_UNKNOWN:
        return []
    regions: list[tuple[int, int, str]] = []
    for m in JSON_LAMBDA.finditer(text):
        blk = lambda_block_after(root, m.start())
        if blk is not None:
            regions.append((blk.start, blk.end, radius))
    return regions


def fatality_at(
    offset: int, lexical: str, regions: list[tuple[int, int, str]]
) -> str:
    """Upgrade an unresolved site that sits inside a peer-invoked handler."""
    if lexical != FATAL_UNKNOWN:
        return lexical
    for start, end, radius in regions:
        if start <= offset < end:
            return radius
    return lexical


def lambda_block_after(root: Block, offset: int) -> Optional[Block]:
    """The block that opens first after ``offset`` -- a stored lambda's body."""
    best: Optional[Block] = None

    def walk(blk: Block) -> None:
        nonlocal best
        if blk.start > offset and (best is None or blk.start < best.start):
            best = blk
        for child in blk.children:
            walk(child)

    walk(root)
    return best


def scan_file(
    path: str, rel: str, index: dict[str, list[FunctionInfo]]
) -> tuple[list[Row], dict[str, list[tuple[str, str]]]]:
    raw = open(path, "r", encoding="utf-8", errors="replace").read()
    text = strip_comments(raw)
    starts = line_index(text)
    root = parse_blocks(text)
    structs = parse_structs(text, root)
    indirect_regions = peer_handler_regions(
        text, root, indirect_dispatch_radius(text, root, index, rel)
    )
    rows: list[Row] = []

    # -- macro-generated serialisers ---------------------------------------
    for m in MACRO_DEFINE.finditer(text):
        with_default = bool(m.group(2))
        type_name = m.group(3)
        fields = [f.strip() for f in m.group(4).split(",") if f.strip()]
        line = line_of(starts, m.start())
        for fname in fields:
            rows.append(
                Row(
                    file=rel,
                    line=line,
                    owner=type_name,
                    field_name=fname,
                    site_kind="macro_with_default" if with_default else "macro_required",
                    absent=OK if with_default else THROW,
                    # the macro emits at(...).get_to(...); a present null throws
                    # for every member type but nlohmann::json and std::optional
                    null=THROW,
                    wrong_type=THROW,
                    oversized=UNBOUNDED,
                    guard="none (macro)",
                    fatality=FATAL_UNKNOWN,
                    function=f"<macro {type_name}>",
                    snippet=m.group(0)[:120],
                    target_types=named_types(
                        next((t for t, n in structs.get(type_name, []) if n == fname), "")
                    ),
                    # NLOHMANN_DEFINE_TYPE keys each member by its own name.
                    member=fname,
                )
            )

    # -- explicit accessor sites -------------------------------------------
    claimed: set[tuple[int, str]] = set()
    for pattern_kind, pattern in ACCESS_PATTERNS:
        for m in pattern.finditer(text):
            site_kind = pattern_kind
            off = m.start()
            doc_raw = m.group(1)
            doc = normalise_doc(doc_raw)
            key = m.group(2) if pattern.groups >= 2 and site_kind != "doc_get" else None

            chain = enclosing_chain(root, off)
            if not chain:
                continue
            fn_name, fn_block = enclosing_function(chain)
            if fn_block is None:
                continue
            roots = peer_roots_for(chain[-1], text)
            if doc not in roots:
                continue

            # de-duplicate: at_bare also matches what at_get already claimed
            marker = (off, doc)
            if site_kind == "at_bare" and any(
                c[0] == off for c in claimed
            ):
                continue
            claimed.add(marker)

            line = line_of(starts, off)
            stmt = statement_prefix(text, off, chain[-1])

            # Guards in force: enclosing if-conditions, plus the current
            # statement's own prefix (ternaries and && chains).
            aliases = bool_aliases(text[fn_block.start : fn_block.end])
            present: set[tuple[str, str]] = set()
            nonnull: set[tuple[str, str]] = set()
            typed: set[tuple[str, str]] = set()
            rejected: set[tuple[str, str]] = set()
            for blk in chain:
                if blk.kind in ("if", "loop"):
                    header = expand_aliases(blk.header, aliases)
                    present |= presence_guards(header)
                    body = text[blk.start : blk.end]
                    if terminates(body) and re.search(r"!\s*[\w\->\.]+?(?:\.at\(|\[)", header):
                        # `if (contains(k) && !at(k).is_object()) { throw; }`
                        # proves the shape for the code that follows, but what
                        # the peer sees is a rejection: sending null here
                        # throws.  Counting it as a tolerance inverted the
                        # verdict on every validated field.
                        rejected |= type_guards(header)
                    else:
                        nonnull |= null_safe_guards(header)
                        typed |= type_guards(header)
            stmt_x = expand_aliases(stmt, aliases)
            present |= presence_guards(stmt_x)
            nonnull |= null_safe_guards(stmt_x)
            typed |= type_guards(stmt_x)
            nonnull -= rejected
            typed -= rejected
            present |= early_return_guards(text, chain, off, aliases)
            nonnull |= early_return_null_guards(text, chain, off)

            if key is None:
                # A whole-document get<T>(): the document is handed to T's own
                # from_json, whose fields this census enumerates separately.
                # The row records the delegation and whether the document
                # itself was shape-checked first.
                key = "<whole document>"
                guarded_present = True
                guarded_null = any(
                    re.search(r"is_null\(\)|is_object\(\)|is_array\(\)", b.header)
                    for b in chain
                    if b.kind == "if"
                ) or "is_null()" in stmt or "is_object()" in stmt
                guarded_type = guarded_null
                site_kind = "delegate"
            else:
                guarded_present = any(doc_matches(d, doc) and k == key for d, k in present)
                guarded_null = any(doc_matches(d, doc) and k == key for d, k in nonnull)
                guarded_type = any(doc_matches(d, doc) and k == key for d, k in typed)

            stmt_end = statement_end(text, off, min(len(text), off + 400))
            target_type = TYPE_ARG.search(text[off:stmt_end])
            tt = target_type.group(1).strip() if target_type else ""
            # Only a bare nlohmann::json absorbs anything.  `std::map<std::string,
            # nlohmann::json>` does not: constructing the map from a null
            # throws, so a substring test here declared a whole class of
            # fields null-tolerant that are not.
            bare_tt = re.sub(r"^(?:const\s+)?std::optional\s*<(.+)>$", r"\1", tt.strip()).strip()
            json_typed = bare_tt in ("nlohmann::json", "json") or site_kind == "at_bare"

            if site_kind == "delegate":
                absent = NA
                null = OK if guarded_null or json_typed else THROW
                wrong = OK if guarded_type or json_typed else THROW
                guard_desc = (
                    f"delegates to {tt or '?'}::from_json"
                    + ("" if guarded_null else "; document not shape-checked")
                )
            elif site_kind == "value_default":
                absent = OK
                null = OK if json_typed else THROW
                wrong = THROW
                guard_desc = "value(key, default)"
            else:
                absent = OK if guarded_present else THROW
                if json_typed:
                    null = OK
                elif guarded_null:
                    null = OK
                else:
                    null = THROW
                wrong = OK if (guarded_type or json_typed) else THROW
                if guarded_null:
                    guard_desc = "contains + null/type check"
                elif guarded_present:
                    guard_desc = "contains only"
                else:
                    guard_desc = "none (at)"

            fn_body = text[fn_block.start : fn_block.end]
            member = member_target(text, off, stmt_end, stmt)
            constrained = NA
            if member:
                for vm in VALIDATOR_CALL.finditer(fn_body):
                    arg = vm.group(2).replace("->", ".")
                    if arg.split(".")[-1] == member:
                        constrained = vm.group(1)
                        break
            rows.append(
                Row(
                    file=rel,
                    line=line,
                    owner=deserialised_type(chain) or f"({fn_name})",
                    field_name=key,
                    site_kind=site_kind,
                    absent=absent,
                    null=null,
                    wrong_type=wrong,
                    oversized=BOUNDED if size_bounded(fn_body) else UNBOUNDED,
                    guard=guard_desc,
                    fatality=fatality_at(
                        off, resolve_fatality(text, chain, fn_name, index, rel), indirect_regions
                    ),
                    function=fn_name,
                    snippet=raw.splitlines()[line - 1].strip()[:140]
                    if line - 1 < len(raw.splitlines())
                    else "",
                    target_types=named_types(tt),
                    target_hint=tt,
                    member=member,
                    constrained=constrained,
                )
            )

    rows.extend(scan_envelope_predicates(text, raw, starts, root, index, rel, indirect_regions))
    return rows, structs


CONTAINS_CALL = re.compile(rf"([\w\->\.]+?)\.contains\(\s*{KEY}\s*\)")


def scan_envelope_predicates(
    text: str,
    raw: str,
    starts: list[int],
    root: Block,
    index: dict[str, list[FunctionInfo]],
    rel: str,
    indirect_regions: list[tuple[int, int, str]],
) -> list[Row]:
    """Presence tests on envelope keys, and what an explicit null does to them."""
    lines = raw.splitlines()
    out: list[Row] = []
    for m in CONTAINS_CALL.finditer(text):
        key = m.group(2)
        if key not in ENVELOPE_KEYS:
            continue
        off = m.start()
        chain = enclosing_chain(root, off)
        if not chain:
            continue
        fn_name, fn_block = enclosing_function(chain)
        if fn_block is None:
            continue
        doc = normalise_doc(m.group(1))
        if doc not in peer_roots_for(chain[-1], text):
            continue

        verdict, reason = ENVELOPE_KEYS[key]
        stmt = statement_prefix(text, m.end(), chain[-1])
        window = text[max(fn_block.start, off - 200) : off + 200]
        aliases = bool_aliases(text[fn_block.start : fn_block.end])
        checked = any(
            doc_matches(d, doc) and k == key
            for d, k in null_safe_guards(expand_aliases(window, aliases))
        )
        if checked:
            verdict, reason = "ok", "presence test is paired with a null check"

        line = line_of(starts, off)
        out.append(
            Row(
                file=rel,
                line=line,
                owner=f"({fn_name})",
                field_name=key,
                site_kind="envelope_presence",
                absent=OK,
                null=OK if verdict == "ok" else MISROUTE,
                wrong_type=NA,
                oversized=NA,
                guard=reason,
                # A misroute does not throw, so throw-radius does not describe
                # it: the message is routed wrong and, on a correlation path,
                # silently dropped -- the pending request then hangs to its
                # timeout rather than failing loudly.
                fatality=MISROUTE
                if verdict == MISROUTE
                else fatality_at(
                    off,
                    resolve_fatality(text, chain, fn_name, index, rel),
                    indirect_regions,
                ),
                function=fn_name,
                snippet=lines[line - 1].strip()[:140] if line - 1 < len(lines) else "",
            )
        )
    return out


def terminates(body: str) -> bool:
    """Does this block leave the enclosing function or loop iteration?"""
    return bool(re.search(r"\b(return|co_return|throw|continue|break)\b", body))


GET_TO_TARGET = re.compile(r"\.get_to\s*\(\s*[\w]+\s*(?:\.|->)\s*(\w+)\s*\)")
ASSIGN_TARGET = re.compile(r"(?:\.|->)\s*(\w+)\s*=\s*$")


def member_target(text: str, offset: int, end: int, stmt: str) -> str:
    """The C++ member a key is read into, so the field can be typed.

    The wire key and the member name usually match, but not always -- `Icon`
    reads its `source` member from `"src"`.  Guessing from the member name
    produced a baseline document that was missing a required key, which made
    every mode of every field of that type throw for a reason unrelated to
    what the case was testing.
    """
    m = GET_TO_TARGET.search(text[offset:end])
    if m:
        return m.group(1)
    m = ASSIGN_TARGET.search(stmt.rstrip())
    if m:
        return m.group(1)
    return ""


def early_return_guards(
    text: str, chain: list[Block], offset: int, aliases: dict[str, str]
) -> set[tuple[str, str]]:
    """Keys proven present by a preceding `if (!doc.contains(k)) return;`."""
    found = set()
    for blk in chain:
        for sib in blk.children:
            if sib.kind != "if" or sib.end >= offset:
                continue
            body = text[sib.start : sib.end]
            if not re.search(r"\b(return|co_return|throw|continue|break)\b", body):
                continue
            header = expand_aliases(sib.header, aliases)
            for m in re.finditer(rf"!\s*([\w\->\.]+?)\.contains\(\s*{KEY}\s*\)", header):
                found.add((normalise_doc(m.group(1)), m.group(2)))
            # `if (!(a && doc.contains(k)))` and `if (!a || !doc.contains(k))`
            for m in re.finditer(rf"\|\|\s*!\s*([\w\->\.]+?)\.contains\(\s*{KEY}\s*\)", header):
                found.add((normalise_doc(m.group(1)), m.group(2)))
    return found


def early_return_null_guards(text: str, chain: list[Block], offset: int) -> set[tuple[str, str]]:
    """Keys proven non-null by a preceding early-return type/null test."""
    found = set()
    for blk in chain:
        for sib in blk.children:
            if sib.kind != "if" or sib.end >= offset:
                continue
            body = text[sib.start : sib.end]
            if not re.search(r"\b(return|co_return|throw|continue|break)\b", body):
                continue
            for m in re.finditer(
                rf"!\s*([\w\->\.]+?)(?:\.at\(\s*{KEY}\s*\)|\[\s*{KEY}\s*\])\s*\.is_\w+\(\)",
                sib.header,
            ):
                found.add((normalise_doc(m.group(1)), m.group(2) or m.group(3)))
            for m in re.finditer(
                rf"([\w\->\.]+?)(?:\.at\(\s*{KEY}\s*\)|\[\s*{KEY}\s*\])\s*\.is_null\(\)",
                sib.header,
            ):
                found.add((normalise_doc(m.group(1)), m.group(2) or m.group(3)))
    return found


def build_index(files: list[tuple[str, str]]) -> dict[str, list[FunctionInfo]]:
    """Name -> function bodies, for the call-graph walk used by fatality."""
    index: dict[str, list[FunctionInfo]] = {}
    for path, rel in files:
        raw = open(path, "r", encoding="utf-8", errors="replace").read()
        text = strip_comments(raw)
        root = parse_blocks(text)

        def walk(blk: Block) -> None:
            if blk.kind == "function":
                m = re.search(
                    r"(\w+)\s*\([^()]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->[^{]*)?$",
                    blk.header.strip(),
                )
                if m:
                    info = FunctionInfo(
                        name=m.group(1),
                        file=rel,
                        block=blk,
                        body=text[blk.start : blk.end],
                        calls=set(),
                    )
                    info.body_text = text  # type: ignore[attr-defined]
                    index.setdefault(m.group(1), []).append(info)
            for child in blk.children:
                walk(child)

        walk(root)
    return index


# --------------------------------------------------------------------------
# Struct members and the type graph
#
# nlohmann reaches a type's ``from_json`` through ``get<T>()``, which no
# name-based call graph can see: there is no call to `from_json` in the text.
# Without this edge every protocol serialiser reports an unknown blast radius,
# which is the same as reporting nothing.  The type graph supplies the edge.
# --------------------------------------------------------------------------

STRUCT_DECL = re.compile(r"\b(?:struct|class)\s+([A-Z]\w*)\s*(?::([^{]*))?\{")
BASE_NAME = re.compile(r"\b(?:public|protected|private)?\s*([A-Z]\w*)")
MEMBER_DECL = re.compile(
    r"^\s*((?:const\s+)?[\w:]+(?:\s*<[^;]*>)?)\s+(\w+)\s*(?:=\s*[^;]+)?;\s*$",
    re.MULTILINE,
)


def parse_bases(text: str, root: Block) -> dict[str, list[str]]:
    """Type name -> its base classes.

    A derived serialiser delegates to its base -- EnumSchema's from_json calls
    the PrimitiveSchemaDefinition one -- so the base's required fields are
    required of the derived type too.  Without this edge the matrix built a
    baseline missing an inherited key and every case for that type threw on
    the inherited field rather than on the mode under test.
    """
    out: dict[str, list[str]] = {}

    def walk(blk: Block) -> None:
        m = STRUCT_DECL.search(blk.header + "{")
        if m and m.group(2):
            out.setdefault(m.group(1), BASE_NAME.findall(m.group(2)))
        for child in blk.children:
            walk(child)

    walk(root)
    return out


def parse_structs(text: str, root: Block) -> dict[str, list[tuple[str, str]]]:
    """Type name -> [(member type, member name)] for every struct in the file."""
    out: dict[str, list[tuple[str, str]]] = {}

    def walk(blk: Block) -> None:
        m = STRUCT_DECL.search(blk.header + "{")
        if m:
            name = m.group(1)
            body = text[blk.start : blk.end]
            # Only direct members: drop anything inside a nested brace.
            flat = strip_nested(body)
            members = [
                (t.strip(), n)
                for t, n in MEMBER_DECL.findall(flat)
                if t.strip() not in ("return", "co_return")
            ]
            if members or name not in out:
                out[name] = members
        for child in blk.children:
            walk(child)

    walk(root)
    return out


def strip_nested(body: str) -> str:
    """Blank out everything inside nested braces, preserving line structure."""
    out = list(body)
    depth = 0
    for i, ch in enumerate(body):
        if ch == "{":
            depth += 1
            out[i] = " "
        elif ch == "}":
            depth = max(0, depth - 1)
            out[i] = " "
        elif depth > 0 and ch != "\n":
            out[i] = " "
    return "".join(out)


IDENT = re.compile(r"\b([A-Z]\w*(?:::\w+)*)\b")

CONTAINER_NOISE = {"T", "Result", "Params", "Args"}


def named_types(type_expr: str) -> tuple[str, ...]:
    """Capitalised type names mentioned in a template argument.

    ``std::vector<RelatedTaskMetadata>`` yields ``RelatedTaskMetadata``;
    ``std::map<std::string, nlohmann::json>`` yields nothing.
    """
    if not type_expr:
        return ()
    return tuple(
        t
        for t in IDENT.findall(type_expr)
        if t not in CONTAINER_NOISE and not t.startswith("std::")
    )

FATAL_ORDER = [FATAL_PROCESS, FATAL_SESSION, FATAL_MESSAGE, FATAL_UNKNOWN]


def worst(a: str, b: str) -> str:
    for f in FATAL_ORDER:
        if a == f or b == f:
            return f
    return FATAL_UNKNOWN


def propagate_type_fatality(
    rows: list[Row],
    delegations: dict[str, set[str]],
    deserialisable: set[str],
) -> None:
    """Give every protocol row the blast radius of the worst entry that reaches it.

    An entry is a site outside the protocol headers that hands a document to
    ``get<T>()``; its blast radius is already resolved lexically.  That radius
    then flows to T and to every type T's serialiser delegates to.
    """
    seeds: dict[str, str] = {}
    for r in rows:
        if r.fatality == FATAL_UNKNOWN:
            continue
        for t in r.target_types:
            if t in deserialisable:
                seeds[t] = worst(seeds.get(t, FATAL_UNKNOWN), r.fatality)

    resolved = dict(seeds)
    frontier = list(seeds.items())
    guard = 0
    while frontier and guard < 10000:
        guard += 1
        t, f = frontier.pop()
        for nxt in delegations.get(t, ()):  # noqa: B007
            new = worst(resolved.get(nxt, FATAL_UNKNOWN), f)
            if resolved.get(nxt) != new:
                resolved[nxt] = new
                frontier.append((nxt, new))

    for r in rows:
        if r.fatality == FATAL_UNKNOWN and r.owner in resolved:
            r.fatality = resolved[r.owner]
            r.fatality_basis = "reached"


def strict_serialisers(files: list[tuple[str, str]]) -> set[str]:
    """Types whose from_json raises rather than accepting whatever it is given."""
    strict: set[str] = set()
    for path, _rel in files:
        text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        root = parse_blocks(text)

        def walk(blk: Block) -> None:
            m = FROM_JSON_SIG.search(blk.header)
            if m and re.search(r"\bthrow\b", text[blk.start : blk.end]):
                strict.add(m.group(2).split("::")[-1])
            for child in blk.children:
                walk(child)

        walk(root)
    return strict


def enum_table(text: str) -> dict[str, str]:
    """Enum type -> its first wire string, which is also its fallback value."""
    out: dict[str, str] = {}
    for m in ENUM_MACRO.finditer(text):
        first = re.search(r'"([^"]+)"', m.group(2))
        if first:
            out[m.group(1).split("::")[-1]] = first.group(1)
    return out


def fill_target_hints(
    rows: list[Row], structs: dict[str, list[tuple[str, str]]]
) -> None:
    """Type a site that reads through ``get_to(x.member)`` from the member.

    ``at("data").get_to(params.data)`` names no type at the call site, so the
    site alone cannot say whether a null is tolerated.  The member's
    declaration can: ``nlohmann::json data`` absorbs a null, ``TaskStatus
    status`` coerces one, ``std::string`` rejects it.
    """
    for r in rows:
        if r.target_hint or not r.member or not r.owner:
            continue
        for ctype, mname in structs.get(r.owner, []):
            if mname == r.member:
                r.target_hint = ctype
                r.target_types = r.target_types or named_types(ctype)
                break


def refine_targets(
    rows: list[Row],
    enums: dict[str, str],
    required_types: set[str],
    known: set[str],
    strict: set[str],
) -> None:
    """Correct verdicts that depend on what the field deserialises *into*.

    Two cases read as a throw from the call site but are not:

    * An enum built with NLOHMANN_JSON_SERIALIZE_ENUM does not throw on an
      unrecognised value -- it falls back to the first enumerator.  Null and
      wrong-typed input are accepted and silently coerced, which is a quieter
      failure than a throw and worth naming as its own class.
    * A type whose serialiser reads only optional fields accepts a null
      document: `contains()` on a null returns false for every key, so every
      field is skipped and an empty value is produced.
    """
    for r in rows:
        if r.site_kind in ("delegate", "envelope_presence", "macro_required"):
            continue
        # The refinement is about what the field deserialises into as a whole.
        # `std::vector<Role>` is not a Role: constructing the vector from a
        # null throws before any enum coercion can happen, so matching on a
        # template argument inverted the verdict for every container of an
        # enum or of an all-optional struct.
        whole = re.sub(r"^(?:const\s+)?std::optional\s*<(.+)>$", r"\1", r.target_hint.strip()).strip()
        target = whole.split("::")[-1] if whole else None
        if target is None:
            continue
        if whole not in ("nlohmann::json", "json") and target not in enums and target not in known:
            continue
        if whole in ("nlohmann::json", "json"):
            r.null = OK
            r.wrong_type = OK
            r.guard += "; the member is a raw json value and absorbs anything"
        elif target in enums:
            r.null = OK
            r.wrong_type = OK
            r.guard += "; enum coerces an unrecognised value to the first enumerator"
        elif target not in required_types and target not in strict:
            r.null = OK
            r.wrong_type = OK
            r.guard += "; target has no required field, so a null parses as an empty value"


def collect_files(base: str) -> list[tuple[str, str]]:
    files = []
    for rootdir in SCAN_ROOTS:
        top = os.path.join(base, rootdir)
        if not os.path.isdir(top):
            continue
        for dirpath, _dirs, names in os.walk(top):
            for name in sorted(names):
                if name.endswith(SOURCE_SUFFIXES):
                    full = os.path.join(dirpath, name)
                    files.append((full, os.path.relpath(full, base)))
    return sorted(files, key=lambda p: p[1])


def export_revision(repo: str, rev: str) -> str:
    tmp = tempfile.mkdtemp(prefix="json-census-")
    proc = subprocess.run(
        ["git", "-C", repo, "archive", rev],
        stdout=subprocess.PIPE,
        check=True,
    )
    with tarfile.open(fileobj=io.BytesIO(proc.stdout)) as tar:
        tar.extractall(tmp)
    return tmp


# --------------------------------------------------------------------------
# The rediscovery gate
#
# Each entry is a defect that was found by hand, at cost, after it reached a
# user.  The census must find every one of them again from source alone.  The
# entries name a file, the owning type or function, the field and the class --
# never a line number, so that the gate keeps working when the code moves.
# That matters concretely: two of these surfaced only because a refactor moved
# them into a new file, where a diff-scoped review mistook a pre-existing
# defect for an added line.
# --------------------------------------------------------------------------

# Each entry is (name, component, owner, field, class, note).
#
# `component` is a path fragment the matching row's file must contain. It is a
# component, not a file: keying to a file would let exactly the refactor that
# exposed F2 -- moving code from client.hpp to client.cpp -- turn a
# rediscovery into a silent pass. But it cannot be dropped either. Both the
# client and the server have a `dispatch_response`, and with no component
# constraint the server's reverse-RPC gate matched the client's row and
# reported PASS while covering nothing.
#
# `owner` is the deserialised type, or the enclosing function, or a tuple when
# a defect has more than one manifestation across revisions.
KNOWN_DEFECTS = [
    (
        "F1-progress",
        "protocol",
        "ProgressNotificationParams",
        "total",
        "null-fragile",
        "a present null total ended the client session",
    ),
    (
        "F1-progress-message",
        "protocol",
        "ProgressNotificationParams",
        "message",
        "null-fragile",
        "a present null message ended the client session",
    ),
    (
        "F1-error-message",
        "protocol",
        "Error",
        "message",
        "required",
        "Error::from_json requires message via at(), so a peer error without one throws",
    ),
    (
        # Either manifestation counts: at the merge base the reverse-RPC
        # correlation path is dispatch_response alone, and the envelope helper
        # that carries the contains(result) == contains(error) predicate is
        # newer.
        "F3-server-reverse-rpc",
        "server",
        ("is_valid_response_envelope", "dispatch_response"),
        "error",
        "misroute",
        "a response carrying a real result plus 'error': null is rejected and "
        "dropped silently on the correlation path, hanging a server-initiated "
        "sampling, roots or elicitation request to its timeout",
    ),
    (
        "F2-client-error",
        "client",
        "dispatch_response",
        "error",
        "null-fragile",
        "pre-existing at merge base; surfaced only when a refactor moved it",
    ),
    (
        "F2-client-params",
        "client",
        "dispatch_notification",
        "params",
        "null-fragile",
        "pre-existing at merge base; surfaced only when a refactor moved it",
    ),
]


def run_self_test(rows: list[Row], rev: str, stream) -> int:
    """Require the census to rediscover every defect that was found by hand."""
    failures = 0
    stream.write(f"rediscovery gate against {rev}\n")
    for name, component, owner, field_name, expected, note in KNOWN_DEFECTS:
        owners = (owner,) if isinstance(owner, str) else owner
        hits = [
            r
            for r in rows
            if r.field_name == field_name
            and r.defect_class == expected
            and component in r.file.replace(os.sep, "/")
            and (
                owner is None
                or any(o in (r.owner, r.function, f"({r.function})") for o in owners)
            )
        ]
        if hits:
            h = hits[0]
            stream.write(
                f"  PASS  {name:22s} {h.file}:{h.line} {h.defect_class} "
                f"fatality={h.fatality}\n"
            )
        else:
            failures += 1
            stream.write(
                f"  FAIL  {name:22s} no {expected} row for "
                f"{owner or '*'}.{field_name} anywhere under '{component}'\n"
                f"        ({note})\n"
            )
    stream.write(
        f"rediscovered {len(KNOWN_DEFECTS) - failures}/{len(KNOWN_DEFECTS)}\n"
    )
    return failures + check_asymmetry(rows, stream)


def check_asymmetry(rows: list[Row], stream) -> int:
    """Hold the line between 'error' and 'result'.

    Null-tolerance applies to `"error"` and not to `"result"`: `"result": null`
    is a legitimate empty result in JSON-RPC, while `"error": null` is not an
    error.  A rule that flattens the two closes one defect and opens another,
    so the distinction is checked rather than trusted -- including against a
    future edit to ENVELOPE_KEYS itself.
    """
    failures = 0
    if ENVELOPE_KEYS.get("result", (None,))[0] != "ok":
        failures += 1
        stream.write(
            "  ASYMMETRY FAIL  'result' is not marked ok: a null result is a "
            "legitimate empty result and must not be treated as absent\n"
        )
    if ENVELOPE_KEYS.get("error", (None,))[0] != MISROUTE:
        failures += 1
        stream.write(
            "  ASYMMETRY FAIL  'error' is not marked misroute: a null error is "
            "not an error and a presence test misroutes it\n"
        )

    bad = [r for r in rows if r.field_name == "result" and r.null == MISROUTE]
    if bad:
        failures += 1
        stream.write(f"  ASYMMETRY FAIL  {len(bad)} 'result' rows classified misroute:\n")
        for r in bad[:10]:
            stream.write(f"      {r.file}:{r.line} {r.function}\n")

    errors = len([r for r in rows if r.field_name == "error" and r.null == MISROUTE])
    results = len([r for r in rows if r.field_name == "result"])
    if not failures:
        stream.write(
            f"asymmetry held: {errors} 'error' presence tests flagged misroute, "
            f"0 of {results} 'result' rows flagged\n"
        )
    return failures


# --------------------------------------------------------------------------
# What this census cannot see
#
# An enumeration whose blind spots are undocumented becomes the next false
# comfort.  These are printed by --blind-spots so they travel with the tool.
# --------------------------------------------------------------------------

BLIND_SPOTS = [
    (
        "Indirect dispatch is attributed, not traced",
        "A handler stored in a std::function and invoked out of a map cannot be "
        "followed by name. The census attributes to every peer-receiving lambda "
        "the blast radius of the function that dispatches out of a handler "
        "registry in the same file. That is right for the registries here, and "
        "it would be wrong for a handler invoked from somewhere else.",
    ),
    (
        "Fatality is static, and public entry points have no caller",
        "Server::dispatch is public: an embedder may call it from anywhere, so "
        "no static analysis can bound what a throw there costs. Rows reached "
        "only that way read 'unknown', which means unproven, not safe.",
    ),
    (
        "Peer-controlled is asserted for nlohmann::json parameters",
        "Every function parameter of json type is treated as peer-controlled. "
        "In this codebase each one is reached from json::parse of bytes off the "
        "wire, but the census asserts that rather than proving it. A json "
        "assembled locally and passed in would be over-reported, never missed.",
    ),
    (
        "Value domains are recognised only through validate_* calls",
        "A field constrained by an inline comparison rather than a validate_* "
        "helper reads as unconstrained, so its oversized column will claim an "
        "arbitrary value is accepted when it is not.",
    ),
    (
        "Size limits are looked for lexically, in the same function",
        "A ceiling applied by the transport before the document reaches the "
        "deserialiser is invisible here. Every row says 'unbounded' because no "
        "deserialisation site bounds anything -- that is a statement about "
        "these sites, not about the system.",
    ),
    (
        "Only include/ and src/ are scanned",
        "Deserialisation in examples/, conformance/ or benchmark/ is out of "
        "scope. So is anything reached through a template the census cannot "
        "resolve to a concrete type.",
    ),
    (
        "The matrix cannot synthesise a baseline for shape-dispatching types",
        "std::variant serialisers that choose an arm by inspecting the document "
        "are excluded by name with a reason. They are the types where a "
        "hand-written case is still required.",
    ),
]


# --------------------------------------------------------------------------
# Dispositions
#
# A flagged row that nobody has looked at is not a finding, it is a backlog
# entry pretending to be one.  Every flagged row must match a rule in
# scripts/json_census_dispositions.json saying what it means, and the check
# fails if any does not -- so a newly intolerant site cannot arrive without
# someone deciding about it.
# --------------------------------------------------------------------------

DISPOSITIONS_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "json_census_dispositions.json"
)


def load_dispositions(path: str) -> list[dict]:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)["rules"]


def disposition_for(row: Row, rules: list[dict]) -> Optional[dict]:
    for rule in rules:
        if all(
            str(getattr(row, k, None)) == str(v) or (k == "defect_class" and row.defect_class == v)
            for k, v in rule["match"].items()
        ):
            return rule
    return None


def check_dispositions(rows: list[Row], rules: list[dict], stream) -> int:
    flagged = [r for r in rows if r.flagged]
    tally: dict[str, int] = {}
    unmatched: list[Row] = []
    for r in flagged:
        rule = disposition_for(r, rules)
        if rule is None:
            unmatched.append(r)
            continue
        key = f"{rule['disposition']}: {rule['reason'][:60]}..."
        tally[key] = tally.get(key, 0) + 1

    stream.write(f"flagged rows: {len(flagged)}\n")
    for key, count in sorted(tally.items(), key=lambda kv: -kv[1]):
        stream.write(f"  {count:4d}  {key}\n")
    if unmatched:
        stream.write(f"\n{len(unmatched)} flagged rows have no disposition:\n")
        for r in unmatched[:40]:
            stream.write(
                f"  {r.file}:{r.line} {r.owner}.{r.field_name} "
                f"[{r.defect_class}] {r.guard[:50]}\n"
            )
        stream.write(
            "\nAdd a rule to scripts/json_census_dispositions.json saying whether "
            "each is a defect to fix, correct behaviour to accept, or a known "
            "cost to record.\n"
        )
    return len(unmatched)


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

COLUMNS = [
    "file",
    "line",
    "owner",
    "field_name",
    "site_kind",
    "absent",
    "null",
    "wrong_type",
    "oversized",
    "guard",
    "fatality",
    "fatality_basis",
    "constrained",
    "function",
]


def emit(rows: list[Row], fmt: str, stream) -> None:
    if fmt == "json":
        json.dump([asdict(r) for r in rows], stream, indent=2)
        stream.write("\n")
        return
    if fmt == "csv":
        writer = csv.DictWriter(stream, fieldnames=COLUMNS + ["snippet"])
        writer.writeheader()
        for r in rows:
            writer.writerow({k: getattr(r, k) for k in COLUMNS + ["snippet"]})
        return
    widths = {c: max(len(c), *(len(str(getattr(r, c))) for r in rows)) for c in COLUMNS} if rows else {}
    stream.write("  ".join(c.ljust(widths[c]) for c in COLUMNS) + "\n")
    stream.write("  ".join("-" * widths[c] for c in COLUMNS) + "\n")
    for r in rows:
        stream.write("  ".join(str(getattr(r, c)).ljust(widths[c]) for c in COLUMNS) + "\n")


def build_rows(base: str) -> list[Row]:
    """The census of ``base``: every row, with every post-pass applied.

    Both the census CLI and the matrix generator go through here.  When the
    generator built its rows directly from scan_file() it silently skipped the
    target-type and fatality refinements, so the matrix asserted verdicts the
    census no longer held -- 54 cases failing against a prediction nothing was
    still making.
    """
    files = collect_files(base)
    index = build_index(files)

    rows: list[Row] = []
    structs: dict[str, list[tuple[str, str]]] = {}
    enums: dict[str, str] = {}
    for path, rel in files:
        file_rows, file_structs = scan_file(path, rel, index)
        rows.extend(file_rows)
        for name, members in file_structs.items():
            structs.setdefault(name, members)
        enums.update(
            enum_table(strip_comments(open(path, encoding="utf-8", errors="replace").read()))
        )

    # Types a peer can steer a document into, and what each delegates to.
    deserialisable = {r.owner for r in rows if r.owner and not r.owner.startswith("(")}
    delegations: dict[str, set[str]] = {}
    for r in rows:
        if not r.owner or r.owner.startswith("("):
            continue
        delegations.setdefault(r.owner, set()).update(
            t for t in r.target_types if t in deserialisable
        )
        # Macro-defined serialisers reach their members' types through the
        # members' declarations rather than through a visible get<T>.
        for mtype, mname in structs.get(r.owner, []):
            delegations[r.owner].update(
                t for t in named_types(mtype) if t in deserialisable
            )

    required_types = {r.owner for r in rows if r.absent == THROW and r.owner}
    fill_target_hints(rows, structs)
    # A hand-written serialiser that raises on a shape it does not accept --
    # RequestId rejects anything that is not a string or an integer -- is not
    # "a type with no required field", even though it reads no field at all.
    strict_types = strict_serialisers(collect_files(base))
    refine_targets(rows, enums, required_types, deserialisable, strict_types)
    propagate_type_fatality(rows, delegations, deserialisable)
    rows.sort(key=lambda r: (r.file, r.line, r.field_name))
    return rows


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--rev", help="census a git revision instead of the working tree")
    ap.add_argument("--format", choices=("table", "json", "csv"), default="table")
    ap.add_argument("--only-flagged", action="store_true", help="rows that reject some well-formed input")
    ap.add_argument("--null-fragile", action="store_true", help="rows that accept absent but reject null")
    ap.add_argument(
        "--require",
        action="append",
        default=[],
        metavar="FILE:LINE",
        help="exit non-zero unless this site is present and flagged",
    )
    ap.add_argument("--summary", action="store_true")
    ap.add_argument(
        "--blind-spots",
        action="store_true",
        help="print what this census cannot see",
    )
    ap.add_argument(
        "--check-dispositions",
        action="store_true",
        help="require every flagged row to have a disposition (exit 1 if any does not)",
    )
    ap.add_argument("--dispositions", default=DISPOSITIONS_PATH)
    ap.add_argument(
        "--fix-list",
        action="store_true",
        help="print only the rows whose disposition is 'fix'",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="require the census to rediscover every hand-found defect (exit 1 if not)",
    )
    args = ap.parse_args(argv)

    if args.blind_spots:
        for title, detail in BLIND_SPOTS:
            sys.stdout.write(f"{title}\n")
            for line in re.findall(r".{1,76}(?:\s|$)", detail):
                sys.stdout.write(f"    {line.strip()}\n")
            sys.stdout.write("\n")
        return 0

    base = export_revision(args.repo, args.rev) if args.rev else args.repo
    rows = build_rows(base)
    files = collect_files(base)

    rows.sort(key=lambda r: (r.file, r.line, r.field_name))

    selected = rows
    if args.null_fragile:
        selected = [r for r in rows if r.absent == OK and r.null in (THROW, MISROUTE)]
    elif args.only_flagged:
        selected = [r for r in rows if r.flagged]

    status = 0
    rules = load_dispositions(args.dispositions)
    if args.check_dispositions:
        status |= 1 if check_dispositions(rows, rules, sys.stderr) else 0
    if args.fix_list:
        selected = [
            r
            for r in rows
            if r.flagged
            and (disposition_for(r, rules) or {}).get("disposition") == "fix"
        ]
    if args.self_test:
        status |= 1 if run_self_test(rows, args.rev or "the working tree", sys.stderr) else 0

    for req in args.require:
        fname, _, lineno = req.rpartition(":")
        hits = [
            r for r in rows if r.file.endswith(fname) and r.line == int(lineno) and r.flagged
        ]
        if hits:
            sys.stderr.write(
                f"REQUIRE ok    {req}: {hits[0].owner}.{hits[0].field_name} "
                f"{hits[0].defect_class} fatality={hits[0].fatality}\n"
            )
        else:
            sys.stderr.write(f"REQUIRE FAIL  {req}: not flagged by the census\n")
            status = 1

    if args.summary:
        total = len(rows)
        flagged = len([r for r in rows if r.flagged])
        fragile = len([r for r in rows if r.absent == OK and r.null in (THROW, MISROUTE)])
        sys.stderr.write(
            f"sites={total} flagged={flagged} null-fragile={fragile} "
            f"files={len(files)} rev={args.rev or 'working tree'}\n"
        )

    emit(selected, args.format, sys.stdout)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
