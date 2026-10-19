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

## Vendored code

`third_party/jsoncons` holds a subset of [jsoncons](https://github.com/danielaparker/jsoncons)
(Boost Software License 1.0), whose JSON Schema extension the SDK uses to
validate tool schemas and the values checked against them. It is compiled into
the SDK library and is not a dependency of anything that links the SDK: its
headers are not installed, its symbols are not exported, and its namespace is
renamed so that it cannot collide with a jsoncons an application uses itself.
Distribution packages declare it as bundled.

The tree is generated, never edited by hand. `scripts/vendor_jsoncons.py`
downloads the release pinned in the script, verifies its SHA-256, copies the
headers the extension needs and applies the recorded transformation described
in the script and in `third_party/jsoncons/PROVENANCE`: `std::regex` is replaced
with the SDK's linear-time matcher, the evaluation context gets limits on
evaluation depth and work, the numeric keywords handle large doubles without
undefined behavior, and the namespace and macro prefix are renamed. CI runs `python3 scripts/vendor_jsoncons.py --check`, which fails when
the committed tree differs from the pinned release plus that transformation.

jsoncons was chosen because it implements JSON Schema drafts 4, 6, 7, 2019-09
and 2020-12, including `$dynamicRef` and the `unevaluated*` keywords, which no
existing dependency provides. Upstream reports vulnerabilities through GitHub
Security Advisories. The vendored copy is part of the monthly and pre-release
dependency review: upstream releases and advisories are checked, and an update
is made by changing the pinned version and digest and rerunning the script.

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
