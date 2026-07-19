from __future__ import annotations

from copy import deepcopy
import json
from typing import Mapping
import unittest
from unittest.mock import patch
from uuid import UUID

from publisher import claim_controller as controller
from publisher.submission_claim import (
    ClaimError,
    PREPARING,
    PUBLISH_JOB,
    PUSH_STEP,
    SUBMITTED,
    PublisherRun,
    SubmissionClaim,
    SubmissionIdentity,
    issue_title,
    parse_body,
    render_body,
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


def context(current_run: PublisherRun | None = None) -> controller.ClaimContext:
    return controller.ClaimContext(
        repository="owner/repository",
        identity=identity(),
        current_run=current_run or publisher_run("18", "2"),
        automation_user_id=AUTOMATION_ID,
        automation_login=AUTOMATION_LOGIN,
    )


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


def run_evidence(run: PublisherRun) -> dict[str, object]:
    return {
        "id": int(run.run_id),
        "run_attempt": int(run.run_attempt),
        "head_sha": run.workflow_sha,
        "event": "workflow_dispatch",
        "head_branch": "main",
        "path": ".github/workflows/publish.yml",
        "status": "completed",
        "conclusion": "success",
    }


def job_pages(run: PublisherRun, conclusion: str) -> list[dict[str, object]]:
    steps: list[dict[str, object]] = []
    if conclusion == "success":
        steps.append(
            {"name": PUSH_STEP, "status": "completed", "conclusion": "success"}
        )
    return [
        {
            "total_count": 1,
            "jobs": [
                {
                    "id": 29,
                    "name": PUBLISH_JOB,
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


class FakeApi:
    def __init__(
        self,
        stored_issue: dict[str, object] | None,
        *,
        prior_run: PublisherRun | None = None,
        prior_conclusion: str = "skipped",
        tamper_next_get: bool = False,
    ) -> None:
        self.stored_issue = deepcopy(stored_issue)
        self.prior_run = prior_run
        self.prior_conclusion = prior_conclusion
        self.tamper_next_get = tamper_next_get
        self.events: list[str] = []

    def list_issues(self) -> list[list[object]]:
        self.events.append("list-issues")
        if self.stored_issue is None:
            return [[]]
        return [[deepcopy(self.stored_issue)]]

    def create_issue(self, request: Mapping[str, object]) -> object:
        self.events.append("create-issue")
        claim = parse_body(request["body"])
        self.stored_issue = issue(claim)
        return {"number": 7}

    def get_issue(self, number: int) -> object:
        self.events.append(f"get-issue:{number}")
        if self.stored_issue is None:
            raise AssertionError("no issue exists")
        value = deepcopy(self.stored_issue)
        if self.tamper_next_get:
            self.tamper_next_get = False
            value["body"] += "tampered\n"
        return value

    def update_issue(self, number: int, request: Mapping[str, object]) -> object:
        self.events.append(f"patch-issue:{number}")
        if self.stored_issue is None:
            raise AssertionError("no issue exists")
        self.stored_issue["body"] = request["body"]
        return deepcopy(self.stored_issue)

    def get_run_attempt(self, run: PublisherRun) -> object:
        self.events.append(f"get-run:{run.run_id}:{run.run_attempt}")
        if run != self.prior_run:
            raise AssertionError("wrong prior run requested")
        return run_evidence(run)

    def list_run_attempt_jobs(self, run: PublisherRun) -> list[object]:
        self.events.append(f"list-jobs:{run.run_id}:{run.run_attempt}")
        if run != self.prior_run:
            raise AssertionError("wrong prior run requested")
        return job_pages(run, self.prior_conclusion)


class FakeResponse:
    def __init__(self, value: object, status: int = 200) -> None:
        self.status = status
        self.payload = json.dumps(value, separators=(",", ":")).encode("utf-8")

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def read(self, size: int) -> bytes:
        return self.payload[:size]


class FakeOpener:
    def __init__(self, *responses: FakeResponse) -> None:
        self.responses = list(responses)
        self.requests: list[object] = []

    def open(self, request: object, timeout: int) -> FakeResponse:
        if timeout != 30:
            raise AssertionError("unexpected timeout")
        self.requests.append(request)
        return self.responses.pop(0)


def observe_validations(api: FakeApi):
    original = controller.validate_issue

    def observed(value: object, **kwargs: object) -> SubmissionClaim:
        state = kwargs["expected_state"]
        run = kwargs["expected_run"]
        api.events.append(f"verify:{state}:{run.run_id}:{run.run_attempt}")
        return original(value, **kwargs)

    return patch.object(controller, "validate_issue", side_effect=observed)


class ClaimControllerTests(unittest.TestCase):
    def test_create_and_exact_refetch_happen_before_success(self) -> None:
        api = FakeApi(None)
        current = context()
        with observe_validations(api):
            result = controller.manage_claim(api, current)
        self.assertEqual(result, controller.ManageResult(7, False))
        self.assertEqual(
            api.events,
            [
                "list-issues",
                "create-issue",
                "get-issue:7",
                "verify:PREPARING:18:2",
            ],
        )

    def test_retry_success_records_prior_submission_in_exact_order(self) -> None:
        prior = publisher_run()
        api = FakeApi(
            issue(SubmissionClaim(PREPARING, identity(), prior)),
            prior_run=prior,
            prior_conclusion="success",
        )
        with observe_validations(api):
            result = controller.manage_claim(api, context())
        self.assertEqual(result, controller.ManageResult(7, True))
        self.assertEqual(
            api.events,
            [
                "list-issues",
                "get-run:17:1",
                "list-jobs:17:1",
                "get-issue:7",
                "verify:PREPARING:17:1",
                "patch-issue:7",
                "get-issue:7",
                "verify:SUBMITTED:17:1",
            ],
        )

    def test_definitely_skipped_retry_transfers_claim_then_resumes(self) -> None:
        prior = publisher_run()
        api = FakeApi(
            issue(SubmissionClaim(PREPARING, identity(), prior)),
            prior_run=prior,
            prior_conclusion="skipped",
        )
        current = context()
        with observe_validations(api):
            result = controller.manage_claim(api, current)
        self.assertEqual(result, controller.ManageResult(7, False))
        self.assertEqual(
            api.events[-5:],
            [
                "get-issue:7",
                "verify:PREPARING:17:1",
                "patch-issue:7",
                "get-issue:7",
                "verify:PREPARING:18:2",
            ],
        )

    def test_record_refetches_and_verifies_immediately_before_patch(self) -> None:
        current = context()
        api = FakeApi(
            issue(SubmissionClaim(PREPARING, current.identity, current.current_run))
        )
        with observe_validations(api):
            controller.record_claim(api, current, 7)
        self.assertEqual(
            api.events,
            [
                "get-issue:7",
                "verify:PREPARING:18:2",
                "patch-issue:7",
                "get-issue:7",
                "verify:SUBMITTED:18:2",
            ],
        )

    def test_changed_claim_stops_before_any_patch(self) -> None:
        current = context()
        api = FakeApi(
            issue(SubmissionClaim(PREPARING, current.identity, current.current_run)),
            tamper_next_get=True,
        )
        with self.assertRaises(ClaimError):
            controller.record_claim(api, current, 7)
        self.assertEqual(api.events, ["get-issue:7"])


class ClaimContextTests(unittest.TestCase):
    def environment(self) -> dict[str, str]:
        value = context()
        return {
            "REPOSITORY": value.repository,
            "TAG": value.identity.source_tag,
            "COMMIT": value.identity.source_commit_sha,
            "MANIFEST_SHA256": value.identity.release_manifest_sha256,
            "PACKAGE_NAME": value.identity.package_name,
            "PACKAGE_SHA256": value.identity.package_sha256,
            "REQUEST_UUID": value.identity.request_uuid,
            "RUN_ID": value.current_run.run_id,
            "RUN_ATTEMPT": value.current_run.run_attempt,
            "WORKFLOW_SHA": value.current_run.workflow_sha,
            "AUTOMATION_USER_ID": str(value.automation_user_id),
            "AUTOMATION_LOGIN": value.automation_login,
        }

    def test_environment_round_trips_exact_context(self) -> None:
        actual = controller.ClaimContext.from_environment(self.environment())
        self.assertEqual(actual, context())

    def test_unsafe_identity_fails_before_api_use(self) -> None:
        for field, replacement in (
            ("REPOSITORY", "owner/repository/extra"),
            ("AUTOMATION_USER_ID", "041898282"),
            ("AUTOMATION_LOGIN", "attacker"),
            ("TAG", "v0.2.0\nINJECTED=1"),
        ):
            environment = self.environment()
            environment[field] = replacement
            with self.subTest(field=field), self.assertRaises(
                (ClaimError, controller.ControllerError)
            ):
                controller.ClaimContext.from_environment(environment)


class RestGitHubApiTests(unittest.TestCase):
    def test_issue_request_uses_only_the_fixed_origin_and_exact_repository(self) -> None:
        opener = FakeOpener(FakeResponse([]))
        api = controller.RestGitHubApi(
            token="test-token",
            repository="owner/repository",
            opener=opener,
        )
        self.assertEqual(api.list_issues(), [[]])
        request = opener.requests[0]
        self.assertEqual(
            request.full_url,
            "https://api.github.com/repos/owner/repository/issues"
            "?state=all&per_page=100&page=1",
        )
        self.assertEqual(request.get_method(), "GET")
        self.assertEqual(request.get_header("Authorization"), "Bearer test-token")
        self.assertNotIn("test-token", request.full_url)

    def test_job_pagination_is_deterministic_and_complete(self) -> None:
        jobs = [{"id": number} for number in range(100)]
        opener = FakeOpener(
            FakeResponse({"total_count": 101, "jobs": jobs}),
            FakeResponse({"total_count": 101, "jobs": [{"id": 100}]}),
        )
        api = controller.RestGitHubApi(
            token="test-token",
            repository="owner/repository",
            opener=opener,
        )
        pages = api.list_run_attempt_jobs(publisher_run())
        self.assertEqual(len(pages), 2)
        self.assertTrue(opener.requests[0].full_url.endswith("&page=1"))
        self.assertTrue(opener.requests[1].full_url.endswith("&page=2"))

    def test_malformed_or_unexpected_api_response_fails_closed(self) -> None:
        malformed = FakeResponse({"ignored": True})
        malformed.payload = b'{"number":1,"number":2}'
        for response in (malformed, FakeResponse([], status=202)):
            opener = FakeOpener(response)
            api = controller.RestGitHubApi(
                token="test-token",
                repository="owner/repository",
                opener=opener,
            )
            with self.subTest(status=response.status), self.assertRaises(
                controller.ControllerError
            ):
                api.list_issues()


if __name__ == "__main__":
    unittest.main()
