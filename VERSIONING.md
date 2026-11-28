# Versioning and Compatibility Policy

`mcp-cpp-sdk` follows Semantic Versioning. The `VERSION` file is authoritative,
and release tags use `vMAJOR.MINOR.PATCH` or `vMAJOR.MINOR.PATCH-rc.NUMBER`.
Protocol revision numbers and SDK release numbers are independent.

## Public compatibility surface

The compatibility surface includes installed headers, exported CMake and
pkg-config targets, documented compiler and dependency floors, serialized MCP
behavior, and documented command-line interfaces. Test helpers, benchmark
adapters, conformance fixtures, source files under `src/`, and symbols in a
`detail` namespace are not public API.

## Before 1.0

- Minor releases may make breaking source or behavior changes when the release
  notes include migration guidance.
- Patch releases contain compatible fixes and documentation changes.
- Release candidates are not stable and may change before the matching final
  release.

The project will not publish `1.0.0` until client and server conformance,
public-API review, packaging, and the current protocol transition meet the
gates in `ROADMAP.md`.

## From 1.0 onward

- Major releases may contain breaking changes.
- Minor releases add functionality while preserving supported source and wire
  compatibility.
- Patch releases contain compatible fixes and security updates.
- The shared library's major SOVERSION identifies its ABI compatibility line.
  Consumers that require ABI stability should use a matching major line and a
  supported toolchain configuration.

Public APIs are deprecated in headers and release notes before removal. Except
for urgent security or protocol-correctness fixes, removal occurs no sooner
than the next major release and after at least one minor release or 90 days,
whichever is longer. Protocol features follow the MCP feature lifecycle;
deprecated features remain available for their required compatibility window
and include migration guidance.

A deprecated declaration is marked `MCP_DEPRECATED(...)`, so users get a
compiler warning, and has an `@deprecated` Doxygen tag. The message of an SDK
deprecation reads `use X; removal no earlier than Y`. The message of a protocol
feature names the proposal that deprecates it, the revision that does, its
earliest removal and the migration path, as the specification's registry of
deprecated features gives them. The SDK keeps a deprecated protocol feature at
least until its earliest removal, and reports its first use at run time as
well (see the Deprecations guide). Defining `MCP_DISABLE_DEPRECATION_ATTRIBUTES`
turns the attribute off; the run-time notice stays.

`scripts/deprecation_registry.json` lists every deprecation. CI rejects a
listed declaration without the attribute and its required message, an
attribute that no entry lists, a public identifier of a deprecated feature that
the registry does not classify, and an `@deprecated` tag without the attribute.
A data member or an enumerator whose attribute would warn in code that does
not use it is listed as reported at run time only, with the reason. Every
deprecation is listed under `Deprecated` in `CHANGELOG.md` with the release or
protocol revision that may remove it.

## Release evidence

The release gate requires an updated `CHANGELOG.md`, passing platform and
package-consumer CI, recorded supported MCP protocol revisions, and reviewed
client/server conformance results. The current CI artifacts are regression
evidence with limited retention, not an immutable release archive. Linking
durable conformance evidence from every release becomes mandatory once release
automation archives it. Any intentional compatibility exception is called out
explicitly in the release notes.
