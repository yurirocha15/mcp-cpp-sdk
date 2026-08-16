# Dependency Policy

This policy covers dependencies required to build, link, test, package, and
release `mcp-cpp-sdk`.

## Supported runtime dependencies

The installed SDK has three direct dependencies:

| Dependency | Supported floor | Purpose |
| --- | --- | --- |
| Boost | 1.74 | Asio executors, coroutines, and networking |
| nlohmann/json | 3.10.5 | JSON and JSON-RPC serialization |
| OpenSSL | 3.0 | Cryptographic randomness and OAuth helper primitives |

The minimum versions in `CMakeLists.txt` are the source of truth. CI must test
those floors on at least one supported platform before a stable release. The
latest stable dependency versions are tested periodically to detect upcoming
compatibility problems.

The SDK's built-in HTTP transports currently use plaintext HTTP. OpenSSL is
not used to provide TLS transport; deployments that leave a loopback or trusted
network boundary must use a TLS-terminating proxy or a custom TLS transport.

Build, test, documentation, and release-only dependencies do not become part
of the public link interface. Their exact versions should be locked where the
tool supports a lock file or pinned by immutable revision in CI.

## Updates and support windows

- Patch and minor dependency updates may be adopted in any SDK release when
  they preserve the documented compiler, platform, source, and ABI contracts.
- Raising a direct dependency floor is announced in the changelog. Before
  `1.0.0` it requires at least a minor SDK release; after `1.0.0` it requires a
  major release unless the old dependency is unsupported or has an unresolved
  vulnerability.
- A dependency release that is end-of-life or prevents protocol conformance may
  be removed from support after notice in the changelog and roadmap.
- Unsupported or unmaintained transitive dependencies are replaced when a
  maintained alternative exists and the migration risk is reasonable.

## Adding dependencies

A new direct dependency must have a compatible license, active maintenance,
documented security reporting, supported CMake consumption, and a demonstrated
benefit that is not reasonably achievable with the C++ standard library or an
existing dependency. Optional features should keep their dependencies private
and optional whenever possible.

## Vulnerabilities

Report vulnerabilities through the private process in `SECURITY.md`. A known
dependency vulnerability is classified using the maintenance priorities in
`MAINTENANCE.md`; CVSS 7.0 or higher is P0. Remediation may include upgrading,
backporting, disabling the affected feature, or documenting that the SDK is not
reachable. Security updates can override the normal compatibility window, and
the release notes must explain any resulting consumer action.

## Review cadence

Maintainers review direct dependencies at least monthly, before each release,
and whenever a relevant security advisory is published. Dependency changes are
covered by the normal build, sanitizer, package-consumer, and conformance gates.
