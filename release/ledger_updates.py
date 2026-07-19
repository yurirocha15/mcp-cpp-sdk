"""Build a canonical release-ledger update from GitHub job conclusions."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Sequence

from .ledger import ALLOWED_RESULTS, DESTINATIONS, selected_channels


_CHANNEL_TO_DESTINATION = {channel: destination for destination, (channel, _) in DESTINATIONS.items()}
_CONCLUSIONS = frozenset({"success", "failure", "cancelled", "skipped"})


def build_updates(
    *,
    channels_csv: str,
    manifest_sha256: str | None,
    observations: dict[str, tuple[str, str | None]],
) -> dict[str, object]:
    channels = selected_channels(channels_csv)
    selected_destinations = {"github"} | {_CHANNEL_TO_DESTINATION[channel] for channel in channels}
    if set(observations) != set(DESTINATIONS):
        raise ValueError("job observations must contain every destination")
    normalized: dict[str, dict[str, str | None]] = {}
    for destination in DESTINATIONS:
        conclusion, result = observations[destination]
        if conclusion not in _CONCLUSIONS:
            raise ValueError(f"unknown job conclusion for {destination}: {conclusion}")
        if destination not in selected_destinations:
            normalized[destination] = {"conclusion": "skipped", "result": None}
            continue
        if conclusion == "success" and result in ALLOWED_RESULTS - {"FAILED", "NOT_SELECTED"}:
            normalized[destination] = {"conclusion": conclusion, "result": result}
        else:
            normalized[destination] = {
                "conclusion": "cancelled" if conclusion == "cancelled" else "failure",
                "result": None,
            }
    return {
        "schema_version": 1,
        "release_manifest_sha256": manifest_sha256,
        "selected_channels": sorted(channels),
        "observations": normalized,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channels", required=True)
    parser.add_argument("--manifest-sha256", default="")
    parser.add_argument("--output", type=Path, required=True)
    for destination in DESTINATIONS:
        option = destination.replace("_", "-")
        parser.add_argument(f"--{option}-conclusion", required=True)
        parser.add_argument(f"--{option}-result", default="")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    observations = {
        destination: (
            getattr(args, f"{destination}_conclusion"),
            getattr(args, f"{destination}_result") or None,
        )
        for destination in DESTINATIONS
    }
    try:
        value = build_updates(
            channels_csv=args.channels,
            manifest_sha256=args.manifest_sha256 or None,
            observations=observations,
        )
    except ValueError as error:
        raise SystemExit(f"ledger-updates: {error}") from error
    args.output.write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
