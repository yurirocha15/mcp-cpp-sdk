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
`summary.md` keeps the scored server and client rows apart from the reported
`server-pending` and `client-backcompat` rows, so a pass rate names its scope.

The baseline must be regenerated only after reviewing the complete runner
output. Updating it to hide an uninvestigated regression is not acceptable.

`run.sh` also runs the server `pending` suite, the scenarios the runner does not
count towards a tier yet, against `expected-failures-pending.yml`, and writes its
results to `server-pending/results`. The runner fails a scenario on any warning,
so `check_pending_sse.py` checks `server-sse-polling` check by check: the priming
event must pass, and the retry field, which the server writes only when the
application sets `LegacyStreamOptions::sse_retry`, must be the only check left
with a warning.

The client `backcompat` suite holds the two 2025-03-26 authorization scenarios,
in which the server publishes no protected resource metadata. `--spec-version`
would select none of them, so `run.sh` runs the suite without it, against
`expected-failures.yml`, writes its results to `client-backcompat/results`, and
fails when fewer than two scenario results exist.

## Skills scenarios

The alpha runner's skills scenarios need a server that serves skills. The
everything server serves three of them (`git-workflow`, `acme/billing/refunds`
and `pdf-processing`) only when the environment variable
`MCP_CONFORMANCE_SKILLS` is set to a non-empty value; without it the server
declares no Skills extension, so the pinned suites run against the server they
always have. `conformance/run_alpha.sh` starts the server with the variable set.

```bash
MCP_CONFORMANCE_SKILLS=1 build/release/mcp-conformance-everything-server
```

The requirement set of `2026-07-28` names no Skills scenario, so
`conformance/run_alpha.sh` also runs the three `sep-2640-skills-*` server
scenarios and the four `sep-2640-client-*` client scenarios one by one with
`--force`. The conformance client lists the skills of the server, reads the
first skill's `SKILL.md` (the SDK checks its size and digest), compares its
frontmatter with the listed entry, and reads the supporting files only when
every check holds. Their results are in `server/skills-results` and
`client/skills-results` of the output directory, and the `Passed:` lines of
their logs are in `summary.txt`.

## Alpha runner

`conformance/run_alpha.sh` runs the alpha channel of the official runner
(`@modelcontextprotocol/conformance@0.2.0-alpha.12`) against the same fixtures,
once per requirement set: `--requirements 2026-07-28` and
`--requirements 2025-11-25`. The runner is installed from
`conformance/runner-alpha/package-lock.json`, and each set gets a fresh fixture
server process and its own directory under the output directory:

```bash
python scripts/build.py --conformance
npm ci --ignore-scripts --prefix conformance/runner-alpha
bash conformance/run_alpha.sh build/release build/conformance-results-alpha
```

`expected-failures-alpha.yml` is passed to the runner with `--expected-failures`.
It lists the scenarios that fail on purpose (see below), and the runner fails on
a new failure and on a stale entry. The exit status of the script is the runner's:
`0` when both sets pass against the baseline, otherwise the highest runner exit
status, and `2` when the harness cannot start. `summary.txt` collects the totals
the runner prints.

The `conformance-alpha.yml` workflow runs the script in an advisory job: the job
sets `continue-on-error: true` and uploads the results as the
`mcp-conformance-alpha` artifact. No other workflow calls it, so the CI result
never waits for it. `scripts/check_alpha_runner_guard.py` checks the lockfile, the
exact pin, the `npm ci` install and both requirement sets, and keeps the alpha
runner out of every other job and script. Alpha results are never tier evidence;
`run.sh` and its pinned runner stay the gate.

The requirement set of `2026-07-28` names no Skills scenario, so `run_alpha.sh`
also runs the three `sep-2640-skills-*` server scenarios and the four
`sep-2640-client-*` client scenarios one by one with `--force`, against a server
started with `MCP_CONFORMANCE_SKILLS=1`. The conformance client lists the skills
of the server, reads the first skill's `SKILL.md` (the SDK checks its size and
digest), compares its frontmatter with the listed entry, and reads the
supporting files only when every check holds. Their results are in
`2026-07-28/server/skills-results` and `2026-07-28/client/skills-results`, and
they do not count towards the exit status. In requirement-set mode the runner
reports `server-sse-polling` without running its checks, so the `2025-11-25`
pass runs that scenario alone with `--force`; its results are in
`2025-11-25/server/sse-polling-results`.

The fixtures serve the tools the scenarios call (`test_input_required_result_*`,
`test_missing_capability`, `test_streaming_elicitation`, `test_logging_tool`,
`test_trigger_tool_change`, `test_trigger_prompt_change`, `test_custom_headers`
and `json_schema_2020_12_tool`), the tools of the tasks scenarios (`greet`,
`slow_compute`, `failing_job`, `protocol_error_job`, `confirm_delete`,
`multi_input` and `test_tool_with_task`, served through
`Server::enable_tasks()`; `test_tool_with_task` asks for a name through
`Server::set_tool_task_preparation()` before its task is created), and handle
the client scenarios of the requirement set. What still fails there does not
come from the fixtures:

- The alpha runner does not score the extension scenarios (client credentials,
  enterprise managed authorization, DPoP, workload identity) and reports them
  as not scored. The client does not implement DPoP (`auth/dpop`,
  `auth/dpop-nonce`) or the workload identity federation grant
  (`auth/wif-jwt-bearer`), so those scenarios fail and are listed in
  `expected-failures-alpha.yml`.
- `input-required-result-ignore-extra-params` reports a warning, which the
  runner counts as a failure, so `expected-failures-alpha.yml` lists it. The
  scenario sends `inputResponses` on a first call that carries no
  `requestState`. The SDK issues a `requestState` with every `input_required`
  result and resumes a request only from it, so the call is a new request and
  the server asks its question again, as the multi round-trip text asks of a
  server that lacks information it needs.
