#!/usr/bin/env python3
"""Bot-authenticated cumulative GitHub release ledger policy and CLI.

The workflow supplies REST issue JSON and ``gh api --paginate --slurp`` comment
JSON.  This module owns parsing, trust decisions, replay policy, drift tokens,
and cumulative snapshot rendering so Actions YAML does not embed executable
ledger logic.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from release.model import ReleaseLedger, SemVer, ValidationError  # noqa: E402


DESTINATIONS = {
    "github": ("github", "GITHUB"),
    "conan2": ("conan2", "CONAN"),
    "deb_apt": ("apt", "APT"),
    "rpm": ("rpm", "RPM"),
    "arch_aur": ("aur", "AUR"),
    "homebrew": ("homebrew", "HOMEBREW"),
    "chocolatey": ("chocolatey", "CHOCOLATEY"),
}
EXTERNAL_CHANNELS = tuple(
    channel for destination, (channel, _) in DESTINATIONS.items() if destination != "github"
)
ALLOWED_RESULTS = frozenset(
    {
        "NOT_SELECTED",
        "PUBLISHED",
        "DISPATCHED_PENDING_REVIEW",
        "DISPATCHED_PENDING_MODERATION",
        "SUBMITTED_PENDING_REVIEW",
        "SUBMITTED_PENDING_MODERATION",
        "BLOCKED_MANUAL_ACTION",
        "FAILED",
        "SKIPPED_ALREADY_IDENTICAL",
        "FIRST_USE_UNPROVEN",
    }
)
DURABLE_RESULTS = frozenset(
    {
        "PUBLISHED",
        "DISPATCHED_PENDING_REVIEW",
        "DISPATCHED_PENDING_MODERATION",
        "SUBMITTED_PENDING_REVIEW",
        "SUBMITTED_PENDING_MODERATION",
        "SKIPPED_ALREADY_IDENTICAL",
    }
)
# A source-workflow retry must never redispatch work already handed to a
# downstream control workflow.  Recovery of pending dispatches belongs there.
SOURCE_RETRYABLE_RESULTS = frozenset(
    {"FAILED", "BLOCKED_MANUAL_ACTION", "FIRST_USE_UNPROVEN"}
)

SNAPSHOT_FIELDS = frozenset(
    {
        "schema_version",
        "sequence",
        "previous_snapshot_sha256",
        "readiness_sha256",
        "workflow_run_id",
        "workflow_run_attempt",
        "selected_channels",
        "ledger",
    }
)
READINESS_FIELDS = frozenset(
    {
        "schema_version",
        "workflow_run_id",
        "workflow_run_attempt",
        "release",
        "request",
        "trust",
        "issue",
        "comment_count",
        "latest_comment_id",
        "latest_snapshot",
        "binding_sha256",
        "readiness_sha256",
    }
)
UPDATE_FIELDS = frozenset(
    {"schema_version", "release_manifest_sha256", "selected_channels", "observations"}
)
OBSERVATION_FIELDS = frozenset({"conclusion", "result"})

SNAPSHOT_PREFIX = "<!-- release-ledger-snapshot:v2\n"
SNAPSHOT_SUFFIX = "\nrelease-ledger-snapshot -->"
SNAPSHOT_MARKER = "release-ledger-snapshot"
DIGEST_PATTERN = re.compile(r"[0-9a-f]{64}")
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
DECIMAL_PATTERN = re.compile(r"0|[1-9][0-9]*")
TIMESTAMP_PATTERN = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z")


class LedgerError(ValueError):
    """Raised when ledger identity, history, or a replay request is unsafe."""


@dataclass(frozen=True)
class BotIdentity:
    """Exact configured identity of the only snapshot-writing principal."""

    user_id: int
    login: str
    user_type: str
    author_association: str

    def to_mapping(self) -> dict[str, object]:
        return {
            "id": self.user_id,
            "login": self.login,
            "type": self.user_type,
            "author_association": self.author_association,
        }


@dataclass(frozen=True)
class ReleaseIdentity:
    """Immutable source identity shared by all cumulative snapshots."""

    tag: str
    version: str
    commit: str

    def __post_init__(self) -> None:
        try:
            parsed = SemVer.from_tag(self.tag)
        except ValidationError as error:
            raise LedgerError(f"release tag is invalid: {error}") from error
        if str(parsed) != self.version:
            raise LedgerError("release tag and version disagree")
        if SHA_PATTERN.fullmatch(self.commit) is None:
            raise LedgerError("release commit must be a lowercase full Git SHA")

    def to_mapping(self) -> dict[str, str]:
        return {"tag": self.tag, "version": self.version, "commit": self.commit}


def canonical_json_bytes(value: object) -> bytes:
    """Serialize policy records in the one accepted stable representation."""

    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    ).encode("ascii")


def sha256_json(value: object) -> str:
    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def expected_issue_body(tag: str) -> str:
    """Return the exact immutable issue body required for a release tag."""

    try:
        SemVer.from_tag(tag)
    except ValidationError as error:
        raise LedgerError(f"release tag is invalid: {error}") from error
    return (
        f"<!-- release-ledger:v2 tag={tag} -->\n"
        "Snapshots are cumulative comments created by release automation.\n"
    )


def selected_channels(value: str | Sequence[str]) -> frozenset[str]:
    """Parse an exact, duplicate-free external channel inventory."""

    if isinstance(value, str):
        items = [] if value == "" else value.split(",")
    elif isinstance(value, Sequence) and not isinstance(value, (bytes, bytearray)):
        items = list(value)
    else:
        raise LedgerError("selected channel inventory must be a comma list or JSON array")
    if any(not isinstance(channel, str) or not channel for channel in items):
        raise LedgerError("selected channel inventory contains an invalid value")
    if len(set(items)) != len(items):
        raise LedgerError("selected channel inventory contains duplicates")
    channels = frozenset(items)
    if not channels <= set(EXTERNAL_CHANNELS):
        raise LedgerError("selected channel inventory is malformed")
    return channels


def _require_mapping(name: str, value: object) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise LedgerError(f"{name} must be an object")
    if any(not isinstance(field, str) for field in value):
        raise LedgerError(f"{name} field names must be strings")
    return value


def _require_exact_fields(name: str, value: object, fields: frozenset[str]) -> Mapping[str, Any]:
    mapping = _require_mapping(name, value)
    if set(mapping) != fields:
        missing = sorted(fields - set(mapping))
        extra = sorted(set(mapping) - fields)
        raise LedgerError(f"{name} fields mismatch; missing={missing}, extra={extra}")
    return mapping


def _require_string(
    name: str,
    value: object,
    *,
    maximum: int = 256,
    allow_empty: bool = False,
    multiline: bool = False,
) -> str:
    if not isinstance(value, str) or (not value and not allow_empty) or len(value) > maximum:
        raise LedgerError(f"{name} must be a bounded string")
    if "\x00" in value or (not multiline and any(ord(character) < 0x20 for character in value)):
        raise LedgerError(f"{name} contains control characters")
    if not multiline and value != value.strip():
        raise LedgerError(f"{name} must be stripped")
    return value


def _require_positive_int(name: str, value: object) -> int:
    if type(value) is not int or value < 1:
        raise LedgerError(f"{name} must be a positive integer")
    return value


def _require_nonnegative_int(name: str, value: object) -> int:
    if type(value) is not int or value < 0:
        raise LedgerError(f"{name} must be a non-negative integer")
    return value


def _require_timestamp(name: str, value: object) -> str:
    timestamp = _require_string(name, value, maximum=32)
    if TIMESTAMP_PATTERN.fullmatch(timestamp) is None:
        raise LedgerError(f"{name} must be a canonical UTC GitHub timestamp")
    return timestamp


def _require_decimal(name: str, value: object, *, positive: bool = False) -> str:
    text = _require_string(name, value, maximum=32)
    if DECIMAL_PATTERN.fullmatch(text) is None or (positive and text == "0"):
        raise LedgerError(f"{name} must be a canonical decimal string")
    return text


def _require_digest(name: str, value: object, *, nullable: bool = False) -> str | None:
    if value is None and nullable:
        return None
    text = _require_string(name, value, maximum=64)
    if DIGEST_PATTERN.fullmatch(text) is None:
        raise LedgerError(f"{name} must be a lowercase SHA-256")
    return text


def _canonical_user(name: str, value: object) -> dict[str, object]:
    user = _require_mapping(name, value)
    return {
        "id": _require_positive_int(f"{name}.id", user.get("id")),
        "node_id": _require_string(f"{name}.node_id", user.get("node_id")),
        "login": _require_string(f"{name}.login", user.get("login")),
        "type": _require_string(f"{name}.type", user.get("type"), maximum=32),
    }


def _validated_bot(bot: BotIdentity) -> BotIdentity:
    if not isinstance(bot, BotIdentity):
        raise LedgerError("configured bot identity has the wrong type")
    validated = BotIdentity(
        _require_positive_int("configured bot.id", bot.user_id),
        _require_string("configured bot.login", bot.login),
        _require_string("configured bot.type", bot.user_type, maximum=32),
        _require_string(
            "configured bot.author_association", bot.author_association, maximum=32
        ),
    )
    if validated.user_type != "Bot":
        raise LedgerError("configured snapshot writer must have GitHub user type Bot")
    return validated


def _validated_distinct_app_bot(bot: BotIdentity) -> BotIdentity:
    """Require a repository-specific GitHub App bot, not the shared Actions bot."""

    validated = _validated_bot(bot)
    if not validated.login.endswith("[bot]"):
        raise LedgerError("configured snapshot writer must be a GitHub App bot login")
    if validated.user_id == 41898282 or validated.login == "github-actions[bot]":
        raise LedgerError("shared github-actions[bot] cannot authenticate ledger snapshots")
    return validated


def _canonical_issue(
    issue: object,
    *,
    identity: ReleaseIdentity,
    issue_number: int,
    owner_id: int,
) -> dict[str, object]:
    value = _require_mapping("ledger issue", issue)
    issue_id = _require_positive_int("ledger issue.id", value.get("id"))
    node_id = _require_string("ledger issue.node_id", value.get("node_id"))
    number = _require_positive_int("ledger issue.number", value.get("number"))
    if number != issue_number:
        raise LedgerError("ledger issue number does not match the dispatch")
    if value.get("pull_request") is not None:
        raise LedgerError("ledger must be an issue, not a pull request")
    if value.get("state") != "open":
        raise LedgerError("ledger issue must remain open")
    if value.get("locked") is not True:
        raise LedgerError("ledger issue must remain locked")
    title = _require_string("ledger issue.title", value.get("title"))
    if title != f"[release] {identity.tag} PREPARING":
        raise LedgerError("ledger issue title is not exact")
    body = _require_string(
        "ledger issue.body",
        value.get("body"),
        maximum=4096,
        multiline=True,
    )
    if body != expected_issue_body(identity.tag):
        raise LedgerError("ledger issue body is not exact")

    user = _canonical_user("ledger issue.user", value.get("user"))
    if user["id"] != owner_id or user["type"] != "User":
        raise LedgerError("ledger issue was not created by the configured numeric owner")
    association = _require_string(
        "ledger issue.author_association",
        value.get("author_association"),
        maximum=32,
    )
    labels = value.get("labels")
    if not isinstance(labels, list) or len(labels) != 1:
        raise LedgerError("ledger issue must have exactly one release-ledger label")
    label = _require_mapping("ledger issue label", labels[0])
    canonical_label = {
        "id": _require_positive_int("ledger issue label.id", label.get("id")),
        "node_id": _require_string("ledger issue label.node_id", label.get("node_id")),
        "name": _require_string("ledger issue label.name", label.get("name")),
    }
    if canonical_label["name"] != "release-ledger":
        raise LedgerError("ledger issue label is not exact")

    return {
        "id": issue_id,
        "node_id": node_id,
        "number": number,
        "url": _require_string("ledger issue.url", value.get("url"), maximum=1024),
        "repository_url": _require_string(
            "ledger issue.repository_url", value.get("repository_url"), maximum=1024
        ),
        "state": "open",
        "locked": True,
        "title": title,
        "body": body,
        "created_at": _require_timestamp("ledger issue.created_at", value.get("created_at")),
        "updated_at": _require_timestamp("ledger issue.updated_at", value.get("updated_at")),
        "comments": _require_nonnegative_int("ledger issue.comments", value.get("comments")),
        "user": user,
        # Association is bound for drift detection but grants no trust.
        "author_association": association,
        "labels": [canonical_label],
    }


def _flatten_comment_pages(document: object) -> list[object]:
    if not isinstance(document, list):
        raise LedgerError("paginated comments must be a JSON array of pages")
    comments: list[object] = []
    for page_index, page in enumerate(document):
        if not isinstance(page, list):
            raise LedgerError(f"paginated comments page {page_index} must be a JSON array")
        comments.extend(page)
    return comments


def _strict_json_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise LedgerError(f"JSON contains duplicate key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise LedgerError(f"JSON contains non-finite number: {value}")


def _strict_json_text(name: str, text: str) -> object:
    try:
        return json.loads(
            text,
            object_pairs_hook=_strict_json_pairs,
            parse_constant=_reject_constant,
        )
    except json.JSONDecodeError as error:
        raise LedgerError(f"{name} is not strict JSON") from error


def _validate_ledger(value: object, identity: ReleaseIdentity) -> dict[str, Any]:
    try:
        ledger = ReleaseLedger.from_mapping(_require_mapping("snapshot ledger", value))
    except ValidationError as error:
        raise LedgerError(str(error)) from error
    normalized = ledger.to_mapping()
    if (
        normalized["tag"] != identity.tag
        or normalized["version"] != identity.version
        or normalized["source_commit_sha"] != identity.commit
    ):
        raise LedgerError("snapshot ledger identity conflicts with the release")
    return normalized


def _validate_snapshot_mapping(value: object, identity: ReleaseIdentity) -> dict[str, Any]:
    snapshot = _require_exact_fields("ledger snapshot", value, SNAPSHOT_FIELDS)
    if snapshot["schema_version"] != 2:
        raise LedgerError("ledger snapshot schema_version must be exactly 2")
    sequence = _require_positive_int("ledger snapshot.sequence", snapshot["sequence"])
    previous = _require_digest(
        "ledger snapshot.previous_snapshot_sha256",
        snapshot["previous_snapshot_sha256"],
        nullable=True,
    )
    readiness = _require_digest("ledger snapshot.readiness_sha256", snapshot["readiness_sha256"])
    run_id = _require_decimal("ledger snapshot.workflow_run_id", snapshot["workflow_run_id"], positive=True)
    run_attempt = _require_decimal(
        "ledger snapshot.workflow_run_attempt",
        snapshot["workflow_run_attempt"],
        positive=True,
    )
    channels = selected_channels(snapshot["selected_channels"])
    if snapshot["selected_channels"] != sorted(channels):
        raise LedgerError("ledger snapshot selected_channels must be sorted")
    return {
        "schema_version": 2,
        "sequence": sequence,
        "previous_snapshot_sha256": previous,
        "readiness_sha256": readiness,
        "workflow_run_id": run_id,
        "workflow_run_attempt": run_attempt,
        "selected_channels": sorted(channels),
        "ledger": _validate_ledger(snapshot["ledger"], identity),
    }


def _snapshot_status(results: Mapping[str, str]) -> str:
    if any(result == "FAILED" for result in results.values()):
        return "FAILED"
    if any(result in {"BLOCKED_MANUAL_ACTION", "FIRST_USE_UNPROVEN"} for result in results.values()):
        return "ATTENTION_REQUIRED"
    return "RECORDED"


def render_snapshot_comment(snapshot: Mapping[str, Any]) -> str:
    """Render one canonical human-readable bot comment with hidden JSON state."""

    ledger = snapshot["ledger"]
    lines = [
        f"Release ledger snapshot **{snapshot['sequence']}** for `{ledger['tag']}`",
        "",
        f"State: **{_snapshot_status(ledger['results'])}**",
        "",
        "| Destination | Result |",
        "| --- | --- |",
    ]
    lines.extend(f"| {destination} | {ledger['results'][destination]} |" for destination in DESTINATIONS)
    lines.extend(
        [
            "",
            SNAPSHOT_PREFIX.rstrip("\n"),
            canonical_json_bytes(snapshot).decode("ascii").rstrip("\n"),
            SNAPSHOT_SUFFIX.lstrip("\n"),
            "",
        ]
    )
    return "\n".join(lines)


def snapshot_sha256(snapshot: Mapping[str, Any]) -> str:
    return sha256_json(snapshot)


def parse_snapshot_comment(body: str, identity: ReleaseIdentity) -> dict[str, Any]:
    """Parse only the exact canonical body produced by ``render_snapshot_comment``."""

    if body.count(SNAPSHOT_PREFIX) != 1 or body.count(SNAPSHOT_SUFFIX) != 1:
        raise LedgerError("bot comment contains an invalid snapshot envelope")
    payload = body.split(SNAPSHOT_PREFIX, 1)[1].split(SNAPSHOT_SUFFIX, 1)[0]
    snapshot = _validate_snapshot_mapping(_strict_json_text("ledger snapshot", payload), identity)
    if render_snapshot_comment(snapshot) != body:
        raise LedgerError("bot snapshot comment is not canonical")
    return snapshot


def _validate_snapshot_transition(
    previous: dict[str, Any] | None,
    current: dict[str, Any],
) -> None:
    expected_sequence = 1 if previous is None else previous["sequence"] + 1
    expected_digest = None if previous is None else snapshot_sha256(previous)
    if current["sequence"] != expected_sequence:
        raise LedgerError("ledger snapshot sequence is not contiguous")
    if current["previous_snapshot_sha256"] != expected_digest:
        raise LedgerError("ledger snapshot hash chain is broken")
    if previous is None:
        previous_results = {destination: "NOT_SELECTED" for destination in DESTINATIONS}
        previous_manifest = None
    else:
        previous_results = previous["ledger"]["results"]
        previous_manifest = previous["ledger"]["release_manifest_sha256"]
        current_manifest = current["ledger"]["release_manifest_sha256"]
        if previous_manifest is not None and current_manifest != previous_manifest:
            raise LedgerError("ledger snapshot changed or removed the anchored manifest digest")

    selected_destinations = {
        destination
        for destination, (channel, _) in DESTINATIONS.items()
        if channel in set(current["selected_channels"])
    }
    for destination in DESTINATIONS:
        old = previous_results[destination]
        new = current["ledger"]["results"][destination]
        if destination != "github" and destination not in selected_destinations and new != old:
            raise LedgerError("ledger snapshot changed an unselected destination")
        if old in DURABLE_RESULTS and new not in DURABLE_RESULTS:
            raise LedgerError("ledger snapshot downgraded durable publication state")


def _canonical_comments(
    issue: Mapping[str, object],
    comments_document: object,
    *,
    identity: ReleaseIdentity,
    bot: BotIdentity,
) -> tuple[list[dict[str, object]], list[dict[str, Any]]]:
    raw_comments = _flatten_comment_pages(comments_document)
    if issue["comments"] != len(raw_comments):
        raise LedgerError("paginated comment count does not match the issue")

    canonical: list[dict[str, object]] = []
    snapshots: list[dict[str, Any]] = []
    prior_snapshot: dict[str, Any] | None = None
    prior_id = 0
    prior_created = ""
    seen_nodes: set[str] = set()
    seen_runs: set[tuple[str, str]] = set()
    for index, raw_comment in enumerate(raw_comments):
        name = f"ledger comment[{index}]"
        comment = _require_mapping(name, raw_comment)
        comment_id = _require_positive_int(f"{name}.id", comment.get("id"))
        node_id = _require_string(f"{name}.node_id", comment.get("node_id"))
        created_at = _require_timestamp(f"{name}.created_at", comment.get("created_at"))
        updated_at = _require_timestamp(f"{name}.updated_at", comment.get("updated_at"))
        if updated_at != created_at:
            raise LedgerError("ledger comments must never be edited")
        if comment_id <= prior_id or (prior_created and created_at < prior_created):
            raise LedgerError("ledger comments are not in monotonic GitHub order")
        if node_id in seen_nodes:
            raise LedgerError("ledger comments contain a duplicate node ID")
        prior_id = comment_id
        prior_created = created_at
        seen_nodes.add(node_id)

        issue_url = _require_string(f"{name}.issue_url", comment.get("issue_url"), maximum=1024)
        if issue_url != issue["url"]:
            raise LedgerError("ledger comment belongs to a different issue")
        body = _require_string(
            f"{name}.body",
            comment.get("body"),
            maximum=65536,
            multiline=True,
        )
        user = _canonical_user(f"{name}.user", comment.get("user"))
        association = _require_string(
            f"{name}.author_association",
            comment.get("author_association"),
            maximum=32,
        )
        exact_bot = (
            user["id"] == bot.user_id
            and user["login"] == bot.login
            and user["type"] == bot.user_type
            and association == bot.author_association
        )
        if exact_bot:
            snapshot = parse_snapshot_comment(body, identity)
            _validate_snapshot_transition(prior_snapshot, snapshot)
            run_identity = (snapshot["workflow_run_id"], snapshot["workflow_run_attempt"])
            if run_identity in seen_runs:
                raise LedgerError("a workflow run attempted more than one ledger snapshot")
            seen_runs.add(run_identity)
            prior_snapshot = snapshot
            snapshots.append(
                {
                    "comment_id": comment_id,
                    "comment_node_id": node_id,
                    "snapshot_sha256": snapshot_sha256(snapshot),
                    "snapshot": snapshot,
                }
            )
        elif SNAPSHOT_MARKER in body:
            raise LedgerError("snapshot marker was posted by an untrusted or spoofed principal")

        canonical.append(
            {
                "id": comment_id,
                "node_id": node_id,
                "issue_url": issue_url,
                "body": body,
                "body_sha256": hashlib.sha256(body.encode("utf-8")).hexdigest(),
                "created_at": created_at,
                "updated_at": updated_at,
                "user": user,
                "author_association": association,
            }
        )
    return canonical, snapshots


def _validate_replay(
    *,
    prior: dict[str, Any] | None,
    channels: frozenset[str],
    mode: str,
    retry_authorized: bool,
    anchor_exists: bool,
) -> None:
    if mode not in {"validate", "publish"}:
        raise LedgerError("dispatch mode is malformed")
    if mode == "publish" and retry_authorized and not anchor_exists:
        raise LedgerError("source retry requires an existing immutable GitHub anchor")
    if prior is None:
        if mode == "publish" and anchor_exists and not retry_authorized:
            raise LedgerError("existing anchor without a bot snapshot requires explicit retry")
        return

    ledger = prior["snapshot"]["ledger"]
    results = ledger["results"]
    manifest = ledger["release_manifest_sha256"]
    durable = {destination for destination, result in results.items() if result in DURABLE_RESULTS}
    if not anchor_exists:
        if manifest is not None or durable:
            raise LedgerError("pre-anchor snapshot claims durable publication state")
    elif manifest is None and durable:
        raise LedgerError("durable snapshot results require an anchored manifest digest")

    if mode != "publish":
        return
    destination_for_channel = {
        channel: destination for destination, (channel, _) in DESTINATIONS.items()
    }
    if retry_authorized:
        invalid = sorted(
            channel
            for channel in channels
            if results[destination_for_channel[channel]] not in SOURCE_RETRYABLE_RESULTS
        )
        if invalid:
            raise LedgerError(
                "selected channels do not have source-retryable ledger state: " + ",".join(invalid)
            )
    elif anchor_exists:
        invalid = sorted(
            channel
            for channel in channels
            if results[destination_for_channel[channel]] != "NOT_SELECTED"
        )
        if invalid:
            raise LedgerError(
                "selected continuation channels require NOT_SELECTED ledger state: "
                + ",".join(invalid)
            )


def build_readiness(
    *,
    issue: object,
    comments: object,
    identity: ReleaseIdentity,
    issue_number: int,
    owner_id: int,
    bot: BotIdentity,
    channels: frozenset[str],
    mode: str,
    retry_authorized: bool,
    anchor_exists: bool,
    workflow_run_id: str,
    workflow_run_attempt: str,
) -> dict[str, Any]:
    """Validate current GitHub state and issue an immutable readiness token."""

    issue_number = _require_positive_int("issue_number", issue_number)
    owner_id = _require_positive_int("owner_id", owner_id)
    run_id = _require_decimal("workflow_run_id", workflow_run_id, positive=True)
    run_attempt = _require_decimal("workflow_run_attempt", workflow_run_attempt, positive=True)
    bot = _validated_distinct_app_bot(bot)
    if bot.user_id == owner_id:
        raise LedgerError("configured owner and snapshot bot must have different numeric IDs")
    canonical_issue = _canonical_issue(
        issue,
        identity=identity,
        issue_number=issue_number,
        owner_id=owner_id,
    )
    canonical_comments, snapshots = _canonical_comments(
        canonical_issue,
        comments,
        identity=identity,
        bot=bot,
    )
    prior = snapshots[-1] if snapshots else None
    _validate_replay(
        prior=prior,
        channels=channels,
        mode=mode,
        retry_authorized=retry_authorized,
        anchor_exists=anchor_exists,
    )
    binding = {
        "schema_version": 1,
        "issue": canonical_issue,
        "comments": canonical_comments,
    }
    record: dict[str, Any] = {
        "schema_version": 1,
        "workflow_run_id": run_id,
        "workflow_run_attempt": run_attempt,
        "release": identity.to_mapping(),
        "request": {
            "channels": sorted(channels),
            "mode": mode,
            "retry_authorized": retry_authorized,
            "anchor_exists": anchor_exists,
        },
        "trust": {
            "owner_id": owner_id,
            "bot": bot.to_mapping(),
        },
        "issue": {
            "id": canonical_issue["id"],
            "node_id": canonical_issue["node_id"],
            "number": canonical_issue["number"],
            "updated_at": canonical_issue["updated_at"],
        },
        "comment_count": len(canonical_comments),
        "latest_comment_id": canonical_comments[-1]["id"] if canonical_comments else None,
        "latest_snapshot": prior,
        "binding_sha256": sha256_json(binding),
    }
    record["readiness_sha256"] = sha256_json(record)
    return record


def _validate_readiness_record(value: object) -> dict[str, Any]:
    record = dict(_require_exact_fields("readiness record", value, READINESS_FIELDS))
    expected_token = _require_digest("readiness record.readiness_sha256", record["readiness_sha256"])
    unsigned = {field: record[field] for field in READINESS_FIELDS if field != "readiness_sha256"}
    if sha256_json(unsigned) != expected_token:
        raise LedgerError("readiness record digest is invalid")
    if record["schema_version"] != 1:
        raise LedgerError("readiness record schema_version must be exactly 1")
    release = _require_exact_fields(
        "readiness record.release",
        record["release"],
        frozenset({"tag", "version", "commit"}),
    )
    identity = ReleaseIdentity(
        _require_string("readiness release.tag", release["tag"]),
        _require_string("readiness release.version", release["version"]),
        _require_string("readiness release.commit", release["commit"]),
    )
    request = _require_exact_fields(
        "readiness record.request",
        record["request"],
        frozenset({"channels", "mode", "retry_authorized", "anchor_exists"}),
    )
    channels = selected_channels(request["channels"])
    if request["channels"] != sorted(channels):
        raise LedgerError("readiness channels must be sorted")
    if request["mode"] not in {"validate", "publish"}:
        raise LedgerError("readiness mode is invalid")
    if type(request["retry_authorized"]) is not bool or type(request["anchor_exists"]) is not bool:
        raise LedgerError("readiness boolean fields are invalid")
    trust = _require_exact_fields(
        "readiness record.trust",
        record["trust"],
        frozenset({"owner_id", "bot"}),
    )
    bot_value = _require_exact_fields(
        "readiness record bot",
        trust["bot"],
        frozenset({"id", "login", "type", "author_association"}),
    )
    bot = _validated_distinct_app_bot(BotIdentity(
        _require_positive_int("readiness bot.id", bot_value["id"]),
        _require_string("readiness bot.login", bot_value["login"]),
        _require_string("readiness bot.type", bot_value["type"]),
        _require_string("readiness bot.author_association", bot_value["author_association"]),
    ))
    owner_id = _require_positive_int("readiness owner_id", trust["owner_id"])
    if bot.user_id == owner_id:
        raise LedgerError("readiness owner and snapshot bot must have different numeric IDs")
    issue = _require_exact_fields(
        "readiness record.issue",
        record["issue"],
        frozenset({"id", "node_id", "number", "updated_at"}),
    )
    _require_positive_int("readiness issue.id", issue["id"])
    _require_string("readiness issue.node_id", issue["node_id"])
    _require_positive_int("readiness issue.number", issue["number"])
    _require_timestamp("readiness issue.updated_at", issue["updated_at"])
    if type(record["comment_count"]) is not int or record["comment_count"] < 0:
        raise LedgerError("readiness comment_count is invalid")
    if record["latest_comment_id"] is not None:
        _require_positive_int("readiness latest_comment_id", record["latest_comment_id"])
    if record["latest_snapshot"] is not None:
        latest = _require_exact_fields(
            "readiness latest_snapshot",
            record["latest_snapshot"],
            frozenset({"comment_id", "comment_node_id", "snapshot_sha256", "snapshot"}),
        )
        _require_positive_int("readiness latest snapshot comment_id", latest["comment_id"])
        _require_string("readiness latest snapshot comment_node_id", latest["comment_node_id"])
        digest = _require_digest("readiness latest snapshot digest", latest["snapshot_sha256"])
        snapshot = _validate_snapshot_mapping(latest["snapshot"], identity)
        if snapshot_sha256(snapshot) != digest:
            raise LedgerError("readiness latest snapshot digest is invalid")
    _require_digest("readiness binding_sha256", record["binding_sha256"])
    _require_decimal("readiness workflow_run_id", record["workflow_run_id"], positive=True)
    _require_decimal(
        "readiness workflow_run_attempt", record["workflow_run_attempt"], positive=True
    )
    # Return normalized nested identity objects only through local reconstruction.
    record["_identity"] = identity
    record["_channels"] = channels
    record["_bot"] = bot
    return record


def recheck_readiness(
    *,
    readiness: object,
    expected_readiness_sha256: str,
    issue: object,
    comments: object,
    issue_number: int,
    owner_id: int,
    bot: BotIdentity,
    workflow_run_id: str,
    workflow_run_attempt: str,
) -> dict[str, Any]:
    """Fail if any bound issue/comment/request/trust fact drifted since readiness."""

    record = _validate_readiness_record(readiness)
    bot = _validated_distinct_app_bot(bot)
    token = _require_digest("expected readiness SHA-256", expected_readiness_sha256)
    if record["readiness_sha256"] != token:
        raise LedgerError("readiness token does not match the trusted job output")
    if (
        record["workflow_run_id"] != workflow_run_id
        or record["workflow_run_attempt"] != workflow_run_attempt
    ):
        raise LedgerError("readiness token belongs to a different workflow run")
    if record["issue"]["number"] != issue_number or record["trust"]["owner_id"] != owner_id:
        raise LedgerError("readiness issue or owner identity changed")
    if record["trust"]["bot"] != bot.to_mapping():
        raise LedgerError("readiness bot identity changed")
    current = build_readiness(
        issue=issue,
        comments=comments,
        identity=record["_identity"],
        issue_number=issue_number,
        owner_id=owner_id,
        bot=bot,
        channels=record["_channels"],
        mode=record["request"]["mode"],
        retry_authorized=record["request"]["retry_authorized"],
        anchor_exists=record["request"]["anchor_exists"],
        workflow_run_id=workflow_run_id,
        workflow_run_attempt=workflow_run_attempt,
    )
    comparable = {field: value for field, value in record.items() if not field.startswith("_")}
    if current != comparable:
        raise LedgerError("ledger issue or comments changed after readiness")
    return current


def verify_append(
    *,
    readiness: object,
    expected_readiness_sha256: str,
    snapshot: object,
    before_issue: object,
    before_comments: object,
    after_issue: object,
    after_comments: object,
    issue_number: int,
    owner_id: int,
    bot: BotIdentity,
    workflow_run_id: str,
    workflow_run_attempt: str,
) -> dict[str, object]:
    """Verify that POST added exactly one expected, canonical App-bot snapshot."""

    verified_before = recheck_readiness(
        readiness=readiness,
        expected_readiness_sha256=expected_readiness_sha256,
        issue=before_issue,
        comments=before_comments,
        issue_number=issue_number,
        owner_id=owner_id,
        bot=bot,
        workflow_run_id=workflow_run_id,
        workflow_run_attempt=workflow_run_attempt,
    )
    record = _validate_readiness_record(verified_before)
    bot = _validated_distinct_app_bot(bot)
    expected_snapshot = _validate_snapshot_mapping(snapshot, record["_identity"])
    if expected_snapshot["readiness_sha256"] != record["readiness_sha256"]:
        raise LedgerError("appended snapshot does not bind the trusted readiness token")
    if (
        expected_snapshot["workflow_run_id"] != record["workflow_run_id"]
        or expected_snapshot["workflow_run_attempt"] != record["workflow_run_attempt"]
    ):
        raise LedgerError("appended snapshot belongs to a different workflow run")
    if expected_snapshot["selected_channels"] != record["request"]["channels"]:
        raise LedgerError("appended snapshot channels do not match readiness")
    prior_entry = record["latest_snapshot"]
    prior_snapshot = None if prior_entry is None else prior_entry["snapshot"]
    _validate_snapshot_transition(prior_snapshot, expected_snapshot)

    identity = record["_identity"]
    canonical_before_issue = _canonical_issue(
        before_issue,
        identity=identity,
        issue_number=issue_number,
        owner_id=owner_id,
    )
    canonical_before_comments, before_snapshots = _canonical_comments(
        canonical_before_issue,
        before_comments,
        identity=identity,
        bot=bot,
    )
    canonical_after_issue = _canonical_issue(
        after_issue,
        identity=identity,
        issue_number=issue_number,
        owner_id=owner_id,
    )
    canonical_after_comments, after_snapshots = _canonical_comments(
        canonical_after_issue,
        after_comments,
        identity=identity,
        bot=bot,
    )

    if len(canonical_after_comments) != len(canonical_before_comments) + 1:
        raise LedgerError("ledger POST must add exactly one comment")
    if canonical_after_comments[:-1] != canonical_before_comments:
        raise LedgerError("ledger comments changed while appending the snapshot")
    if canonical_after_issue["comments"] != canonical_before_issue["comments"] + 1:
        raise LedgerError("ledger issue comment count did not increase by exactly one")
    if canonical_after_issue["updated_at"] < canonical_before_issue["updated_at"]:
        raise LedgerError("ledger issue update timestamp moved backwards")
    ignored_issue_fields = {"comments", "updated_at"}
    before_static = {
        field: value
        for field, value in canonical_before_issue.items()
        if field not in ignored_issue_fields
    }
    after_static = {
        field: value
        for field, value in canonical_after_issue.items()
        if field not in ignored_issue_fields
    }
    if after_static != before_static:
        raise LedgerError("ledger issue identity changed while appending the snapshot")

    new_comment = canonical_after_comments[-1]
    new_user = new_comment["user"]
    if (
        new_user["id"] != bot.user_id
        or new_user["login"] != bot.login
        or new_user["type"] != bot.user_type
        or new_comment["author_association"] != bot.author_association
    ):
        raise LedgerError("new ledger comment was not created by the configured App bot")
    expected_body = render_snapshot_comment(expected_snapshot)
    if new_comment["body"] != expected_body:
        raise LedgerError("new ledger comment body does not match the rendered snapshot")
    if len(after_snapshots) != len(before_snapshots) + 1:
        raise LedgerError("new ledger comment is not exactly one canonical snapshot")
    if after_snapshots[:-1] != before_snapshots:
        raise LedgerError("prior ledger snapshot history changed during append")
    appended = after_snapshots[-1]
    if (
        appended["comment_id"] != new_comment["id"]
        or appended["comment_node_id"] != new_comment["node_id"]
        or appended["snapshot"] != expected_snapshot
    ):
        raise LedgerError("new ledger comment does not contain the expected snapshot")
    digest = snapshot_sha256(expected_snapshot)
    if appended["snapshot_sha256"] != digest:
        raise LedgerError("new ledger snapshot digest is inconsistent")
    return {
        "comment_id": new_comment["id"],
        "comment_node_id": new_comment["node_id"],
        "snapshot_sha256": digest,
    }


def _validate_updates(value: object, expected_channels: frozenset[str]) -> dict[str, Any]:
    updates = _require_exact_fields("ledger updates", value, UPDATE_FIELDS)
    if updates["schema_version"] != 1:
        raise LedgerError("ledger updates schema_version must be exactly 1")
    manifest = _require_digest(
        "ledger updates.release_manifest_sha256",
        updates["release_manifest_sha256"],
        nullable=True,
    )
    channels = selected_channels(updates["selected_channels"])
    if updates["selected_channels"] != sorted(channels) or channels != expected_channels:
        raise LedgerError("ledger update channels do not match readiness")
    observations = _require_mapping("ledger updates.observations", updates["observations"])
    if set(observations) != set(DESTINATIONS):
        raise LedgerError("ledger observations must contain every destination exactly once")
    normalized: dict[str, dict[str, str | None]] = {}
    for destination in DESTINATIONS:
        observation = _require_exact_fields(
            f"ledger observation {destination}",
            observations[destination],
            OBSERVATION_FIELDS,
        )
        conclusion = _require_string(
            f"ledger observation {destination}.conclusion",
            observation["conclusion"],
            maximum=16,
        )
        if conclusion not in {"success", "failure", "cancelled", "skipped"}:
            raise LedgerError("ledger observation has an unknown job conclusion")
        result = observation["result"]
        if result is not None:
            result = _require_string(f"ledger observation {destination}.result", result)
            if result not in ALLOWED_RESULTS:
                raise LedgerError("ledger observation has an unknown result")
        normalized[destination] = {"conclusion": conclusion, "result": result}
    return {
        "schema_version": 1,
        "release_manifest_sha256": manifest,
        "selected_channels": sorted(channels),
        "observations": normalized,
    }


def merge_snapshot(
    *,
    readiness: object,
    updates: object,
    expected_readiness_sha256: str,
) -> tuple[dict[str, Any], str]:
    """Merge one cumulative snapshot without erasing prior durable results."""

    record = _validate_readiness_record(readiness)
    expected_token = _require_digest(
        "expected readiness SHA-256", expected_readiness_sha256
    )
    if record["readiness_sha256"] != expected_token:
        raise LedgerError("readiness token does not match the trusted job output")
    normalized_updates = _validate_updates(updates, record["_channels"])
    previous_entry = record["latest_snapshot"]
    if previous_entry is None:
        previous_snapshot = None
        previous_ledger = {
            "schema_version": 1,
            "version": record["release"]["version"],
            "tag": record["release"]["tag"],
            "source_commit_sha": record["release"]["commit"],
            "release_manifest_sha256": None,
            "results": {destination: "NOT_SELECTED" for destination in DESTINATIONS},
        }
    else:
        previous_snapshot = previous_entry["snapshot"]
        previous_ledger = previous_snapshot["ledger"]

    selected_destinations = {
        destination
        for destination, (channel, _) in DESTINATIONS.items()
        if channel in record["_channels"]
    }
    results = dict(previous_ledger["results"])
    for destination in DESTINATIONS:
        observation = normalized_updates["observations"][destination]
        selected = destination == "github" or destination in selected_destinations
        if not selected:
            if observation != {"conclusion": "skipped", "result": None}:
                raise LedgerError("unselected destinations must be exactly skipped without a result")
            continue
        if observation["conclusion"] == "success":
            candidate = observation["result"]
            if candidate in {None, "NOT_SELECTED", "FAILED"}:
                raise LedgerError("successful selected jobs require a concrete non-failure result")
        elif observation["conclusion"] in {"failure", "cancelled"}:
            if observation["result"] is not None:
                raise LedgerError("failed or cancelled jobs must not claim a result")
            candidate = "FAILED"
        else:
            raise LedgerError("selected destinations must not be skipped")
        if results[destination] in DURABLE_RESULTS and candidate not in DURABLE_RESULTS:
            continue
        results[destination] = candidate

    prior_manifest = previous_ledger["release_manifest_sha256"]
    update_manifest = normalized_updates["release_manifest_sha256"]
    if prior_manifest is not None and update_manifest not in {None, prior_manifest}:
        raise LedgerError("ledger update manifest conflicts with the anchored manifest")
    manifest = prior_manifest if prior_manifest is not None else update_manifest
    ledger_value = {
        "schema_version": 1,
        "version": record["release"]["version"],
        "tag": record["release"]["tag"],
        "source_commit_sha": record["release"]["commit"],
        "release_manifest_sha256": manifest,
        "results": results,
    }
    identity = record["_identity"]
    ledger = _validate_ledger(ledger_value, identity)
    snapshot = {
        "schema_version": 2,
        "sequence": 1 if previous_snapshot is None else previous_snapshot["sequence"] + 1,
        "previous_snapshot_sha256": (
            None if previous_snapshot is None else snapshot_sha256(previous_snapshot)
        ),
        "readiness_sha256": record["readiness_sha256"],
        "workflow_run_id": record["workflow_run_id"],
        "workflow_run_attempt": record["workflow_run_attempt"],
        "selected_channels": normalized_updates["selected_channels"],
        "ledger": ledger,
    }
    if snapshot["selected_channels"] != record["request"]["channels"]:
        raise LedgerError("merged snapshot channels do not match readiness")
    _validate_snapshot_transition(previous_snapshot, snapshot)
    return snapshot, render_snapshot_comment(snapshot)


def _load_json(path: Path) -> object:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise LedgerError(f"cannot read {path}: {error}") from error
    if not data or len(data) > 2 * 1024 * 1024:
        raise LedgerError(f"{path} must be a non-empty JSON file no larger than 2 MiB")
    try:
        text = data.decode("utf-8")
    except UnicodeError as error:
        raise LedgerError(f"{path} is not UTF-8") from error
    return _strict_json_text(str(path), text)


def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
    except BaseException:
        Path(temporary_name).unlink(missing_ok=True)
        raise


def _write_output(path: Path | None, name: str, value: str) -> None:
    if path is None:
        return
    if "\n" in value or "\r" in value:
        raise LedgerError("GitHub output values must be single-line")
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"{name}={value}\n")


def _bot_from_args(args: argparse.Namespace) -> BotIdentity:
    return BotIdentity(
        _require_positive_int("bot_id", args.bot_id),
        _require_string("bot_login", args.bot_login),
        _require_string("bot_type", args.bot_type, maximum=32),
        _require_string("bot_association", args.bot_association, maximum=32),
    )


def _readiness_command(args: argparse.Namespace) -> None:
    identity = ReleaseIdentity(args.tag, args.version, args.commit)
    record = build_readiness(
        issue=_load_json(args.issue),
        comments=_load_json(args.comments),
        identity=identity,
        issue_number=args.issue_number,
        owner_id=args.owner_id,
        bot=_bot_from_args(args),
        channels=selected_channels(args.channels),
        mode=args.mode,
        retry_authorized=args.retry_authorized == "true",
        anchor_exists=args.anchor_exists == "true",
        workflow_run_id=args.workflow_run_id,
        workflow_run_attempt=args.workflow_run_attempt,
    )
    _write_atomic(args.output, canonical_json_bytes(record))
    prior = record["latest_snapshot"]
    prior_manifest = "" if prior is None else prior["snapshot"]["ledger"]["release_manifest_sha256"] or ""
    _write_output(args.github_output, "ledger_readiness_sha256", record["readiness_sha256"])
    _write_output(args.github_output, "ledger_binding_sha256", record["binding_sha256"])
    _write_output(args.github_output, "ledger_issue_id", str(record["issue"]["id"]))
    _write_output(args.github_output, "ledger_issue_node_id", record["issue"]["node_id"])
    _write_output(args.github_output, "prior_manifest_sha256", prior_manifest)
    _write_output(args.github_output, "prior_snapshot_present", str(prior is not None).lower())


def _recheck_command(args: argparse.Namespace) -> None:
    record = recheck_readiness(
        readiness=_load_json(args.readiness),
        expected_readiness_sha256=args.expected_readiness_sha256,
        issue=_load_json(args.issue),
        comments=_load_json(args.comments),
        issue_number=args.issue_number,
        owner_id=args.owner_id,
        bot=_bot_from_args(args),
        workflow_run_id=args.workflow_run_id,
        workflow_run_attempt=args.workflow_run_attempt,
    )
    _write_output(args.github_output, "ledger_recheck", "VALID")
    _write_output(args.github_output, "ledger_binding_sha256", record["binding_sha256"])


def _merge_command(args: argparse.Namespace) -> None:
    snapshot, body = merge_snapshot(
        readiness=_load_json(args.readiness),
        updates=_load_json(args.updates),
        expected_readiness_sha256=args.expected_readiness_sha256,
    )
    _write_atomic(args.snapshot_output, canonical_json_bytes(snapshot))
    _write_atomic(args.comment_output, body.encode("utf-8"))
    _write_output(args.github_output, "ledger_snapshot_sha256", snapshot_sha256(snapshot))


def _verify_append_command(args: argparse.Namespace) -> None:
    receipt = verify_append(
        readiness=_load_json(args.readiness),
        expected_readiness_sha256=args.expected_readiness_sha256,
        snapshot=_load_json(args.snapshot),
        before_issue=_load_json(args.before_issue),
        before_comments=_load_json(args.before_comments),
        after_issue=_load_json(args.after_issue),
        after_comments=_load_json(args.after_comments),
        issue_number=args.issue_number,
        owner_id=args.owner_id,
        bot=_bot_from_args(args),
        workflow_run_id=args.workflow_run_id,
        workflow_run_attempt=args.workflow_run_attempt,
    )
    _write_output(args.github_output, "ledger_append", "VALID")
    _write_output(args.github_output, "ledger_comment_id", str(receipt["comment_id"]))
    _write_output(args.github_output, "ledger_snapshot_sha256", receipt["snapshot_sha256"])


def _add_trust_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--issue-number", type=int, required=True)
    parser.add_argument("--owner-id", type=int, required=True)
    parser.add_argument("--bot-id", type=int, required=True)
    parser.add_argument("--bot-login", required=True)
    parser.add_argument("--bot-type", required=True)
    parser.add_argument("--bot-association", required=True)
    parser.add_argument("--workflow-run-id", required=True)
    parser.add_argument("--workflow-run-attempt", required=True)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    readiness = commands.add_parser("readiness")
    readiness.add_argument("--issue", type=Path, required=True)
    readiness.add_argument("--comments", type=Path, required=True)
    readiness.add_argument("--tag", required=True)
    readiness.add_argument("--version", required=True)
    readiness.add_argument("--commit", required=True)
    readiness.add_argument("--channels", required=True)
    readiness.add_argument("--mode", choices=("validate", "publish"), required=True)
    readiness.add_argument("--retry-authorized", choices=("true", "false"), required=True)
    readiness.add_argument("--anchor-exists", choices=("true", "false"), required=True)
    readiness.add_argument("--output", type=Path, required=True)
    readiness.add_argument("--github-output", type=Path)
    _add_trust_arguments(readiness)
    readiness.set_defaults(handler=_readiness_command)

    recheck = commands.add_parser("recheck")
    recheck.add_argument("--readiness", type=Path, required=True)
    recheck.add_argument("--expected-readiness-sha256", required=True)
    recheck.add_argument("--issue", type=Path, required=True)
    recheck.add_argument("--comments", type=Path, required=True)
    recheck.add_argument("--github-output", type=Path)
    _add_trust_arguments(recheck)
    recheck.set_defaults(handler=_recheck_command)

    merge = commands.add_parser("render-merge")
    merge.add_argument("--readiness", type=Path, required=True)
    merge.add_argument("--expected-readiness-sha256", required=True)
    merge.add_argument("--updates", type=Path, required=True)
    merge.add_argument("--snapshot-output", type=Path, required=True)
    merge.add_argument("--comment-output", type=Path, required=True)
    merge.add_argument("--github-output", type=Path)
    merge.set_defaults(handler=_merge_command)

    verify = commands.add_parser("verify-append")
    verify.add_argument("--readiness", type=Path, required=True)
    verify.add_argument("--expected-readiness-sha256", required=True)
    verify.add_argument("--snapshot", type=Path, required=True)
    verify.add_argument("--before-issue", type=Path, required=True)
    verify.add_argument("--before-comments", type=Path, required=True)
    verify.add_argument("--after-issue", type=Path, required=True)
    verify.add_argument("--after-comments", type=Path, required=True)
    verify.add_argument("--github-output", type=Path)
    _add_trust_arguments(verify)
    verify.set_defaults(handler=_verify_append_command)

    arguments = parser.parse_args(argv)
    try:
        arguments.handler(arguments)
    except (LedgerError, OSError, UnicodeError) as error:
        print(f"release-ledger: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
