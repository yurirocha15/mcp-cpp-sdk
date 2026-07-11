# Offline release tooling

This directory contains provider-independent release construction and policy
code. It performs no network requests and holds no publishing credentials.

Run its tests with:

```sh
python3 -m unittest discover -s test/release -p 'test_*.py' -v
```

The command-line entry point is `python3 -m release.cli`. Its main operations
validate canonical versions, broker dispatch requests, and release ledgers;
build deterministic source archives, checksum manifests, and SPDX/CycloneDX
SBOMs; construct the signed-manifest input; and render one strict package
template at a time.

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
