#!/usr/bin/env python3
"""Generate a reproducible, counterbalanced benchmark schedule."""

from __future__ import annotations

import argparse
import json
import random


def counterbalanced_orders(
    servers: list[str], runs: int, seed: int
) -> list[list[str]]:
    base = list(servers)
    random.Random(seed).shuffle(base)
    if runs == 3 and len(base) == 5:
        # Each server occupies three distinct positions, and every pair appears
        # in both lead/follow orders over the three-round production profile.
        indices = (
            (0, 1, 2, 3, 4),
            (1, 2, 3, 4, 0),
            (4, 3, 0, 2, 1),
        )
        return [[base[index] for index in order] for order in indices]

    return [
        base[offset % len(base) :] + base[: offset % len(base)]
        for offset in range(runs)
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--runs", type=int, required=True)
    parser.add_argument("--run", type=int)
    parser.add_argument("servers", nargs="+")
    args = parser.parse_args()

    if args.runs < 1:
        parser.error("--runs must be at least 1")
    if len(set(args.servers)) != len(args.servers):
        parser.error("server names must be unique")

    orders = counterbalanced_orders(args.servers, args.runs, args.seed)
    if args.run is None:
        print(json.dumps({"seed": args.seed, "orders": orders}, indent=2))
        return 0
    if args.run < 1 or args.run > args.runs:
        parser.error("--run must be between 1 and --runs")
    for server in orders[args.run - 1]:
        print(server)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
