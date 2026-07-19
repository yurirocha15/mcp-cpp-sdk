#!/usr/bin/env python3
"""Validate and normalize untrusted release workflow-dispatch inputs."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path


EXTERNAL_CHANNELS = (
    "conan2",
    "apt",
    "rpm",
    "aur",
    "homebrew",
    "chocolatey",
)
OPERATIONS = {
    "validate-selected": ("validate", False, False),
    "validate-all": ("validate", True, False),
    "publish-selected": ("publish", False, False),
    "publish-retry-selected": ("publish", False, True),
    "publish-all": ("publish", True, False),
}
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
    operation: str
    mode: str
    version: str
    release_kind: str
    channels: tuple[str, ...]
    selection_label: str
    ledger_issue: str
    retry_authorized: bool

    def workflow_outputs(self) -> dict[str, str]:
        outputs = {
            "operation": self.operation,
            "mode": self.mode,
            "version": self.version,
            "release_kind": self.release_kind,
            "normalized_channels": ",".join(self.channels),
            "selection_label": self.selection_label,
            "ledger_issue": self.ledger_issue,
            "retry_authorized": str(self.retry_authorized).lower(),
        }
        outputs.update(
            (f"target_{channel}", "true" if channel in self.channels else "false")
            for channel in EXTERNAL_CHANNELS
        )
        return outputs


def parse_release_identity(tag: str) -> tuple[str, str]:
    has_invalid_character = any(
        ord(character) < 33 or ord(character) > 126 for character in tag
    )
    if len(tag) > 64 or has_invalid_character:
        raise ContractError("tag contains invalid characters")
    stable_match = STABLE_TAG.fullmatch(tag)
    if stable_match:
        if int(stable_match.group(1)) >= 1:
            raise ContractError(
                "1.x releases require a reviewed multi-platform ABI policy before dispatch"
            )
        return tag[1:], "stable"
    rc_match = RC_TAG.fullmatch(tag)
    if rc_match:
        if int(rc_match.group(1)) >= 1:
            raise ContractError(
                "1.x release candidates require a reviewed multi-platform ABI policy before dispatch"
            )
        return tag[1:], "rc"
    raise ContractError("tag is not canonical stable or RC SemVer")


def parse_boolean(name: str, value: str) -> bool:
    if value not in {"true", "false"}:
        raise ContractError(f"{name} must be exactly true or false")
    return value == "true"


def parse_channel_selection(
    *, publish_all: bool, channel_inputs: dict[str, str]
) -> tuple[tuple[str, ...], str]:
    if set(channel_inputs) != set(EXTERNAL_CHANNELS):
        raise ContractError("channel checkbox inventory is incomplete")
    selected = tuple(
        channel
        for channel in EXTERNAL_CHANNELS
        if parse_boolean(channel, channel_inputs[channel])
    )
    if publish_all and selected:
        raise ContractError("an all-channel operation cannot be combined with individual channels")
    if publish_all:
        return EXTERNAL_CHANNELS, "all"
    return selected, ",".join(("github", *selected))


def validate_dispatch(
    *,
    tag: str,
    operation: str,
    conan2: str,
    apt: str,
    rpm: str,
    aur: str,
    homebrew: str,
    chocolatey: str,
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
    try:
        mode, publish_all, retry_authorized = OPERATIONS[operation]
    except KeyError as error:
        raise ContractError("operation is not an allowed release action") from error

    version, release_kind = parse_release_identity(tag)
    channels, selection_label = parse_channel_selection(
        publish_all=publish_all,
        channel_inputs={
            "conan2": conan2,
            "apt": apt,
            "rpm": rpm,
            "aur": aur,
            "homebrew": homebrew,
            "chocolatey": chocolatey,
        },
    )
    if release_kind == "rc" and channels:
        raise ContractError("RC releases cannot select third-party channels")
    if not re.fullmatch(r"[1-9][0-9]*", ledger_issue):
        raise ContractError("ledger_issue must be a positive canonical decimal")
    expected_confirmation = f"{operation}:{tag}:{selection_label}:{ledger_issue}"
    if confirmation != expected_confirmation:
        raise ContractError("confirmation must exactly bind operation:tag:channels:ledger_issue")
    return DispatchContract(
        operation,
        mode,
        version,
        release_kind,
        channels,
        selection_label,
        ledger_issue,
        retry_authorized,
    )


def main() -> int:
    contract = validate_dispatch(
        tag=os.environ["DISPATCH_TAG"],
        operation=os.environ["DISPATCH_OPERATION"],
        conan2=os.environ["DISPATCH_CONAN2"],
        apt=os.environ["DISPATCH_APT"],
        rpm=os.environ["DISPATCH_RPM"],
        aur=os.environ["DISPATCH_AUR"],
        homebrew=os.environ["DISPATCH_HOMEBREW"],
        chocolatey=os.environ["DISPATCH_CHOCOLATEY"],
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
