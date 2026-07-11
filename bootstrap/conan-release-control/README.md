# mcp-cpp-sdk ConanCenter release control

This public repository is the hardened approval broker for opening one
ConanCenter recipe pull request. It is not a general GitHub API proxy.

The dispatch accepts exactly nine bounded strings. Public verification runs
before the protected environment. The PAT job starts on a fresh runner, does
not checkout or execute repository/source/recipe content, injects the PAT into
one final inline standard-library client step, and has no later step.

The final client only performs fixed GitHub API GETs and, when no identical PR
exists, one fixed POST to `conan-io/conan-center-index`. It rejects redirects,
pagination, oversized bodies, ambiguous PRs, identity mismatches, recovery
branch misuse, and every caller-supplied URL.

`repository-settings.json` names the required public controls, checks,
variables, and secret without storing operator identities, numeric IDs,
fingerprints, or provisioning evidence. Operational setup remains outside this
tracked repository.
