# Changelog

All notable changes are documented here. This project follows Semantic
Versioning; a `0.x` version is a stable release unless its version has an
`-rc.N` suffix.

## [Unreleased]

## [0.2.0] - TBD

### Added

- Shared and static library variants with installed CMake and pkg-config
  metadata.
- A manually dispatched, fail-closed release pipeline for GitHub Release,
  ConanCenter contribution, APT/DEB, RPM, AUR, Homebrew, and Chocolatey.
- Deterministic source assets, checksums, signatures, SBOMs, provenance, and
  immutable release-anchor verification.

### Changed

- The canonical project version is now read from the root `VERSION` file.
- The documented minimum toolchains and dependency versions are explicit.
