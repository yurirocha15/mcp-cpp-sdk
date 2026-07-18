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

Run all structural tests from the source repository root:

```sh
python3 -m unittest discover -s bootstrap/tests -p 'test_*.py' -v
```

The tests deliberately fail if action references float, write jobs checkout
source, workflow-dispatch inputs drift, protected checks/environments change,
or the broker PAT client stops using its fixed standard-library route set.
