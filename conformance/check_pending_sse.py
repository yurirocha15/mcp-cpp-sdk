#!/usr/bin/env python3
"""Check the server-sse-polling result of the pinned runner's pending suite.

The runner's baseline check fails a scenario on any warning, so it cannot tell which checks of
server-sse-polling pass. The server primes the POST streams of a 2025-11-25 session and writes
`retry` only when the application sets one, which the conformance server does not. So the priming
check must pass and the retry field must be the only check left with a warning or a failure.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

CHECKS_GLOB = "server-server-sse-polling-*/checks.json"
PRIMING_CHECK = "server-sse-priming-event"
RETRY_CHECK = "server-sse-retry-field"
# Statuses that report neither a warning nor a failure.
QUIET_STATUSES = ("SUCCESS", "INFO")


class CheckError(Exception):
    """The scenario result does not match what the server sends."""


def check(results: Path) -> None:
    files = sorted(results.glob(CHECKS_GLOB))
    if len(files) != 1:
        raise CheckError(f"expected one {CHECKS_GLOB} under {results}, found {len(files)}")
    checks = json.loads(files[0].read_text(encoding="utf-8"))
    priming = [entry.get("status") for entry in checks if entry.get("id") == PRIMING_CHECK]
    if priming != ["SUCCESS"]:
        raise CheckError(f"{PRIMING_CHECK}: expected [SUCCESS], found {priming}")
    loud = sorted(
        {entry.get("id", "") for entry in checks if entry.get("status") not in QUIET_STATUSES}
    )
    if loud != [RETRY_CHECK]:
        raise CheckError(f"checks with a warning or failure: expected [{RETRY_CHECK}], found {loud}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, help="output directory of the pending suite")
    args = parser.parse_args()
    try:
        check(args.results)
    except CheckError as error:
        print(f"server-sse-polling: {error}", file=sys.stderr)
        return 1
    print(f"server-sse-polling: {PRIMING_CHECK} passes; only {RETRY_CHECK} warns")
    return 0


if __name__ == "__main__":
    sys.exit(main())
