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

## Release evidence

The release gate requires an updated `CHANGELOG.md`, passing platform and
package-consumer CI, recorded supported MCP protocol revisions, and reviewed
client/server conformance results. The current CI artifacts are regression
evidence with limited retention, not an immutable release archive. Linking
durable conformance evidence from every release becomes mandatory once release
automation archives it. Any intentional compatibility exception is called out
explicitly in the release notes.
