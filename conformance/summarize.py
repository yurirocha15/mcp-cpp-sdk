#!/usr/bin/env python3
"""Create scenario-level conformance evidence from runner check files."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


TIMESTAMP_SUFFIX = re.compile(r"-\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}-\d{3}Z$")

EXPECTED_SCENARIOS = {
    "server": {
        "completion-complete",
        "dns-rebinding-protection",
        "elicitation-sep1034-defaults",
        "elicitation-sep1330-enums",
        "logging-set-level",
        "ping",
        "prompts-get-embedded-resource",
        "prompts-get-simple",
        "prompts-get-with-args",
        "prompts-get-with-image",
        "prompts-list",
        "resources-list",
        "resources-read-binary",
        "resources-read-text",
        "resources-subscribe",
        "resources-templates-read",
        "resources-unsubscribe",
        "server-initialize",
        "server-sse-multiple-streams",
        "tools-call-audio",
        "tools-call-elicitation",
        "tools-call-embedded-resource",
        "tools-call-error",
        "tools-call-image",
        "tools-call-mixed-content",
        "tools-call-sampling",
        "tools-call-simple-text",
        "tools-call-with-logging",
        "tools-call-with-progress",
        "tools-list",
    },
    "client": {
        "auth/basic-cimd",
        "auth/metadata-default",
        "auth/metadata-var1",
        "auth/metadata-var2",
        "auth/metadata-var3",
        "auth/pre-registration",
        "auth/scope-from-scopes-supported",
        "auth/scope-from-www-authenticate",
        "auth/scope-omitted-when-undefined",
        "auth/scope-retry-limit",
        "auth/scope-step-up",
        "auth/token-endpoint-auth-basic",
        "auth/token-endpoint-auth-none",
        "auth/token-endpoint-auth-post",
        "elicitation-sep1034-client-defaults",
        "initialize",
        "sse-retry",
        "tools_call",
    },
}


def scenario_name(suite: str, root: Path, checks_file: Path) -> str:
    name = checks_file.parent.relative_to(root).as_posix()
    name = TIMESTAMP_SUFFIX.sub("", name)
    if suite == "server" and name.startswith("server-"):
        name = name.removeprefix("server-")
    return name


def summarize_suite(suite: str, root: Path) -> dict[str, object]:
    emitted: dict[str, list[Path]] = {}
    for checks_file in sorted(root.rglob("checks.json")):
        name = scenario_name(suite, root, checks_file)
        emitted.setdefault(name, []).append(checks_file)

    scenarios: list[dict[str, object]] = []
    scenario_names = EXPECTED_SCENARIOS[suite] | emitted.keys()
    for name in sorted(scenario_names):
        checks_files = emitted.get(name, [])
        statuses: list[str] = []
        for checks_file in checks_files:
            checks = json.loads(checks_file.read_text(encoding="utf-8"))
            statuses.extend(check.get("status", "UNKNOWN") for check in checks)
        if not checks_files:
            statuses = ["MISSING"]
        elif len(checks_files) > 1:
            statuses.append("DUPLICATE")

        passed = (
            name in EXPECTED_SCENARIOS[suite]
            and len(checks_files) == 1
            and "FAILURE" not in statuses
            and "SUCCESS" in statuses
        )
        scenarios.append(
            {
                "name": name,
                "passed": passed,
                "statuses": statuses,
                "checks_files": [str(path.relative_to(root)) for path in checks_files],
            }
        )

    passed_count = sum(bool(scenario["passed"]) for scenario in scenarios)
    total = len(scenarios)
    return {
        "passed": passed_count,
        "total": total,
        "pass_rate_percent": round(100.0 * passed_count / total, 1) if total else 0.0,
        "failed_scenarios": [
            scenario["name"] for scenario in scenarios if not scenario["passed"]
        ],
        "scenarios": scenarios,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-results", type=Path, required=True)
    parser.add_argument("--client-results", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--server-status", type=int, required=True)
    parser.add_argument("--client-status", type=int, required=True)
    args = parser.parse_args()

    suites = {
        "server": summarize_suite("server", args.server_results),
        "client": summarize_suite("client", args.client_results),
    }
    summary = {
        "runner": "@modelcontextprotocol/conformance@0.1.16",
        "protocol_version": "2025-11-25",
        "runner_exit_status": {
            "server": args.server_status,
            "client": args.client_status,
        },
        "suites": suites,
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    markdown = [
        "# MCP conformance regression baseline",
        "",
        "A green runner status means the checked-in expected-failure baseline did not drift;",
        "it does not mean that an MCP SDK tier has been achieved. The pass rate follows the",
        "tier-check convention: warnings do not fail a scenario, while missing evidence does.",
        "",
        "| Suite | Passed scenarios | Total scenarios | Pass rate | Runner status |",
        "|---|---:|---:|---:|---:|",
    ]
    for suite in ("server", "client"):
        result = suites[suite]
        markdown.append(
            f"| {suite} | {result['passed']} | {result['total']} | "
            f"{result['pass_rate_percent']}% | {summary['runner_exit_status'][suite]} |"
        )
    markdown.append("")
    (args.output_dir / "summary.md").write_text("\n".join(markdown), encoding="utf-8")


if __name__ == "__main__":
    main()
