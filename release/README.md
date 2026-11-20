# Release tooling

This directory keeps release logic out of GitHub Actions YAML. Construction,
manifest, signing-boundary, and package-validation modules are deterministic
and testable offline. Small provider adapters perform explicit read or publish
operations only from their protected workflow jobs; credentials stay in GitHub
environments and are never stored here. Native package builds run in the
locked builder images from `native-builders/`; only the Conan builder container
is started with network access.

## Making a release

1. Add a `### Highlights` part to the changelog's Unreleased entries: the few
   lines a reader of the release page needs. On a branch from `main`, run
   `python3 -m release.version_bump prepare X.Y.Z`. It writes `VERSION`, turns
   the Unreleased entries into the dated `X.Y.Z` section, and updates the
   documentation that names the latest release. Add migration notes for
   breaking changes to the new section under a heading such as
   `### Migrating from 0.3.0`, then open the release pull request. Its CI
   builds every package, because `VERSION` changed.
2. Merge it. The `Release dry run` workflow starts a dry run of `vX.Y.Z` on
   `main`. Approve the run when it asks; nothing is published.
3. When the dry run passes, run `python3 -m release.tag_release` with the
   release keyring (`--gnupghome` or `MCP_RELEASE_GNUPGHOME`). It signs
   `vX.Y.Z` on the head of `main`, checks the signature and the repository
   with the workflow's readiness check, and asks before it pushes the tag and
   before it starts the release. The release publishes to the channels the
   `RELEASE_CHANNELS` repository variable names, separated by spaces, from
   `conan2 apt rpm aur homebrew chocolatey`; the script stops before the push
   when the variable is missing or names another channel.
4. Approve the release run, then the publication once the candidate is
   verified.

The release workflow writes the GitHub Release notes from the section: its
highlights, a link to its migration notes when it has them, and links to the
full section and the installation guide at the tag. A section without
highlights fails the candidate, so the dry run reports it before the tag is
signed.

CI runs `python3 -m release.version_bump check`, through the release tests,
so the documentation always names the version in `VERSION`. A release
candidate (`X.Y.Z-rc.N`) changes only `VERSION` and the changelog, has no dry
run, and publishes only the GitHub Release.

Each release run asks for two reviews: the `release-approval` environment
before any job reads a release secret or signs, and the
`release-publication-approval` environment before the GitHub Release and the
channels. Those two environments require a maintainer's review. The
environments that hold the release secrets (`release-signing`,
`release-github`, `release-cloudsmith`, `release-aur`, `release-homebrew`,
`release-chocolatey`, `release-control-dispatch`) need no review of their own,
because every job that uses them waits for one of the two approvals. Every
release environment allows only protected branches.

## Tests

Run its tests with:

```sh
python3 -m unittest discover -s test/release -p 'test_*.py' -v
```

The command-line entry point is `python3 -m release.cli`. Its main operations
validate canonical versions; build deterministic source archives, checksum
manifests, and SPDX/CycloneDX SBOMs; construct the signed-manifest input; and
render one strict package template at a time.

Workflow-only modules run through `scripts/run_release_tool.py`, which uses
isolated Python and an explicit module allow-list; modules that call the GitHub
API also require GitHub CLI 2.96.0 or newer. Build identities are checked against the target catalog in
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

`scripts/release/sign_release.sh` signs `release-manifest.json` and the archives
first, checksums all payloads plus those signatures, and signs `SHA256SUMS`
last. `verify_candidate` checks the `SHA256SUMS` signature and every listed
digest before it parses any other candidate file. Before the first candidate of
a stable release is signed, every provider is preflighted, whether or not it is
selected, so the signed publication contract only carries verified coordinates.
GitHub, Cloudsmith, and AUR publishers read the public object back before
reporting success; Cloudsmith RPMs are compared by header and payload digest
because the repository re-signs them. Homebrew, Chocolatey, and ConanCenter
changes belong to their control repositories; this repository dispatches a
signed release identity to the publisher workflow on each repository's
protected `main`, at the head commit it reads just before dispatching.
