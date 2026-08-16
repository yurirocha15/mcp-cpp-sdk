# Roadmap to MCP SDK Tier 1

This repository is an independent community SDK. Completing these gates makes
the project tier-ready; official Tier assignment and inclusion in the MCP SDK
roster require approval from MCP governance.

## Tier 2 foundation

- Pin the official conformance runner and publish separate client and server
  results for protocol revision `2025-11-25`.
- Reach at least 80% applicable conformance on both sides, with 100% as this
  project's internal target and all expected failures kept visible.
- Close protocol lifecycle, capability negotiation, structured result,
  timeout/cancellation, disconnect cleanup, and HTTP security gaps.
- Compile documentation examples and validate installed-package consumers in
  CI.
- Operate the 30-day issue-triage and 14-day P0 maintenance commitments.
- Implement newly released non-experimental protocol features within six
  months, with their conformance, documentation, and example coverage.
- Publish a non-prerelease `1.0.0` or later. Project-stable `0.x` releases do
  not satisfy the official Tier 2 stable-release requirement.
- Complete the public API and dependency review required for `1.0.0`.

## Stable 1.0 release

- Freeze the supported C++ API, compatibility policy, compiler matrix, and
  dependency floors.
- Publish a non-prerelease `1.0.0` with migration notes and immutable
  conformance evidence.
- Rebaseline against the final 2026 protocol and the conformance release chosen
  by the MCP SDK Working Group before making a tier application.

## Tier 1 readiness

- Maintain 100% of applicable client and server conformance for the accepted
  current protocol revision.
- Provide runnable documentation and examples for all 48 non-experimental MCP
  features in the Tier assessment, with a checked coverage matrix linking each
  feature to its API, guide, and example.
- Demonstrate issue triage within two business days and P0 resolution within
  seven calendar days.
- Track MCP release candidates early enough to support required features on the
  release schedule agreed with the SDK Working Group.
- Maintain explicit source, ABI, deprecation, dependency, and breaking-change
  policies for every stable release.

## Governance checkpoint

Before claiming an official tier, maintainers will ask the MCP SDK Working
Group to clarify roster admission, repository governance, the pinned
conformance release, and evidence submission. Until accepted, project material
will report measured conformance percentages or use "tier-ready" language,
never an official Tier 1 or Tier 2 designation.
