from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
from uuid import UUID

from publisher.submission_claim import (
    ClaimError,
    PREPARING,
    PUBLISH_JOB,
    PUSH_STEP,
    SUBMITTED,
    PublisherRun,
    SubmissionClaim,
    SubmissionIdentity,
    inspect_issue_pages,
    issue_title,
    parse_body,
    reconcile_job_pages,
    render_body,
    validate_issue,
    write_issue_request,
)


AUTOMATION_ID = 41898282
AUTOMATION_LOGIN = "github-actions[bot]"


def identity() -> SubmissionIdentity:
    return SubmissionIdentity.from_mapping(
        {
            "source_tag": "v0.2.0",
            "source_commit_sha": "1" * 40,
            "release_manifest_sha256": "2" * 64,
            "package_name": "mcp-cpp-sdk.0.2.0.nupkg",
            "package_sha256": "3" * 64,
            "request_uuid": str(UUID(int=1)),
        }
    )


def publisher_run(run_id: str = "17", attempt: str = "1") -> PublisherRun:
    return PublisherRun(run_id, attempt, "4" * 40)


def issue(claim: SubmissionClaim, number: int = 7) -> dict[str, object]:
    return {
        "id": 100 + number,
        "number": number,
        "state": "open",
        "locked": False,
        "title": issue_title(claim.identity),
        "body": render_body(claim),
        "labels": [{"name": "chocolatey-submission"}],
        "user": {
            "id": AUTOMATION_ID,
            "login": AUTOMATION_LOGIN,
            "type": "Bot",
        },
    }


def job_pages(
    *,
    conclusion: str,
    steps: object,
    run: PublisherRun | None = None,
    name: str = PUBLISH_JOB,
) -> list[dict[str, object]]:
    run = run or publisher_run()
    return [
        {
            "total_count": 1,
            "jobs": [
                {
                    "id": 29,
                    "name": name,
                    "run_id": int(run.run_id),
                    "head_sha": run.workflow_sha,
                    "status": "completed",
                    "conclusion": conclusion,
                    "steps": steps,
                    "runner_id": None if conclusion == "skipped" else 4,
                }
            ],
        }
    ]


def run_evidence(
    run: PublisherRun | None = None, *, conclusion: str = "success"
) -> dict[str, object]:
    run = run or publisher_run()
    return {
        "id": int(run.run_id),
        "run_attempt": int(run.run_attempt),
        "head_sha": run.workflow_sha,
        "event": "workflow_dispatch",
        "head_branch": "main",
        "path": ".github/workflows/publish.yml",
        "status": "completed",
        "conclusion": conclusion,
    }


class ClaimSchemaTests(unittest.TestCase):
    def test_body_and_request_are_canonical_and_round_trip(self) -> None:
        claim = SubmissionClaim(PREPARING, identity(), publisher_run())
        self.assertEqual(parse_body(render_body(claim)), claim)
        with tempfile.TemporaryDirectory() as directory:
            request = Path(directory) / "request.json"
            write_issue_request(request, claim, create=True)
            value = json.loads(request.read_text())
            self.assertEqual(
                set(value), {"title", "body", "labels"}
            )
            self.assertEqual(value["labels"], ["chocolatey-submission"])
            self.assertEqual(value["body"], render_body(claim))

    def test_claim_schema_rejects_extra_missing_or_noncanonical_fields(self) -> None:
        claim = SubmissionClaim(PREPARING, identity(), publisher_run())
        encoded = claim.as_mapping()
        mutations = []
        extra = deepcopy(encoded)
        extra["extra"] = True
        mutations.append(extra)
        missing = deepcopy(encoded)
        del missing["publisher_run"]
        mutations.append(missing)
        bad_package = deepcopy(encoded)
        bad_package["identity"]["package_name"] = "other.nupkg"
        mutations.append(bad_package)
        bad_attempt = deepcopy(encoded)
        bad_attempt["publisher_run"]["run_attempt"] = "01"
        mutations.append(bad_attempt)
        for value in mutations:
            body = render_body(claim)
            canonical = json.dumps(value, sort_keys=True, separators=(",", ":"))
            body = body.split("<!-- chocolatey-submission-json\n", 1)[0] + (
                "<!-- chocolatey-submission-json\n"
                + canonical
                + "\nchocolatey-submission-json -->\n"
            )
            with self.subTest(value=value), self.assertRaises(ClaimError):
                parse_body(body)


class IssueValidationTests(unittest.TestCase):
    def test_exact_automation_issue_is_accepted(self) -> None:
        claim = SubmissionClaim(PREPARING, identity(), publisher_run())
        self.assertEqual(
            validate_issue(
                issue(claim),
                identity=claim.identity,
                automation_user_id=AUTOMATION_ID,
                automation_login=AUTOMATION_LOGIN,
                expected_state=PREPARING,
                expected_run=claim.publisher_run,
            ),
            claim,
        )

    def test_spoofed_marker_and_every_mutable_issue_boundary_fail_closed(self) -> None:
        claim = SubmissionClaim(PREPARING, identity(), publisher_run())
        mutations = {
            "user-id": ("user", "id", AUTOMATION_ID + 1),
            "user-login": ("user", "login", "attacker[bot]"),
            "user-type": ("user", "type", "User"),
            "label": ("labels", 0, {"name": "other"}),
            "title": ("title", None, "lookalike"),
            "state": ("state", None, "closed"),
            "locked": ("locked", None, True),
            "body": ("body", None, render_body(claim) + "edited\n"),
        }
        for label, (field, child, replacement) in mutations.items():
            value = issue(claim)
            if child is None:
                value[field] = replacement
            else:
                value[field][child] = replacement
            with self.subTest(label=label), self.assertRaises(ClaimError):
                validate_issue(
                    value,
                    identity=claim.identity,
                    automation_user_id=AUTOMATION_ID,
                    automation_login=AUTOMATION_LOGIN,
                )

        spoof = issue(claim)
        spoof["user"] = {"id": 99, "login": "attacker", "type": "User"}
        with self.assertRaises(ClaimError):
            inspect_issue_pages(
                [[spoof]],
                identity=claim.identity,
                current_run=publisher_run("18", "1"),
                automation_user_id=AUTOMATION_ID,
                automation_login=AUTOMATION_LOGIN,
            )

    def test_inspection_creates_submits_or_requests_reconciliation(self) -> None:
        current = publisher_run("18", "1")
        self.assertEqual(
            inspect_issue_pages(
                [[]],
                identity=identity(),
                current_run=current,
                automation_user_id=AUTOMATION_ID,
                automation_login=AUTOMATION_LOGIN,
            ).action,
            "create",
        )
        for state, expected in ((PREPARING, "reconcile"), (SUBMITTED, "submitted")):
            claim = SubmissionClaim(state, identity(), publisher_run())
            result = inspect_issue_pages(
                [[issue(claim)]],
                identity=claim.identity,
                current_run=current,
                automation_user_id=AUTOMATION_ID,
                automation_login=AUTOMATION_LOGIN,
            )
            self.assertEqual(result.action, expected)
            self.assertEqual(result.issue_number, 7)

    def test_duplicate_claims_or_same_attempt_preparing_are_ambiguous(self) -> None:
        claim = SubmissionClaim(PREPARING, identity(), publisher_run())
        with self.assertRaises(ClaimError):
            inspect_issue_pages(
                [[issue(claim, 7), issue(claim, 8)]],
                identity=claim.identity,
                current_run=publisher_run("18", "1"),
                automation_user_id=AUTOMATION_ID,
                automation_login=AUTOMATION_LOGIN,
            )
        with self.assertRaises(ClaimError):
            inspect_issue_pages(
                [[issue(claim)]],
                identity=claim.identity,
                current_run=claim.publisher_run,
                automation_user_id=AUTOMATION_ID,
                automation_login=AUTOMATION_LOGIN,
            )


class ReconciliationTests(unittest.TestCase):
    def test_prior_exact_push_success_becomes_submitted(self) -> None:
        steps = [
            {
                "name": PUSH_STEP,
                "status": "completed",
                "conclusion": "success",
            }
        ]
        self.assertEqual(
            reconcile_job_pages(
                job_pages(conclusion="success", steps=steps),
                run_value=run_evidence(),
                prior_run=publisher_run(),
            ),
            "submitted",
        )

    def test_prior_definitely_skipped_job_can_resume(self) -> None:
        self.assertEqual(
            reconcile_job_pages(
                job_pages(conclusion="skipped", steps=[]),
                run_value=run_evidence(),
                prior_run=publisher_run(),
            ),
            "resume",
        )

    def test_failure_cancelled_in_progress_or_ambiguous_evidence_requires_manual(self) -> None:
        cases = [
            job_pages(conclusion="failure", steps=[]),
            job_pages(conclusion="cancelled", steps=[]),
            job_pages(conclusion="success", steps=[]),
            job_pages(conclusion="skipped", steps=[{"name": PUSH_STEP}]),
            job_pages(conclusion="success", steps=[], name="different job"),
            [],
        ]
        in_progress = job_pages(conclusion="failure", steps=[])
        in_progress[0]["jobs"][0]["status"] = "in_progress"
        in_progress[0]["jobs"][0]["conclusion"] = None
        cases.append(in_progress)
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ClaimError):
                reconcile_job_pages(
                    value,
                    run_value=run_evidence(),
                    prior_run=publisher_run(),
                )

        for field, replacement in (
            ("run_attempt", 9),
            ("head_sha", "9" * 40),
            ("path", ".github/workflows/other.yml"),
            ("status", "in_progress"),
        ):
            evidence = run_evidence()
            evidence[field] = replacement
            with self.subTest(field=field), self.assertRaises(ClaimError):
                reconcile_job_pages(
                    job_pages(conclusion="skipped", steps=[]),
                    run_value=evidence,
                    prior_run=publisher_run(),
                )


if __name__ == "__main__":
    unittest.main()
