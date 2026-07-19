# External release repository bootstrap templates

These directories are reviewed source templates for the three public repositories
that intentionally live outside `yurirocha15/mcp-cpp-sdk`:

- `conan-release-control/` becomes `yurirocha15/mcp-cpp-sdk-release-control`.
- `homebrew-tap/` becomes `yurirocha15/homebrew-mcp-cpp-sdk`.
- `chocolatey-publisher/` becomes
  `yurirocha15/mcp-cpp-sdk-chocolatey-publisher`.

They contain no credentials and make no provider calls during local tests.
Each `repository-settings.json` is a public, value-free policy contract; manual
provisioning values and evidence are intentionally maintained outside tracked
source. Never copy `bootstrap/tests/` into an external repository.

Each publisher snapshot is activated by an asset-free immutable GitHub Release
on its lightweight `release-control-v1` tag. The source preflight resolves that
tag, proves release immutability, verifies every reviewed control byte, and
dispatches the exact tag. A publisher's credential-free first job independently
requires the same tag, commit SHA, and tagged workflow identity.

Activation order is fixed: merge the reviewed snapshot to protected `main`,
record that full commit SHA, enable repository release immutability, create an
asset-free non-prerelease Release whose lightweight `release-control-v1` tag
targets that exact SHA, verify the public Release reports `immutable: true` and
the tag ref still resolves directly to the SHA, then restrict publisher
environments to that exact tag. Never move or reuse the tag for new controls;
publish a newly reviewed `release-control-vN` snapshot instead.

Run all structural tests from the source repository root:

```sh
python3 -m unittest discover -s bootstrap/tests -p 'test_*.py' -v
```

The tests deliberately fail if action references float, write jobs checkout
source, workflow-dispatch inputs drift, protected checks/environments change,
or the broker PAT client stops using its fixed standard-library route set.
