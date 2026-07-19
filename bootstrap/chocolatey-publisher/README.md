# Chocolatey publisher control

This public control repository accepts one fixed source-release dispatch,
independently verifies the signed immutable release and Chocolatey package,
claims an idempotency record without credentials, and exposes the community
feed API key only to a final no-checkout push job.

The protected push job never downloads, installs, or upgrades Chocolatey. It
requires the `windows-2025` image's preinstalled Chocolatey CLI to match the
reviewed version and the exact Authenticode signer documented by
[Chocolatey Software](https://docs.chocolatey.org/en-us/information/security/).
The same-run handoff includes a digest-bound PowerShell client; the job checks
that digest before both its credential-free preflight and its final push. A
runner-image, CLI-version, or signing-certificate change therefore fails closed
and requires a reviewed control-tag update. Review the public Windows runner
image manifest before promoting that update for a real release.

Publishing is dispatched only through the asset-free immutable GitHub Release
tag `release-control-v1`. A credential-free first job requires that exact tag,
its immutable commit SHA supplied by the source preflight, and the exact tagged
workflow identity. The protected environment accepts only that tag; the tag
ruleset blocks updates and deletion without bypass actors.

A `PREPARING` claim records the exact publisher run, attempt, and protected
workflow commit. A retry resumes only when GitHub's prior-attempt job evidence
proves the credential-bearing job was skipped without starting. Proven push
success is recorded as submitted; failed, cancelled, incomplete, or ambiguous
evidence requires private operator reconciliation. A completed identical claim
skips publication. Public configuration policy declares only variable names,
not operator IDs, fingerprints, credentials, or provisioning evidence.
