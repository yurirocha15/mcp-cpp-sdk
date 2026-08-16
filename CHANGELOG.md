# Changelog

All notable changes are documented here. This project follows Semantic
Versioning; a `0.x` version is a stable release unless its version has an
`-rc.N` suffix.

## [Unreleased]

### Added

- Fixtures and a pinned harness for the official MCP conformance runner
  (`@modelcontextprotocol/conformance@0.1.16`, spec revision `2025-11-25`),
  with a regression baseline in `conformance/expected-failures.yml`, a
  `--conformance` build flag, and a CI workflow that fails on any drift.
- Secure random generation and serialized transport-write helpers under
  `mcp::detail`.
- Reproducible cross-SDK benchmark tooling (order counterbalancing,
  environment/container capture, resource-headroom validation, protocol
  verification) and audited benchmark results.
- Project policy documents: `ROADMAP.md` (tier gates), `MAINTENANCE.md`
  (triage SLAs), `VERSIONING.md` (compatibility surface), and
  `DEPENDENCY_POLICY.md` (runtime dependency floors).
- GitHub issue templates and a label manifest (`.github/labels.yml`).

### Changed

- Implementation moved out of oversized headers into compiled translation
  units (OAuth, client runtime, protocol tools, memory transport, HTTP
  types); protocol models and typed handler templates remain header-based.
- Documentation guides and feature examples refreshed to match the compiled
  runtime split.

## [0.2.0] - TBD

### Added

- Shared and static library variants with installed CMake and pkg-config
  metadata.
- A manually dispatched, fail-closed release pipeline for GitHub Release,
  ConanCenter contribution, APT/DEB, RPM, AUR, Homebrew, and Chocolatey.
- Deterministic source assets, checksums, signatures, SBOMs, provenance, and
  release-ledger support.

### Changed

- The canonical project version is now read from the root `VERSION` file.
- The documented minimum toolchains and dependency versions are explicit.
