# Homebrew tap bootstrap

This template becomes the public `yurirocha15/homebrew-mcp-cpp-sdk` tap. Its
trusted workflow accepts only a source-App-authored formula PR branch and exact
head, verifies the immutable source release, builds bottles without publisher
credentials, uploads only digest-bound same-run artifacts, and pauses in a
second protected environment before finalization.

Publishing is dispatched only through the asset-free immutable GitHub Release
tag `release-control-v1`. A credential-free first job requires that exact tag,
its immutable commit SHA supplied by the source preflight, and the exact tagged
workflow identity. Protected environments accept deployments from only that
tag; the tag ruleset blocks updates and deletion without bypass actors.

Bottle builders checkout only the prevalidated exact formula head and receive
no environment secret or package-write permission. The upload job receives no
checkout and consumes only same-run artifacts whose IDs and SHA-256 values are
recorded in its handoff. Every artifact name includes its producer run ID and
attempt. A failed-job rerun may reuse the newest successful bottle for each
matrix tag from an earlier attempt of the same run; it cannot adopt artifacts
from another workflow run.

Bottle upload and anonymous public GHCR tag, manifest, and bottle-archive
digest verification complete before the contents-write App token is minted.
That token is available only to the fixed formula API update. The finalizer
belongs to the same original workflow run, performs no build or upload,
revalidates every recorded public GHCR tag against the same manifest and
bottle digest without App credentials, and only then mints a fresh token for
the fixed exact-head merge API call. An exact, App-authored one-commit bottled
branch is accepted on a rerun only when its formula bytes equal the newly
verified publication bundle.

If artifacts expire, branch state is not one of the two exact states, GHCR
cannot be made public, or the original run cannot be resumed, finalization
fails closed; a new dispatch may not adopt or finalize the old bottles.

`repository-settings.json` is a public code-policy contract containing only
generic variable/secret names and expected controls. Operator identities,
provider object IDs, and provisioning evidence remain outside tracked source.
