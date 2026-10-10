# Maintenance Policy

This project uses public issue labels and milestones to make maintenance status
observable. Security reports follow the private process in `SECURITY.md`.

This policy is effective prospectively for issues opened on or after
2026-07-19. It does not establish historical response-time evidence.

## Service levels

- New public issues are triaged within 30 calendar days.
- P0 issues are resolved, mitigated, or have a safe release available within
  14 calendar days of the initial report.
- The Tier 1 target is triage within two business days and P0 resolution within
  seven calendar days; those shorter windows become release policy only after
  the project has demonstrated that capacity.

Triage means reproducing or requesting the information needed to reproduce,
classifying the issue, and identifying the next state. Actionable issues also
receive one priority label. An acknowledgement without classification is not
complete triage.

## Priority

- **P0:** CVSS 7.0 or higher, failure of core MCP operations for supported
  users, data loss, authentication bypass, or a release-blocking regression.
- **P1:** severe degradation without a reasonable workaround or a major
  conformance regression.
- **P2:** normal correctness, interoperability, performance, or usability work.
- **P3:** low-impact cleanup, polish, or long-term improvement.

## Workflow labels

Every triaged issue uses one type label: `bug`, `enhancement`, or `question`.
It also receives one of `needs confirmation`, `needs repro`, or `ready for
work`. Actionable issues receive exactly one of `P0` through `P3`. Issues
suitable for outside contributors may additionally use `good first issue` or
`help wanted`. The canonical label definitions are stored in
`.github/labels.yml`; maintainers must apply that manifest to the live
repository before using it as tier evidence.

## Supported releases

Before `1.0.0`, only the latest minor release receives fixes. Starting with
`1.0.0`, the latest major release is supported; older major lines receive only
explicitly announced security backports. Supported compilers, platforms, and
dependency floors are those exercised by CI and documented for the release.
