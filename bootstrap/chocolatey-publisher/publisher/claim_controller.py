#!/usr/bin/env python3
"""Operate exact Chocolatey submission claims through the GitHub REST API."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import sys
from typing import Mapping, Protocol, Sequence
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import (
    HTTPRedirectHandler,
    Request,
    build_opener,
)


if __package__:
    from .submission_claim import (
        ClaimError,
        PREPARING,
        SUBMITTED,
        PublisherRun,
        SubmissionClaim,
        SubmissionIdentity,
        inspect_issue_pages,
        issue_request,
        reconcile_job_pages,
        validate_automation_identity,
        validate_issue,
    )
else:
    # Isolated mode deliberately omits the script directory from sys.path.
    # Import only the sibling module from this protected checkout.
    _PUBLISHER_DIRECTORY = Path(__file__).resolve().parent
    sys.path.insert(0, str(_PUBLISHER_DIRECTORY))
    from submission_claim import (  # type: ignore[no-redef]
        ClaimError,
        PREPARING,
        SUBMITTED,
        PublisherRun,
        SubmissionClaim,
        SubmissionIdentity,
        inspect_issue_pages,
        issue_request,
        reconcile_job_pages,
        validate_automation_identity,
        validate_issue,
    )


API_ORIGIN = "https://api.github.com"
API_VERSION = "2022-11-28"
MAX_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_ISSUE_PAGES = 100
MAX_JOB_PAGES = 100
PER_PAGE = 100

_POSITIVE = re.compile(r"[1-9][0-9]*")
_REPOSITORY = re.compile(
    r"[A-Za-z0-9](?:[A-Za-z0-9_.-]{0,99})/"
    r"[A-Za-z0-9](?:[A-Za-z0-9_.-]{0,99})"
)


class ControllerError(ValueError):
    """Raised when API evidence or controller input is not provably safe."""


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ControllerError(f"GitHub response contains duplicate field: {key}")
        value[key] = item
    return value


def _required(environment: Mapping[str, str], name: str) -> str:
    value = environment.get(name)
    if value is None or not value or "\0" in value or "\r" in value or "\n" in value:
        raise ControllerError(f"{name} is missing or contains unsafe characters")
    return value


def _positive(value: str, label: str) -> str:
    if _POSITIVE.fullmatch(value) is None:
        raise ControllerError(f"{label} is not a positive canonical decimal")
    return value


def _repository(value: str) -> str:
    if _REPOSITORY.fullmatch(value) is None:
        raise ControllerError("REPOSITORY is not a canonical owner/name pair")
    owner, name = value.split("/", 1)
    if owner in {".", ".."} or name in {".", ".."}:
        raise ControllerError("REPOSITORY contains an unsafe path component")
    return value


@dataclass(frozen=True)
class ClaimContext:
    repository: str
    identity: SubmissionIdentity
    current_run: PublisherRun
    automation_user_id: int
    automation_login: str

    @classmethod
    def from_environment(cls, environment: Mapping[str, str]) -> "ClaimContext":
        automation_id = _positive(
            _required(environment, "AUTOMATION_USER_ID"),
            "AUTOMATION_USER_ID",
        )
        automation_login = _required(environment, "AUTOMATION_LOGIN")
        validate_automation_identity(int(automation_id), automation_login)
        return cls(
            repository=_repository(_required(environment, "REPOSITORY")),
            identity=SubmissionIdentity.from_mapping(
                {
                    "source_tag": _required(environment, "TAG"),
                    "source_commit_sha": _required(environment, "COMMIT"),
                    "release_manifest_sha256": _required(
                        environment, "MANIFEST_SHA256"
                    ),
                    "package_name": _required(environment, "PACKAGE_NAME"),
                    "package_sha256": _required(environment, "PACKAGE_SHA256"),
                    "request_uuid": _required(environment, "REQUEST_UUID"),
                }
            ),
            current_run=PublisherRun(
                run_id=_required(environment, "RUN_ID"),
                run_attempt=_required(environment, "RUN_ATTEMPT"),
                workflow_sha=_required(environment, "WORKFLOW_SHA"),
            ),
            automation_user_id=int(automation_id),
            automation_login=automation_login,
        )


@dataclass(frozen=True)
class ManageResult:
    issue_number: int
    already_submitted: bool


class ClaimApi(Protocol):
    """Small interface used by the controller and by offline test doubles."""

    def list_issues(self) -> list[list[object]]: ...

    def create_issue(self, request: Mapping[str, object]) -> object: ...

    def get_issue(self, number: int) -> object: ...

    def update_issue(self, number: int, request: Mapping[str, object]) -> object: ...

    def get_run_attempt(self, run: PublisherRun) -> object: ...

    def list_run_attempt_jobs(self, run: PublisherRun) -> list[object]: ...


class _RejectRedirects(HTTPRedirectHandler):
    def redirect_request(  # type: ignore[override]
        self,
        request: Request,
        file_pointer: object,
        code: int,
        message: str,
        headers: object,
        new_url: str,
    ) -> None:
        return None


class RestGitHubApi:
    """Minimal, redirect-free GitHub client with bounded JSON responses."""

    def __init__(
        self,
        *,
        token: str,
        repository: str,
        opener: object | None = None,
    ) -> None:
        if not token or len(token) > 4096 or any(char in token for char in "\0\r\n"):
            raise ControllerError("GH_TOKEN is missing or malformed")
        self._token = token
        self._repository = _repository(repository)
        self._opener = opener or build_opener(_RejectRedirects())

    def _request(
        self,
        method: str,
        path: str,
        *,
        expected_status: int,
        query: Mapping[str, object] | None = None,
        body: Mapping[str, object] | None = None,
    ) -> object:
        url = API_ORIGIN + path
        if query:
            url += "?" + urlencode(query)
        encoded = None
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "User-Agent": "mcp-cpp-sdk-chocolatey-publisher",
            "X-GitHub-Api-Version": API_VERSION,
        }
        if body is not None:
            encoded = json.dumps(body, sort_keys=True, separators=(",", ":")).encode(
                "utf-8"
            )
            headers["Content-Type"] = "application/json"
        request = Request(url, data=encoded, headers=headers, method=method)
        try:
            response = self._opener.open(request, timeout=30)
            with response:
                if response.status != expected_status:
                    raise ControllerError(
                        "GitHub API returned HTTP "
                        f"{response.status} for {method} {path}"
                    )
                payload = response.read(MAX_RESPONSE_BYTES + 1)
        except HTTPError as error:
            raise ControllerError(
                f"GitHub API returned HTTP {error.code} for {method} {path}"
            ) from error
        except URLError as error:
            raise ControllerError(
                f"GitHub API request failed for {method} {path}"
            ) from error
        if len(payload) > MAX_RESPONSE_BYTES:
            raise ControllerError(
                f"GitHub API response is oversized for {method} {path}"
            )
        try:
            return json.loads(payload.decode("utf-8"), object_pairs_hook=_strict_pairs)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ControllerError(
                f"GitHub API returned malformed JSON for {method} {path}"
            ) from error

    @property
    def _repository_path(self) -> str:
        return f"/repos/{self._repository}"

    def list_issues(self) -> list[list[object]]:
        pages: list[list[object]] = []
        for page_number in range(1, MAX_ISSUE_PAGES + 1):
            value = self._request(
                "GET",
                f"{self._repository_path}/issues",
                expected_status=200,
                query={"state": "all", "per_page": PER_PAGE, "page": page_number},
            )
            if not isinstance(value, list):
                raise ControllerError("GitHub issue page is not an array")
            pages.append(value)
            if len(value) < PER_PAGE:
                return pages
        raise ControllerError("GitHub issue pagination exceeded its safety bound")

    def create_issue(self, request: Mapping[str, object]) -> object:
        return self._request(
            "POST",
            f"{self._repository_path}/issues",
            expected_status=201,
            body=request,
        )

    def get_issue(self, number: int) -> object:
        return self._request(
            "GET",
            f"{self._repository_path}/issues/{number}",
            expected_status=200,
        )

    def update_issue(self, number: int, request: Mapping[str, object]) -> object:
        return self._request(
            "PATCH",
            f"{self._repository_path}/issues/{number}",
            expected_status=200,
            body=request,
        )

    def get_run_attempt(self, run: PublisherRun) -> object:
        return self._request(
            "GET",
            f"{self._repository_path}/actions/runs/{run.run_id}/attempts/"
            f"{run.run_attempt}",
            expected_status=200,
        )

    def list_run_attempt_jobs(self, run: PublisherRun) -> list[object]:
        path = (
            f"{self._repository_path}/actions/runs/{run.run_id}/attempts/"
            f"{run.run_attempt}/jobs"
        )
        first = self._request(
            "GET",
            path,
            expected_status=200,
            query={"filter": "all", "per_page": PER_PAGE, "page": 1},
        )
        if (
            not isinstance(first, Mapping)
            or type(first.get("total_count")) is not int
            or first["total_count"] < 0
            or not isinstance(first.get("jobs"), list)
        ):
            raise ControllerError("GitHub job page has an invalid pagination schema")
        pages: list[object] = [first]
        page_count = max(1, (first["total_count"] + PER_PAGE - 1) // PER_PAGE)
        if page_count > MAX_JOB_PAGES:
            raise ControllerError("GitHub job pagination exceeded its safety bound")
        for page_number in range(2, page_count + 1):
            pages.append(
                self._request(
                    "GET",
                    path,
                    expected_status=200,
                    query={
                        "filter": "all",
                        "per_page": PER_PAGE,
                        "page": page_number,
                    },
                )
            )
        return pages


def _issue_number(value: object) -> int:
    if not isinstance(value, Mapping):
        raise ControllerError("GitHub issue creation response is not an object")
    number = value.get("number")
    if type(number) is not int or number < 1:
        raise ControllerError(
            "GitHub issue creation response has no valid issue number"
        )
    return number


def _validate(
    value: object,
    *,
    context: ClaimContext,
    state: str,
    run: PublisherRun,
) -> None:
    validate_issue(
        value,
        identity=context.identity,
        automation_user_id=context.automation_user_id,
        automation_login=context.automation_login,
        expected_state=state,
        expected_run=run,
    )


def manage_claim(api: ClaimApi, context: ClaimContext) -> ManageResult:
    inspection = inspect_issue_pages(
        api.list_issues(),
        identity=context.identity,
        current_run=context.current_run,
        automation_user_id=context.automation_user_id,
        automation_login=context.automation_login,
    )
    if inspection.action == "create":
        request = issue_request(
            SubmissionClaim(PREPARING, context.identity, context.current_run),
            create=True,
        )
        number = _issue_number(api.create_issue(request))
        _validate(
            api.get_issue(number),
            context=context,
            state=PREPARING,
            run=context.current_run,
        )
        return ManageResult(number, False)

    if inspection.issue_number is None or inspection.prior_run is None:
        raise ControllerError("claim inspection omitted its exact prior identity")
    number = inspection.issue_number
    prior_run = inspection.prior_run
    if inspection.action == "submitted":
        _validate(
            api.get_issue(number),
            context=context,
            state=SUBMITTED,
            run=prior_run,
        )
        return ManageResult(number, True)
    if inspection.action != "reconcile":
        raise ControllerError("claim inspection returned an unsupported action")

    run_evidence = api.get_run_attempt(prior_run)
    job_evidence = api.list_run_attempt_jobs(prior_run)
    reconciliation = reconcile_job_pages(
        job_evidence,
        run_value=run_evidence,
        prior_run=prior_run,
    )
    if reconciliation == "submitted":
        target_state = SUBMITTED
        target_run = prior_run
        already_submitted = True
    elif reconciliation == "resume":
        target_state = PREPARING
        target_run = context.current_run
        already_submitted = False
    else:
        raise ControllerError("retry reconciliation returned an unsupported action")

    update = issue_request(
        SubmissionClaim(target_state, context.identity, target_run),
        create=False,
    )
    # This read and exact verification intentionally sit directly before the
    # mutation.  Edited, spoofed, or differently owned claims fail closed.
    _validate(
        api.get_issue(number),
        context=context,
        state=PREPARING,
        run=prior_run,
    )
    api.update_issue(number, update)
    _validate(
        api.get_issue(number),
        context=context,
        state=target_state,
        run=target_run,
    )
    return ManageResult(number, already_submitted)


def record_claim(api: ClaimApi, context: ClaimContext, issue_number: int) -> None:
    update = issue_request(
        SubmissionClaim(SUBMITTED, context.identity, context.current_run),
        create=False,
    )
    # The controller does no I/O between this exact read/verification and PATCH.
    _validate(
        api.get_issue(issue_number),
        context=context,
        state=PREPARING,
        run=context.current_run,
    )
    api.update_issue(issue_number, update)
    _validate(
        api.get_issue(issue_number),
        context=context,
        state=SUBMITTED,
        run=context.current_run,
    )


def _write_github_output(path: Path, result: ManageResult) -> None:
    with path.open("a", encoding="ascii") as output:
        output.write(f"issue_number={result.issue_number}\n")
        output.write(
            f"already_submitted={'true' if result.already_submitted else 'false'}\n"
        )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    manage = commands.add_parser("manage")
    manage.add_argument("--github-output", type=Path, required=True)
    record = commands.add_parser("record")
    record.add_argument("--issue-number", required=True)
    return parser


def main(
    argv: Sequence[str] | None = None,
    *,
    environment: Mapping[str, str] | None = None,
) -> int:
    args = _parser().parse_args(argv)
    environment = os.environ if environment is None else environment
    try:
        context = ClaimContext.from_environment(environment)
        api = RestGitHubApi(
            token=_required(environment, "GH_TOKEN"),
            repository=context.repository,
        )
        if args.command == "manage":
            result = manage_claim(api, context)
            _write_github_output(args.github_output, result)
        else:
            number = int(_positive(args.issue_number, "issue number"))
            record_claim(api, context, number)
    except (ClaimError, ControllerError, OSError, UnicodeError) as error:
        print(f"claim-controller: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
