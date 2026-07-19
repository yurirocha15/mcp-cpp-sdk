# Native release builders

Release jobs run on fresh GitHub-hosted `ubuntu-24.04`,
`ubuntu-24.04-arm`, and `windows-2022` machines. Linux distribution packages
are built inside the dependency-complete images listed in `lock.json`; release
jobs pull those images by digest, inspect the resolved repository digest and
local image ID, disable container networking, and include that identity in the
signed release evidence.

`UNRESOLVED` is an intentional bootstrap sentinel. It is accepted by offline
source validation so the bootstrap change can be reviewed, but every release
dispatch fails before package construction until both digest stages are
complete. `release.native_builder_bootstrap` performs the registry operations
and emits evidence or a proposed lock; it never edits the reviewed lock:

1. On each target's listed hosted runner, run `resolve-base`. It reads the
   registry manifest index and selects the exact `linux/amd64` or
   `linux/arm64` child digest, never the multi-platform index digest. Collect
   exactly one evidence JSON file per lock target, run `merge --stage base`,
   review the proposed lock, and merge that first lock update.
2. Check out that reviewed commit, authenticate Docker only to the dedicated
   GHCR builder namespace, and run `build-image` once per target. It builds
   without cache from `base_image@base_digest`, labels the image with the full
   source commit, pushes a staging tag, reads the single-platform manifest
   back, and verifies its content digest. Collect the complete evidence set,
   run `merge --stage image --source-commit <full SHA>`, review the proposed
   lock, make the single GHCR `mcp-cpp-sdk-release-builders` package publicly
   readable, and merge the second lock update. All targets are staging tags in
   that one package; release jobs consume only the locked manifest digests.

The Linux Conan validator is a separate x86_64 builder in the same lock. Its
Python 3.12 dependency graph and Conan 2.30.0 wheel are hash-locked in
`conan-requirements.txt`. The Windows source build instead installs the
platform-specific hash lock from `release/windows/conan-requirements.txt` on
the fresh `windows-2022` runner.

The Arch container deliberately runs as UID 1001, matching the current hosted
Linux runner. Every invocation performs a real bind-mounted output write probe
before building, so a future runner UID change fails explicitly instead of
losing artifacts. Official AUR validation is x86_64 only.

Never use a mutable tag in the release workflow. Builder publication is an
administrative bootstrap operation, not a package release, and must not be run
from untrusted pull-request code. Builder bootstrap credentials must never be
passed as build arguments, environment variables, or mounted files.
