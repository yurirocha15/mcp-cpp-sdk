#!/usr/bin/env python3
"""Validate and normalize untrusted release workflow-dispatch inputs."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path


ALLOWED_TARGETS = (
    "github",
    "conan2",
    "apt",
    "rpm",
    "aur",
    "homebrew",
    "chocolatey",
)
NUMERIC_IDENTIFIER = r"(?:0|[1-9][0-9]*)"
STABLE_TAG = re.compile(
    rf"^v({NUMERIC_IDENTIFIER})\.({NUMERIC_IDENTIFIER})\.({NUMERIC_IDENTIFIER})$"
)
RC_TAG = re.compile(
    rf"^v({NUMERIC_IDENTIFIER})\.({NUMERIC_IDENTIFIER})\.({NUMERIC_IDENTIFIER})"
    rf"-rc\.([1-9][0-9]*)$"
)


class ContractError(ValueError):
    """Raised when a dispatch input or trusted GitHub identity is invalid."""


@dataclass(frozen=True)
class DispatchContract:
    mode: str
    version: str
    release_kind: str
    targets: tuple[str, ...]
    ledger_issue: str
    retry: bool

    def workflow_outputs(self) -> dict[str, str]:
        outputs = {
            "mode": self.mode,
            "version": self.version,
            "release_kind": self.release_kind,
            "normalized_targets": ",".join(self.targets),
            "ledger_issue": self.ledger_issue,
            "retry": str(self.retry).lower(),
        }
        outputs.update(
            (f"target_{target}", "true" if target in self.targets else "false")
            for target in ALLOWED_TARGETS
        )
        return outputs


def parse_release_identity(tag: str) -> tuple[str, str]:
    has_invalid_character = any(
        ord(character) < 33 or ord(character) > 126 for character in tag
    )
    if len(tag) > 64 or has_invalid_character:
        raise ContractError("tag contains invalid characters")
    if STABLE_TAG.fullmatch(tag):
        return tag[1:], "stable"
    if RC_TAG.fullmatch(tag):
        return tag[1:], "rc"
    raise ContractError("tag is not canonical stable or RC SemVer")


def parse_targets(targets_input: str) -> tuple[str, ...]:
    if targets_input == "all":
        return ALLOWED_TARGETS
    targets = tuple(targets_input.split(","))
    if not targets or any(not target for target in targets):
        raise ContractError("targets must not contain empty entries")
    if len(targets) != len(set(targets)):
        raise ContractError("targets must not contain duplicates")
    if set(targets) - set(ALLOWED_TARGETS):
        raise ContractError("targets contain an unknown destination")
    canonical_targets = tuple(target for target in ALLOWED_TARGETS if target in targets)
    if targets != canonical_targets:
        raise ContractError("targets must use canonical order without whitespace")
    return targets


def validate_dispatch(
    *,
    tag: str,
    mode: str,
    targets_input: str,
    ledger_issue: str,
    confirmation: str,
    event_name: str,
    workflow_ref: str,
    repository: str,
    repository_id: str,
    owner_id: str,
    expected_repository: str,
    expected_repository_id: str,
    expected_owner_id: str,
) -> DispatchContract:
    if not expected_repository or "/" not in expected_repository:
        raise ContractError("expected repository variable is missing or invalid")
    for value, label in (
        (expected_repository_id, "expected repository ID"),
        (expected_owner_id, "expected owner ID"),
    ):
        if not re.fullmatch(r"[1-9][0-9]*", value):
            raise ContractError(f"{label} variable is missing or invalid")
    trusted_identity = (
        (event_name, "workflow_dispatch", "event"),
        (workflow_ref, "refs/heads/main", "workflow ref"),
        (repository, expected_repository, "source repository"),
        (repository_id, expected_repository_id, "source repository ID"),
        (owner_id, expected_owner_id, "source owner ID"),
    )
    for actual, expected, label in trusted_identity:
        if actual != expected:
            raise ContractError(f"unexpected {label}")
    if mode not in {"validate", "publish"}:
        raise ContractError("mode must be validate or publish")

    version, release_kind = parse_release_identity(tag)
    targets = parse_targets(targets_input)
    if release_kind == "rc" and targets != ("github",):
        raise ContractError("RC releases are GitHub-only")
    if not re.fullmatch(r"[1-9][0-9]*", ledger_issue):
        raise ContractError("ledger_issue must be a positive canonical decimal")
    expected_confirmation = f"{mode}:{tag}:{targets_input}:{ledger_issue}"
    if confirmation != expected_confirmation:
        raise ContractError("confirmation must exactly bind mode:tag:targets:ledger_issue")
    retry = mode == "publish" and release_kind == "stable" and targets_input != "all"
    return DispatchContract(mode, version, release_kind, targets, ledger_issue, retry)


def main() -> int:
    contract = validate_dispatch(
        tag=os.environ["DISPATCH_TAG"],
        mode=os.environ["DISPATCH_MODE"],
        targets_input=os.environ["DISPATCH_TARGETS"],
        ledger_issue=os.environ["DISPATCH_LEDGER_ISSUE"],
        confirmation=os.environ["DISPATCH_CONFIRMATION"],
        event_name=os.environ["EVENT_NAME"],
        workflow_ref=os.environ["WORKFLOW_REF"],
        repository=os.environ["SOURCE_REPOSITORY"],
        repository_id=os.environ["SOURCE_REPOSITORY_ID"],
        owner_id=os.environ["SOURCE_OWNER_ID"],
        expected_repository=os.environ["EXPECTED_REPOSITORY"],
        expected_repository_id=os.environ["EXPECTED_REPOSITORY_ID"],
        expected_owner_id=os.environ["EXPECTED_OWNER_ID"],
    )
    output_path = Path(os.environ["GITHUB_OUTPUT"])
    with output_path.open("a", encoding="utf-8") as output:
        for key, value in contract.workflow_outputs().items():
            output.write(f"{key}={value}\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ContractError as error:
        raise SystemExit(f"release dispatch rejected: {error}") from error
