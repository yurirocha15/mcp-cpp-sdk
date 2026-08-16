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
