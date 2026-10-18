# Offline release tooling

This directory contains provider-independent release construction and policy
code. It holds no publishing credentials, and its tests run offline. Native
package builds run in the locked builder images from `native-builders/`; only
the Conan builder container is started with network access.

Run its tests with:

```sh
python3 -m unittest discover -s test/release -p 'test_*.py' -v
```

The command-line entry point is `python3 -m release.cli`. Its main operations
validate canonical versions; build deterministic source archives, checksum
manifests, and SPDX/CycloneDX SBOMs; construct the signed-manifest input; and
render one strict package template at a time.

Native package construction and validation (`native_build`, `native_builder`,
`aur_validation`, `conan_validation`, and `package_validation`) run through
`scripts/run_release_tool.py`, which uses isolated Python and an explicit module
allow-list. Build identities are checked against the target catalog in
`packaging/targets.json`. The Windows SDK archive is built by
`scripts/release/build_windows.ps1` from the locked Conan inputs in `windows/`.

`windows/conan.lock` is shared by the Windows build and by both Conan candidate
checks, so it must resolve the repository `conanfile.py` and the ConanCenter
recipe in `packaging/conan-center/all/` with the Windows and Linux profiles,
static and shared. After changing a dependency, extend it with Conan 2.30.0
from `windows/conan-requirements.txt`, keeping the other recipe revisions:

```sh
conan lock create . --lockfile=release/windows/conan.lock \
  --lockfile-partial --lockfile-clean --lockfile-out=release/windows/conan.lock \
  -pr:h release/windows/conan-msvc-release.profile \
  -pr:b release/windows/conan-msvc-release.profile
```

Then repeat `conan lock create` with `--lockfile-partial` (without
`--lockfile-clean`) for a rendered copy of the ConanCenter recipe
(`--version <version>`) under both profiles and both values of
`-o '&:shared=True|False'`, and confirm each combination with
`conan graph info` and the final lock.

Examples:

```sh
python3 -m release.cli validate-version 0.2.0 --stable-only
python3 -m release.cli source-archives \
  --root . \
  --inventory release/source-archive-inventory.txt \
  --version 0.2.0 \
  --source-date-epoch 1700000000 \
  --output-dir dist
python3 -m release.cli checksums --output dist/SHA256SUMS dist/*
```

Signing and provider publication deliberately remain outside this module. The
workflow must sign `release-manifest.json` first, checksum all payloads plus the
manifest and its signature, and sign `SHA256SUMS` last. Publisher jobs must use
the policy helpers to treat an absent object as publishable, one digest- and
manifest-identical object as an idempotent skip, and every mismatch or
ambiguous result as a manual-action block.
