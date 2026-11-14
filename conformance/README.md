# Conformance fixtures

These fixtures integrate the official MCP conformance runner without claiming
an official SDK tier. The runner is locked to version `0.1.16`, the tested
protocol revision is `2025-11-25`, and the full Tier-required server `active`
and client `core` suites run on every conformance CI job.

```bash
python scripts/build.py --conformance
npm ci --prefix conformance/runner
bash conformance/run.sh build/release build/conformance-results
```

`expected-failures.yml` is a regression baseline, not an exclusion list. Every
scenario still runs. A new failure, or an expected failure that starts passing
without its baseline entry being removed, fails the command. Result directories
contain the individual scenario checks, fixture logs, runner logs, source
revision, and an aggregate scenario-level summary. A green job means the
baseline did not drift; it does not mean that an SDK tier has been achieved.

The baseline must be regenerated only after reviewing the complete runner
output. Updating it to hide an uninvestigated regression is not acceptable.

## Skills scenarios

The alpha runner's skills scenarios need a server that serves skills. The
everything server serves three of them (`git-workflow`, `acme/billing/refunds`
and `pdf-processing`) only when the environment variable
`MCP_CONFORMANCE_SKILLS` is set to a non-empty value; without it the server
declares no Skills extension, so the pinned suites run against the server they
always have.

```bash
MCP_CONFORMANCE_SKILLS=1 build/release/mcp-conformance-everything-server
```

## 2026-07-28 alpha run

`conformance/run_alpha.sh` runs the alpha channel of the official runner
(`@modelcontextprotocol/conformance@0.2.0-alpha.12`, fetched with `npx`) with
`--requirements 2026-07-28` against the same fixtures, and writes
`build/conformance-results-alpha`:

```bash
python scripts/build.py --conformance
bash conformance/run_alpha.sh build/release build/conformance-results-alpha
```

It never gates anything. The script exits `0` once the runner has run, and the
`conformance-2026-07-28-alpha` job of `.github/workflows/conformance.yml` sets
`continue-on-error: true` and uploads the results as the
`mcp-conformance-2026-07-28-alpha` artifact. The alpha runner has no baseline
file, so every failing scenario shows in the report; the 2025-11-25 gate above
is the only evidence this directory claims.
`scripts/check_alpha_runner_guard.py` keeps the alpha runner out of every other
job and script.

The fixtures serve the tools the scenarios call (`test_input_required_result_*`,
`test_missing_capability`, `test_streaming_elicitation`, `test_logging_tool`,
`test_trigger_tool_change`, `test_trigger_prompt_change`, `test_custom_headers`
and `json_schema_2020_12_tool`) and handle the client scenarios of the
requirement set. What still fails there does not come from the fixtures:

- The tasks extension scenarios fail because the conformance server does not
  enable the extension.
- `input-required-result-basic-elicitation` fails because it expects the
  question under the key `user_name`, while the server's fixture lets the SDK pick the keys
  of its questions.
- `auth/authorization-server-migration` fails because the client sends no
  registration request to the new authorization server.
- The alpha runner does not score the extension scenarios (client credentials,
  enterprise managed authorization, DPoP, workload identity) and reports them
  as not scored.
- The conformance client pins the protocol version the runner names, so
  `request-metadata` reports a warning for its simulated version rejection:
  a pinned client does not renegotiate, and `ClientProtocol::Auto()` does.
