#!/usr/bin/env python3
"""Exhaustive offline tests for the cumulative release ledger policy."""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from release.ledger import (
    BotIdentity,
    DESTINATIONS,
    LedgerError,
    ReleaseIdentity,
    build_readiness,
    expected_issue_body,
    merge_snapshot,
    recheck_readiness,
    render_snapshot_comment,
    snapshot_sha256,
    verify_append,
)


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "release/ledger.py"
OWNER_ID = 4281771
BOT = BotIdentity(987654321, "mcp-cpp-sdk-ledger[bot]", "Bot", "NONE")
IDENTITY = ReleaseIdentity("v0.2.0", "0.2.0", "1" * 40)
MANIFEST = "2" * 64
ISSUE_URL = "https://api.github.com/repos/example/mcp-cpp-sdk/issues/17"


def issue(*, comments: int = 0, updated_at: str = "2026-07-19T01:00:00Z") -> dict[str, object]:
    return {
        "id": 9001,
        "node_id": "I_kwDOReleaseLedger",
        "number": 17,
        "url": ISSUE_URL,
        "repository_url": "https://api.github.com/repos/example/mcp-cpp-sdk",
        "state": "open",
        "locked": True,
        "title": "[release] v0.2.0 PREPARING",
        "body": expected_issue_body("v0.2.0"),
        "created_at": "2026-07-19T01:00:00Z",
        "updated_at": updated_at,
        "comments": comments,
        "user": {
            "id": OWNER_ID,
            "node_id": "U_owner",
            "login": "repository-owner",
            "type": "User",
        },
        "author_association": "OWNER",
        "labels": [
            {"id": 7001, "node_id": "LA_release_ledger", "name": "release-ledger"}
        ],
    }


def bot_comment(
    snapshot: dict[str, object],
    *,
    comment_id: int = 10001,
    created_at: str = "2026-07-19T01:01:00Z",
) -> dict[str, object]:
    return {
        "id": comment_id,
        "node_id": f"IC_{comment_id}",
        "issue_url": ISSUE_URL,
        "body": render_snapshot_comment(snapshot),
        "created_at": created_at,
        "updated_at": created_at,
        "user": {
            "id": BOT.user_id,
            "node_id": "MDM6Qm90NDE4OTgyODI=",
            "login": BOT.login,
            "type": BOT.user_type,
        },
        "author_association": BOT.author_association,
    }


def human_comment(
    *,
    comment_id: int = 10000,
    body: str = "Human note without machine state.",
    created_at: str = "2026-07-19T01:00:30Z",
) -> dict[str, object]:
    return {
        "id": comment_id,
        "node_id": f"IC_{comment_id}",
        "issue_url": ISSUE_URL,
        "body": body,
        "created_at": created_at,
        "updated_at": created_at,
        "user": {"id": 555, "node_id": "U_human", "login": "collaborator", "type": "User"},
        "author_association": "COLLABORATOR",
    }


def observations(
    *,
    selected: dict[str, str] | None = None,
    github_result: str = "PUBLISHED",
    github_conclusion: str = "success",
) -> dict[str, dict[str, str | None]]:
    value = {
        destination: {"conclusion": "skipped", "result": None}
        for destination in DESTINATIONS
    }
    value["github"] = {
        "conclusion": github_conclusion,
        "result": github_result if github_conclusion == "success" else None,
    }
    for destination, result in (selected or {}).items():
        value[destination] = {"conclusion": "success", "result": result}
    return value


def updates(
    channels: list[str],
    *,
    selected: dict[str, str] | None = None,
    manifest: str | None = MANIFEST,
    github_result: str = "PUBLISHED",
    github_conclusion: str = "success",
) -> dict[str, object]:
    return {
        "schema_version": 1,
        "release_manifest_sha256": manifest,
        "selected_channels": channels,
        "observations": observations(
            selected=selected,
            github_result=github_result,
            github_conclusion=github_conclusion,
        ),
    }


def readiness(
    *,
    issue_value: dict[str, object] | None = None,
    pages: list[list[dict[str, object]]] | None = None,
    channels: frozenset[str] = frozenset(),
    mode: str = "publish",
    retry: bool = False,
    anchor: bool = False,
    run_id: str = "123",
    run_attempt: str = "1",
) -> dict[str, object]:
    pages = [[]] if pages is None else pages
    count = sum(len(page) for page in pages)
    current_issue = issue(comments=count) if issue_value is None else issue_value
    return build_readiness(
        issue=current_issue,
        comments=pages,
        identity=IDENTITY,
        issue_number=17,
        owner_id=OWNER_ID,
        bot=BOT,
        channels=channels,
        mode=mode,
        retry_authorized=retry,
        anchor_exists=anchor,
        workflow_run_id=run_id,
        workflow_run_attempt=run_attempt,
    )


def first_snapshot(
    *,
    channels: list[str] | None = None,
    selected: dict[str, str] | None = None,
    github_result: str = "PUBLISHED",
    manifest: str | None = MANIFEST,
) -> dict[str, object]:
    channel_list = channels or []
    record = readiness(channels=frozenset(channel_list))
    update_value = updates(
        channel_list,
        selected={
            destination: result
            for destination, result in (selected or {}).items()
            if result != "FAILED"
        },
        github_result=github_result,
        manifest=manifest,
    )
    for destination, result in (selected or {}).items():
        if result == "FAILED":
            update_value["observations"][destination] = {
                "conclusion": "failure",
                "result": None,
            }
    snapshot, _ = merge_snapshot(
        readiness=record,
        updates=update_value,
        expected_readiness_sha256=record["readiness_sha256"],
    )
    return snapshot


class IssueTrustTests(unittest.TestCase):
    def test_exact_numeric_owner_is_required_regardless_of_association_claim(self) -> None:
        wrong = issue()
        wrong["user"]["id"] = OWNER_ID + 1
        wrong["author_association"] = "OWNER"
        with self.assertRaises(LedgerError):
            readiness(issue_value=wrong)

        collaborator = issue()
        collaborator["user"]["id"] = OWNER_ID + 2
        collaborator["author_association"] = "COLLABORATOR"
        with self.assertRaises(LedgerError):
            readiness(issue_value=collaborator)

        # Association is bound but never grants trust; only the configured ID does.
        owner_with_changed_association = issue()
        owner_with_changed_association["author_association"] = "COLLABORATOR"
        self.assertEqual(readiness(issue_value=owner_with_changed_association)["issue"]["id"], 9001)

    def test_snapshot_writer_must_be_a_distinct_github_bot(self) -> None:
        for configured_bot in (
            BotIdentity(BOT.user_id, BOT.login, "User", BOT.author_association),
            BotIdentity(OWNER_ID, BOT.login, BOT.user_type, BOT.author_association),
            BotIdentity(41898282, "github-actions[bot]", "Bot", "NONE"),
            BotIdentity(BOT.user_id, "ordinary-bot-login", "Bot", "NONE"),
        ):
            with self.subTest(bot=configured_bot), self.assertRaises(LedgerError):
                build_readiness(
                    issue=issue(),
                    comments=[[]],
                    identity=IDENTITY,
                    issue_number=17,
                    owner_id=OWNER_ID,
                    bot=configured_bot,
                    channels=frozenset(),
                    mode="publish",
                    retry_authorized=False,
                    anchor_exists=False,
                    workflow_run_id="123",
                    workflow_run_attempt="1",
                )

    def test_title_body_label_state_and_pull_request_mutations_fail_closed(self) -> None:
        mutations = {
            "title": ("title", "[release] v0.2.0 READY"),
            "body": ("body", expected_issue_body("v0.2.0") + "extra\n"),
            "state": ("state", "closed"),
            "unlocked": ("locked", False),
            "pull request": ("pull_request", {"url": "https://example.invalid"}),
        }
        for name, (field, value) in mutations.items():
            current = issue()
            current[field] = value
            with self.subTest(name=name), self.assertRaises(LedgerError):
                readiness(issue_value=current)

        missing_lock = issue()
        del missing_lock["locked"]
        with self.assertRaises(LedgerError):
            readiness(issue_value=missing_lock)

        for labels in ([], [{"id": 1, "node_id": "x", "name": "other"}], issue()["labels"] * 2):
            current = issue()
            current["labels"] = labels
            with self.subTest(labels=labels), self.assertRaises(LedgerError):
                readiness(issue_value=current)

    def test_paginated_comment_count_and_page_shapes_are_exact(self) -> None:
        note = human_comment()
        record = readiness(issue_value=issue(comments=1), pages=[[note], []])
        self.assertEqual(record["comment_count"], 1)
        with self.assertRaises(LedgerError):
            readiness(issue_value=issue(comments=2), pages=[[note]])
        with self.assertRaises(LedgerError):
            build_readiness(
                issue=issue(), comments=[note], identity=IDENTITY, issue_number=17,
                owner_id=OWNER_ID, bot=BOT, channels=frozenset(), mode="publish",
                retry_authorized=False, anchor_exists=False, workflow_run_id="123",
                workflow_run_attempt="1",
            )


class BotCommentTests(unittest.TestCase):
    def snapshot_comment(self) -> tuple[dict[str, object], dict[str, object]]:
        snapshot = first_snapshot()
        comment = bot_comment(snapshot)
        return snapshot, comment

    def test_exact_unedited_bot_identity_is_the_only_snapshot_author(self) -> None:
        _, valid = self.snapshot_comment()
        readiness(issue_value=issue(comments=1), pages=[[valid]], anchor=True, retry=True)
        for field, value in (
            ("id", BOT.user_id + 1),
            ("login", "github-action[bot]"),
            ("type", "User"),
        ):
            spoof = deepcopy(valid)
            spoof["user"][field] = value
            with self.subTest(field=field), self.assertRaises(LedgerError):
                readiness(issue_value=issue(comments=1), pages=[[spoof]], anchor=True, retry=True)
        spoof = deepcopy(valid)
        spoof["author_association"] = "OWNER"
        with self.assertRaises(LedgerError):
            readiness(issue_value=issue(comments=1), pages=[[spoof]], anchor=True, retry=True)

    def test_snapshot_spoof_edit_and_noncanonical_body_are_rejected(self) -> None:
        _, valid = self.snapshot_comment()
        edited = deepcopy(valid)
        edited["updated_at"] = "2026-07-19T01:02:00Z"
        human_spoof = human_comment(body=valid["body"])
        noncanonical = deepcopy(valid)
        noncanonical["body"] += "\n"
        bot_without_snapshot = deepcopy(valid)
        bot_without_snapshot["body"] = "ordinary bot text"
        for name, comment in (
            ("edited", edited),
            ("human spoof", human_spoof),
            ("noncanonical", noncanonical),
            ("bot without snapshot", bot_without_snapshot),
        ):
            with self.subTest(name=name), self.assertRaises(LedgerError):
                readiness(issue_value=issue(comments=1), pages=[[comment]], anchor=True, retry=True)

    def test_comment_ids_timestamps_nodes_and_issue_urls_are_bound_and_monotonic(self) -> None:
        first = human_comment(comment_id=10000)
        second = human_comment(comment_id=10001, created_at="2026-07-19T01:00:40Z")
        readiness(issue_value=issue(comments=2), pages=[[first], [second]])
        mutations = []
        wrong_id = deepcopy(second)
        wrong_id["id"] = 9999
        mutations.append(wrong_id)
        wrong_time = deepcopy(second)
        wrong_time["created_at"] = "2026-07-19T00:59:00Z"
        wrong_time["updated_at"] = wrong_time["created_at"]
        mutations.append(wrong_time)
        wrong_node = deepcopy(second)
        wrong_node["node_id"] = first["node_id"]
        mutations.append(wrong_node)
        wrong_issue = deepcopy(second)
        wrong_issue["issue_url"] = ISSUE_URL + "0"
        mutations.append(wrong_issue)
        for comment in mutations:
            with self.subTest(comment=comment), self.assertRaises(LedgerError):
                readiness(issue_value=issue(comments=2), pages=[[first, comment]])


class ReadinessDriftTests(unittest.TestCase):
    def setUp(self) -> None:
        self.note = human_comment()
        self.issue = issue(comments=1)
        self.pages = [[self.note]]
        self.record = readiness(issue_value=self.issue, pages=self.pages)

    def recheck(self, issue_value: object, pages: object, *, token: str | None = None):
        return recheck_readiness(
            readiness=self.record,
            expected_readiness_sha256=token or self.record["readiness_sha256"],
            issue=issue_value,
            comments=pages,
            issue_number=17,
            owner_id=OWNER_ID,
            bot=BOT,
            workflow_run_id="123",
            workflow_run_attempt="1",
        )

    def test_unchanged_issue_and_paginated_comments_recheck(self) -> None:
        self.assertEqual(self.recheck(self.issue, self.pages)["readiness_sha256"], self.record["readiness_sha256"])

    def test_every_bound_issue_identity_field_detects_concurrent_drift(self) -> None:
        mutations = (
            ("id", 9002),
            ("node_id", "I_other"),
            ("title", "[release] v0.2.0 READY"),
            ("body", expected_issue_body("v0.2.0") + "changed\n"),
            ("locked", False),
            ("updated_at", "2026-07-19T01:03:00Z"),
            ("author_association", "MEMBER"),
        )
        for field, value in mutations:
            changed = deepcopy(self.issue)
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(LedgerError):
                self.recheck(changed, self.pages)
        changed_user = deepcopy(self.issue)
        changed_user["user"]["login"] = "renamed-owner"
        with self.assertRaises(LedgerError):
            self.recheck(changed_user, self.pages)
        changed_label = deepcopy(self.issue)
        changed_label["labels"][0]["node_id"] = "LA_changed"
        with self.assertRaises(LedgerError):
            self.recheck(changed_label, self.pages)

    def test_comment_add_edit_delete_and_reorder_detect_concurrent_drift(self) -> None:
        added = human_comment(comment_id=10002, created_at="2026-07-19T01:02:00Z")
        added_issue = deepcopy(self.issue)
        added_issue["comments"] = 2
        added_issue["updated_at"] = "2026-07-19T01:02:00Z"
        with self.assertRaises(LedgerError):
            self.recheck(added_issue, [[self.note, added]])

        edited = deepcopy(self.note)
        edited["body"] = "changed"
        with self.assertRaises(LedgerError):
            self.recheck(self.issue, [[edited]])

        deleted_issue = deepcopy(self.issue)
        deleted_issue["comments"] = 0
        deleted_issue["updated_at"] = "2026-07-19T01:02:00Z"
        with self.assertRaises(LedgerError):
            self.recheck(deleted_issue, [[]])

        second = human_comment(comment_id=10002, created_at="2026-07-19T01:02:00Z")
        two_issue = issue(comments=2, updated_at="2026-07-19T01:02:00Z")
        two_record = readiness(issue_value=two_issue, pages=[[self.note, second]])
        with self.assertRaises(LedgerError):
            recheck_readiness(
                readiness=two_record,
                expected_readiness_sha256=two_record["readiness_sha256"],
                issue=two_issue,
                comments=[[second, self.note]],
                issue_number=17,
                owner_id=OWNER_ID,
                bot=BOT,
                workflow_run_id="123",
                workflow_run_attempt="1",
            )

    def test_wrong_token_run_owner_bot_and_issue_number_are_rejected(self) -> None:
        with self.assertRaises(LedgerError):
            self.recheck(self.issue, self.pages, token="f" * 64)
        common = dict(
            readiness=self.record,
            expected_readiness_sha256=self.record["readiness_sha256"],
            issue=self.issue,
            comments=self.pages,
            issue_number=17,
            owner_id=OWNER_ID,
            bot=BOT,
            workflow_run_id="123",
            workflow_run_attempt="1",
        )
        for field, value in (
            ("workflow_run_id", "124"),
            ("owner_id", OWNER_ID + 1),
            ("issue_number", 18),
            ("bot", BotIdentity(BOT.user_id + 1, BOT.login, BOT.user_type, BOT.author_association)),
        ):
            changed = {**common, field: value}
            with self.subTest(field=field), self.assertRaises(LedgerError):
                recheck_readiness(**changed)


class ReplayPolicyTests(unittest.TestCase):
    def readiness_with_result(
        self,
        destination: str,
        channel: str,
        result: str,
        *,
        retry: bool,
        anchor: bool = True,
        mode: str = "publish",
    ) -> dict[str, object]:
        snapshot = first_snapshot(channels=[channel], selected={destination: result})
        comment = bot_comment(snapshot)
        return readiness(
            issue_value=issue(comments=1),
            pages=[[comment]],
            channels=frozenset({channel}),
            retry=retry,
            anchor=anchor,
            mode=mode,
            run_id="124",
        )

    def test_source_retry_accepts_only_failed_manual_block_and_first_use(self) -> None:
        for state in ("FAILED", "BLOCKED_MANUAL_ACTION", "FIRST_USE_UNPROVEN"):
            with self.subTest(state=state):
                self.assertIsNotNone(
                    self.readiness_with_result("homebrew", "homebrew", state, retry=True)
                )
        for state in ("NOT_SELECTED", "PUBLISHED", "SKIPPED_ALREADY_IDENTICAL"):
            if state == "NOT_SELECTED":
                snapshot = first_snapshot()
                comment = bot_comment(snapshot)
                with self.subTest(state=state), self.assertRaises(LedgerError):
                    readiness(
                        issue_value=issue(comments=1), pages=[[comment]],
                        channels=frozenset({"homebrew"}), retry=True, anchor=True, run_id="124",
                    )
            else:
                with self.subTest(state=state), self.assertRaises(LedgerError):
                    self.readiness_with_result("homebrew", "homebrew", state, retry=True)

    def test_handed_off_review_and_moderation_states_are_never_source_retryable(self) -> None:
        cases = (
            ("homebrew", "homebrew", "DISPATCHED_PENDING_REVIEW"),
            ("homebrew", "homebrew", "SUBMITTED_PENDING_REVIEW"),
            ("chocolatey", "chocolatey", "DISPATCHED_PENDING_MODERATION"),
            ("chocolatey", "chocolatey", "SUBMITTED_PENDING_MODERATION"),
        )
        for destination, channel, state in cases:
            with self.subTest(state=state), self.assertRaises(LedgerError):
                self.readiness_with_result(destination, channel, state, retry=True)

    def test_retry_requires_anchor_and_normal_continuation_requires_not_selected(self) -> None:
        with self.assertRaises(LedgerError):
            self.readiness_with_result("homebrew", "homebrew", "FAILED", retry=True, anchor=False)
        with self.assertRaises(LedgerError):
            self.readiness_with_result("homebrew", "homebrew", "FAILED", retry=False)
        snapshot = first_snapshot()
        comment = bot_comment(snapshot)
        self.assertIsNotNone(
            readiness(
                issue_value=issue(comments=1), pages=[[comment]],
                channels=frozenset({"homebrew"}), anchor=True, run_id="124",
            )
        )

    def test_validate_mode_checks_history_but_does_not_authorize_replay(self) -> None:
        self.assertIsNotNone(
            self.readiness_with_result(
                "homebrew", "homebrew", "PUBLISHED", retry=False, mode="validate"
            )
        )


class MergeAndChainTests(unittest.TestCase):
    def test_merge_requires_the_exact_trusted_readiness_token(self) -> None:
        record = readiness(channels=frozenset({"apt"}))
        with self.assertRaises(LedgerError):
            merge_snapshot(
                readiness=record,
                updates=updates(["apt"], selected={"deb_apt": "PUBLISHED"}),
                expected_readiness_sha256="f" * 64,
            )

    def test_first_and_later_batches_preserve_all_cumulative_results(self) -> None:
        first_record = readiness(channels=frozenset({"apt"}))
        first, _ = merge_snapshot(
            readiness=first_record,
            updates=updates(["apt"], selected={"deb_apt": "PUBLISHED"}),
            expected_readiness_sha256=first_record["readiness_sha256"],
        )
        self.assertEqual(first["sequence"], 1)
        self.assertEqual(first["ledger"]["results"]["deb_apt"], "PUBLISHED")

        comment = bot_comment(first)
        second_record = readiness(
            issue_value=issue(comments=1), pages=[[comment]],
            channels=frozenset({"homebrew"}), anchor=True, run_id="124",
        )
        second, body = merge_snapshot(
            readiness=second_record,
            updates=updates(
                ["homebrew"],
                selected={"homebrew": "DISPATCHED_PENDING_REVIEW"},
                github_result="SKIPPED_ALREADY_IDENTICAL",
            ),
            expected_readiness_sha256=second_record["readiness_sha256"],
        )
        self.assertEqual(second["sequence"], 2)
        self.assertEqual(second["previous_snapshot_sha256"], snapshot_sha256(first))
        self.assertEqual(second["ledger"]["results"]["deb_apt"], "PUBLISHED")
        self.assertEqual(second["ledger"]["results"]["homebrew"], "DISPATCHED_PENDING_REVIEW")
        self.assertEqual(render_snapshot_comment(second), body)

    def test_failed_selected_job_never_erases_durable_prior_state(self) -> None:
        first = first_snapshot(channels=["homebrew"], selected={"homebrew": "PUBLISHED"})
        record = readiness(
            issue_value=issue(comments=1), pages=[[bot_comment(first)]],
            channels=frozenset({"homebrew"}), anchor=True, retry=False,
            mode="validate", run_id="124",
        )
        failed = updates(["homebrew"], github_result="SKIPPED_ALREADY_IDENTICAL")
        failed["observations"]["homebrew"] = {"conclusion": "failure", "result": None}
        second, _ = merge_snapshot(
            readiness=record,
            updates=failed,
            expected_readiness_sha256=record["readiness_sha256"],
        )
        self.assertEqual(second["ledger"]["results"]["homebrew"], "PUBLISHED")

    def test_unselected_destination_mutation_and_manifest_conflict_fail(self) -> None:
        record = readiness(channels=frozenset({"apt"}))
        wrong_channels = updates(["apt"], selected={"deb_apt": "PUBLISHED"})
        wrong_channels["selected_channels"] = ["apt", "rpm"]
        with self.assertRaises(LedgerError):
            merge_snapshot(
                readiness=record,
                updates=wrong_channels,
                expected_readiness_sha256=record["readiness_sha256"],
            )

        wrong = updates(["apt"], selected={"deb_apt": "PUBLISHED"})
        wrong["observations"]["rpm"] = {"conclusion": "success", "result": "PUBLISHED"}
        with self.assertRaises(LedgerError):
            merge_snapshot(
                readiness=record,
                updates=wrong,
                expected_readiness_sha256=record["readiness_sha256"],
            )

        first = first_snapshot()
        later = readiness(
            issue_value=issue(comments=1), pages=[[bot_comment(first)]],
            channels=frozenset(), anchor=True, retry=False, mode="validate", run_id="124",
        )
        with self.assertRaises(LedgerError):
            merge_snapshot(
                readiness=later,
                updates=updates([], manifest="3" * 64, github_result="SKIPPED_ALREADY_IDENTICAL"),
                expected_readiness_sha256=later["readiness_sha256"],
            )

    def test_broken_middle_chain_duplicate_run_and_tail_deletion_are_detected(self) -> None:
        first = first_snapshot()
        first_comment = bot_comment(first)
        second_record = readiness(
            issue_value=issue(comments=1), pages=[[first_comment]],
            mode="validate", anchor=True, run_id="124",
        )
        second, _ = merge_snapshot(
            readiness=second_record,
            updates=updates([], github_result="SKIPPED_ALREADY_IDENTICAL"),
            expected_readiness_sha256=second_record["readiness_sha256"],
        )
        second_comment = bot_comment(
            second, comment_id=10002, created_at="2026-07-19T01:02:00Z"
        )
        full_issue = issue(comments=2, updated_at="2026-07-19T01:02:00Z")
        full_record = readiness(
            issue_value=full_issue, pages=[[first_comment], [second_comment]],
            mode="validate", anchor=True, run_id="125",
        )
        with self.assertRaises(LedgerError):
            readiness(
                issue_value=issue(comments=1), pages=[[second_comment]],
                mode="validate", anchor=True, run_id="125",
            )
        with self.assertRaises(LedgerError):
            recheck_readiness(
                readiness=full_record,
                expected_readiness_sha256=full_record["readiness_sha256"],
                issue=issue(comments=1, updated_at="2026-07-19T01:03:00Z"),
                comments=[[first_comment]],
                issue_number=17,
                owner_id=OWNER_ID,
                bot=BOT,
                workflow_run_id="125",
                workflow_run_attempt="1",
            )
        duplicate_run = deepcopy(second)
        duplicate_run["workflow_run_id"] = first["workflow_run_id"]
        duplicate_run["workflow_run_attempt"] = first["workflow_run_attempt"]
        duplicate_comment = bot_comment(
            duplicate_run, comment_id=10002, created_at="2026-07-19T01:02:00Z"
        )
        with self.assertRaises(LedgerError):
            readiness(
                issue_value=full_issue, pages=[[first_comment, duplicate_comment]],
                mode="validate", anchor=True, run_id="125",
            )


class AppendVerificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.before_issue = issue()
        self.before_comments: list[list[dict[str, object]]] = [[]]
        self.record = readiness(
            issue_value=self.before_issue,
            pages=self.before_comments,
            channels=frozenset({"apt"}),
        )
        self.snapshot, _ = merge_snapshot(
            readiness=self.record,
            updates=updates(["apt"], selected={"deb_apt": "PUBLISHED"}),
            expected_readiness_sha256=self.record["readiness_sha256"],
        )
        self.appended_comment = bot_comment(self.snapshot)
        self.after_issue = issue(
            comments=1,
            updated_at=self.appended_comment["created_at"],
        )
        self.after_comments = [[self.appended_comment]]

    def verify(self, **overrides: object) -> dict[str, object]:
        arguments = {
            "readiness": self.record,
            "expected_readiness_sha256": self.record["readiness_sha256"],
            "snapshot": self.snapshot,
            "before_issue": self.before_issue,
            "before_comments": self.before_comments,
            "after_issue": self.after_issue,
            "after_comments": self.after_comments,
            "issue_number": 17,
            "owner_id": OWNER_ID,
            "bot": BOT,
            "workflow_run_id": "123",
            "workflow_run_attempt": "1",
        }
        arguments.update(overrides)
        return verify_append(**arguments)

    def test_exact_single_canonical_app_bot_append_is_accepted(self) -> None:
        receipt = self.verify()
        self.assertEqual(receipt["comment_id"], self.appended_comment["id"])
        self.assertEqual(receipt["comment_node_id"], self.appended_comment["node_id"])
        self.assertEqual(receipt["snapshot_sha256"], snapshot_sha256(self.snapshot))

    def test_zero_multiple_human_spoof_and_edited_appends_fail_closed(self) -> None:
        second = human_comment(
            comment_id=10002,
            created_at="2026-07-19T01:02:00Z",
        )
        human_spoof = human_comment(
            comment_id=10001,
            body=render_snapshot_comment(self.snapshot),
            created_at="2026-07-19T01:01:00Z",
        )
        edited = deepcopy(self.appended_comment)
        edited["updated_at"] = "2026-07-19T01:02:00Z"
        wrong_body = deepcopy(self.appended_comment)
        wrong_body["body"] += "\n"
        wrong_bot = deepcopy(self.appended_comment)
        wrong_bot["user"]["id"] = BOT.user_id + 1
        cases = (
            ("zero", issue(), [[]]),
            (
                "multiple",
                issue(comments=2, updated_at="2026-07-19T01:02:00Z"),
                [[self.appended_comment, second]],
            ),
            ("human spoof", self.after_issue, [[human_spoof]]),
            ("edited", self.after_issue, [[edited]]),
            ("wrong body", self.after_issue, [[wrong_body]]),
            ("wrong bot", self.after_issue, [[wrong_bot]]),
        )
        for name, after_issue_value, after_comment_pages in cases:
            with self.subTest(name=name), self.assertRaises(LedgerError):
                self.verify(
                    after_issue=after_issue_value,
                    after_comments=after_comment_pages,
                )

    def test_prestate_prefix_issue_token_and_snapshot_mutations_fail_closed(self) -> None:
        wrong_token = "f" * 64
        changed_before = deepcopy(self.before_issue)
        changed_before["updated_at"] = "2026-07-19T01:00:01Z"
        changed_after_issue = deepcopy(self.after_issue)
        changed_after_issue["body"] += "changed\n"
        unlocked_after_issue = deepcopy(self.after_issue)
        unlocked_after_issue["locked"] = False
        changed_snapshot = deepcopy(self.snapshot)
        changed_snapshot["readiness_sha256"] = "e" * 64
        changed_channels = deepcopy(self.snapshot)
        changed_channels["selected_channels"] = ["apt", "rpm"]
        cases = (
            ("wrong token", {"expected_readiness_sha256": wrong_token}),
            ("changed prestate", {"before_issue": changed_before}),
            ("changed issue", {"after_issue": changed_after_issue}),
            ("unlocked issue", {"after_issue": unlocked_after_issue}),
            ("wrong snapshot", {"snapshot": changed_snapshot}),
            ("wrong channels", {"snapshot": changed_channels}),
        )
        for name, overrides in cases:
            with self.subTest(name=name), self.assertRaises(LedgerError):
                self.verify(**overrides)

        note = human_comment()
        before_issue_value = issue(comments=1)
        before_pages = [[note]]
        record = readiness(
            issue_value=before_issue_value,
            pages=before_pages,
            channels=frozenset({"apt"}),
        )
        snapshot, _ = merge_snapshot(
            readiness=record,
            updates=updates(["apt"], selected={"deb_apt": "PUBLISHED"}),
            expected_readiness_sha256=record["readiness_sha256"],
        )
        changed_note = deepcopy(note)
        changed_note["body"] = "mutated concurrent prefix"
        new_comment = bot_comment(
            snapshot,
            comment_id=10001,
            created_at="2026-07-19T01:01:00Z",
        )
        with self.assertRaises(LedgerError):
            verify_append(
                readiness=record,
                expected_readiness_sha256=record["readiness_sha256"],
                snapshot=snapshot,
                before_issue=before_issue_value,
                before_comments=before_pages,
                after_issue=issue(comments=2, updated_at="2026-07-19T01:01:00Z"),
                after_comments=[[changed_note, new_comment]],
                issue_number=17,
                owner_id=OWNER_ID,
                bot=BOT,
                workflow_run_id="123",
                workflow_run_attempt="1",
            )


class LedgerCliTests(unittest.TestCase):
    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["python3", "-I", "-S", str(SCRIPT), *arguments],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_minimal_readiness_recheck_and_render_merge_invocations(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            issue_path = root / "issue.json"
            comments_path = root / "comments.json"
            readiness_path = root / "readiness.json"
            updates_path = root / "updates.json"
            snapshot_path = root / "snapshot.json"
            comment_path = root / "comment.md"
            after_issue_path = root / "after-issue.json"
            after_comments_path = root / "after-comments.json"
            outputs_path = root / "outputs.txt"
            issue_path.write_text(json.dumps(issue()), encoding="utf-8")
            comments_path.write_text("[[]]\n", encoding="utf-8")
            common = (
                "--issue-number", "17", "--owner-id", str(OWNER_ID),
                "--bot-id", str(BOT.user_id), "--bot-login", BOT.login,
                "--bot-type", BOT.user_type, "--bot-association", BOT.author_association,
                "--workflow-run-id", "123", "--workflow-run-attempt", "1",
            )
            prepared = self.run_cli(
                "readiness", "--issue", str(issue_path), "--comments", str(comments_path),
                "--tag", IDENTITY.tag, "--version", IDENTITY.version,
                "--commit", IDENTITY.commit, "--channels", "apt", "--mode", "publish",
                "--retry-authorized", "false", "--anchor-exists", "false",
                "--output", str(readiness_path), "--github-output", str(outputs_path), *common,
            )
            self.assertEqual(prepared.returncode, 0, prepared.stderr)
            record = json.loads(readiness_path.read_text(encoding="utf-8"))

            checked = self.run_cli(
                "recheck", "--readiness", str(readiness_path),
                "--expected-readiness-sha256", record["readiness_sha256"],
                "--issue", str(issue_path), "--comments", str(comments_path),
                "--github-output", str(outputs_path), *common,
            )
            self.assertEqual(checked.returncode, 0, checked.stderr)

            updates_path.write_text(
                json.dumps(updates(["apt"], selected={"deb_apt": "PUBLISHED"})),
                encoding="utf-8",
            )
            rendered = self.run_cli(
                "render-merge", "--readiness", str(readiness_path),
                "--expected-readiness-sha256", record["readiness_sha256"],
                "--updates", str(updates_path), "--snapshot-output", str(snapshot_path),
                "--comment-output", str(comment_path), "--github-output", str(outputs_path),
            )
            self.assertEqual(rendered.returncode, 0, rendered.stderr)
            self.assertIn("release-ledger-snapshot", comment_path.read_text())
            self.assertEqual(json.loads(snapshot_path.read_text())["sequence"], 1)
            self.assertIn("ledger_recheck=VALID", outputs_path.read_text())

            snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
            appended = bot_comment(snapshot)
            after_issue_path.write_text(
                json.dumps(issue(comments=1, updated_at=appended["created_at"])),
                encoding="utf-8",
            )
            after_comments_path.write_text(json.dumps([[appended]]), encoding="utf-8")
            verified = self.run_cli(
                "verify-append",
                "--readiness", str(readiness_path),
                "--expected-readiness-sha256", record["readiness_sha256"],
                "--snapshot", str(snapshot_path),
                "--before-issue", str(issue_path),
                "--before-comments", str(comments_path),
                "--after-issue", str(after_issue_path),
                "--after-comments", str(after_comments_path),
                "--github-output", str(outputs_path),
                *common,
            )
            self.assertEqual(verified.returncode, 0, verified.stderr)
            output_text = outputs_path.read_text(encoding="utf-8")
            self.assertIn("ledger_append=VALID", output_text)
            self.assertIn(f"ledger_comment_id={appended['id']}", output_text)

    def test_invalid_release_tag_is_a_clean_cli_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            issue_path = root / "issue.json"
            comments_path = root / "comments.json"
            issue_path.write_text(json.dumps(issue()), encoding="utf-8")
            comments_path.write_text("[[]]\n", encoding="utf-8")
            result = self.run_cli(
                "readiness",
                "--issue", str(issue_path),
                "--comments", str(comments_path),
                "--tag", "invalid",
                "--version", IDENTITY.version,
                "--commit", IDENTITY.commit,
                "--channels", "",
                "--mode", "publish",
                "--retry-authorized", "false",
                "--anchor-exists", "false",
                "--output", str(root / "readiness.json"),
                "--issue-number", "17",
                "--owner-id", str(OWNER_ID),
                "--bot-id", str(BOT.user_id),
                "--bot-login", BOT.login,
                "--bot-type", BOT.user_type,
                "--bot-association", BOT.author_association,
                "--workflow-run-id", "123",
                "--workflow-run-attempt", "1",
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("release-ledger: release tag is invalid", result.stderr)
            self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
