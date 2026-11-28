# Roadmap to MCP SDK Tier 1

This repository is an independent community SDK. Completing the work below
makes the project tier-ready; official Tier assignment and inclusion in the MCP
SDK roster require approval from MCP governance.

The [specification support](https://mcp-cpp-sdk.yurirocha.com/spec-support.html)
page lists what the SDK implements today. The work that remains is measured
against the [SDK tier requirements](https://modelcontextprotocol.io/community/sdk-tiers)
and the non-experimental features of the tier assessment.

## Where the SDK stands

- Protocol revision 2025-11-25, pinned conformance runner 0.1.16: all 30 server
  and 18 client scenarios that CI runs pass, with no expected failures.
- Protocol revision 2026-07-28, conformance runner 0.2.0-alpha.12: every
  scenario that the revision's requirement set scores passes (37 server and 32
  client scenarios). This run reports results but does not block merges.
- CI compiles the documentation examples and builds an installed-package
  consumer.
- Releases are project-stable `0.x` versions. The tier rules ask for `1.0.0` or
  later.

## Next release: 0.5.0

Each item is Planned, In progress or Done.

### Specification features

| Work item | State |
|---|---|
| Send `notifications/elicitation/complete` to the client that started a URL-mode elicitation, and give clients a handler for it | Planned |
| Return the URL elicitation required error (`-32042`) from a server handler, and read its elicitations on the client | Planned |
| Typed client calls for resource subscribe, resource unsubscribe and logging level | Planned |
| The legacy HTTP+SSE transport for clients and servers, marked deprecated from the start (SEP-2596) | Planned |
| A server call that sends any notification to legacy sessions, and an observer for `subscriptions/listen` streams | Planned |
| Compiled examples for the features that lack one: image, audio and embedded-resource tool results, binary resources, embedded resources and images in prompts, URL-mode elicitation and its completion, roots change notifications, and resource-template completion | Planned |
| A feature matrix that links each tier-assessment feature to its API reference, guide and example, checked in CI | Planned |

### Conformance

| Work item | State |
|---|---|
| Fall back to the 2025-03-26 authorization discovery when a server publishes no protected resource metadata, so the runner's 2025-03-26 back-compat scenarios pass | Planned |
| Send the SSE priming event on POST streams of 2025-11-25 sessions | Planned |
| Run the pending and back-compat scenarios in CI | Planned |
| Make the conformance and repository guard checks block merges | Planned |
| Install the 2026-07-28 runner from a lockfile and run the 2025-11-25 and 2026-07-28 requirement sets | Planned |
| Make the 2026-07-28 run block merges once it has stayed green on main | Planned |

### Deprecated features

Revision 2026-07-28 deprecates Roots, Sampling, Logging and dynamic client
registration; the `includeContext` values `thisServer` and `allServers` follow
Sampling. The earliest removal is the first revision released on or after
2027-07-28 ([deprecated features](https://modelcontextprotocol.io/specification/draft/deprecated)).
The [feature lifecycle](https://modelcontextprotocol.io/community/feature-lifecycle)
asks Tier 1 SDKs to mark such APIs with the language's own mechanism and to
warn when they are used.

| Work item | State |
|---|---|
| Mark the APIs of these features `[[deprecated]]` with the SEP and the earliest removal, with a macro that turns the attribute off | Planned |
| Report the first use of a deprecated feature through a replaceable handler, printing to standard error by default | Planned |
| A CI build that fails on deprecation warnings in the SDK's own code | Planned |

### Authorization extensions

No tier requires protocol extensions.

| Work item | State |
|---|---|
| DPoP (SEP-1932) | Not planned |

Workload identity federation waits until its extension specification is
merged.

### Project process

| Work item | State |
|---|---|
| Create the status and priority labels of the tier rules: `needs confirmation`, `needs repro`, `ready for work`, `P0` to `P3` | Planned |
| A `CONTRIBUTING.md` in the repository root that points to the contributing guide | Planned |
| Publish an SDK release within 30 days of each specification release | Planned |
| Adopt the Tier 1 windows of triage within two business days and P0 resolution within seven days, once `MAINTENANCE.md` shows the capacity | Planned |

## Stable 1.0 release

- Freeze the supported C++ API, compatibility policy, compiler matrix, and
  dependency floors.
- Complete the public API and dependency review.
- Publish a non-prerelease `1.0.0` with migration notes and immutable
  conformance evidence.
- Rebaseline against the final 2026 protocol and the conformance release chosen
  by the MCP SDK Working Group before making a tier application.

## Tier 1 readiness

- Maintain 100% of applicable client and server conformance for the accepted
  current protocol revision.
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
