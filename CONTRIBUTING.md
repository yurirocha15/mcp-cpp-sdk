# Contributing to mcp-cpp-sdk

Contributions are welcome. The contributing guide,
[docs/contributing.rst](docs/contributing.rst), also published at
<https://mcp-cpp-sdk.yurirocha.com/contributing.html>, covers the development
setup, code style, testing, the pull request process and known compiler issues.

## Before you open a pull request

- Build and run the tests with `python scripts/build.py --test`.
- Format with `make format` and lint with `make lint`.
- Add or update tests for the code you change.
- Give a change that users notice one entry under `## [Unreleased]` in
  `CHANGELOG.md`: one sentence of at most 200 characters about what changed.

## Commit messages

Each subject is a conventional commit: `feat:`, `fix:`, `docs:`, `style:`,
`refactor:`, `perf:`, `test:`, `build:`, `ci:` or `chore:`, followed by an
imperative summary under 80 characters.

## Issues

- Report a bug or request a feature in a GitHub issue. For a bug, give the
  SDK version, compiler, Boost version and platform, and a minimal
  reproduction. `MAINTENANCE.md` describes how issues are triaged and
  prioritized.
- Report a security vulnerability privately, as `SECURITY.md` describes, not
  in a public issue.
