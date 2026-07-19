#!/usr/bin/env python3
"""Exact issue claim and safe retry policy for Chocolatey submissions."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import sys
from typing import Mapping, Sequence
from uuid import UUID


LABEL = "chocolatey-submission"
SCHEMA_VERSION = 2
PREPARING = "PREPARING"
SUBMITTED = "SUBMITTED"
PUBLISH_JOB = "push the exact package with the isolated API key"
PUSH_STEP = "Verify handoff and push as the final credential-bearing step"

_TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
_SHA1 = re.compile(r"[0-9a-f]{40}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POSITIVE = re.compile(r"[1-9][0-9]*")
_BOT_LOGIN = re.compile(r"[A-Za-z0-9][A-Za-z0-9-]{0,79}\[bot\]")


class ClaimError(ValueError):
    """Raised when an issue claim or retry outcome is not provably safe."""


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ClaimError(f"JSON contains duplicate field: {key}")
        value[key] = item
    return value


def load_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_strict_pairs)
    except json.JSONDecodeError as error:
        raise ClaimError(f"{path} contains malformed JSON") from error


def _positive(value: str, label: str) -> str:
    if not isinstance(value, str) or _POSITIVE.fullmatch(value) is None:
        raise ClaimError(f"{label} is not a positive canonical decimal")
    return value


@dataclass(frozen=True)
class SubmissionIdentity:
    source_tag: str
    source_commit_sha: str
    release_manifest_sha256: str
    package_name: str
    package_sha256: str
    request_uuid: str

    @classmethod
    def from_mapping(cls, value: object) -> "SubmissionIdentity":
        fields = {
            "source_tag",
            "source_commit_sha",
            "release_manifest_sha256",
            "package_name",
            "package_sha256",
            "request_uuid",
        }
        if not isinstance(value, Mapping) or set(value) != fields:
            raise ClaimError("submission identity fields are not exact")
        if any(not isinstance(value[field], str) for field in fields):
            raise ClaimError("submission identity values must be strings")
        identity = cls(**{field: value[field] for field in fields})
        identity.validate()
        return identity

    def validate(self) -> None:
        if _TAG.fullmatch(self.source_tag) is None:
            raise ClaimError("submission source tag is not a canonical stable tag")
        if _SHA1.fullmatch(self.source_commit_sha) is None:
            raise ClaimError("submission source commit is not a lowercase full SHA")
        if _SHA256.fullmatch(self.release_manifest_sha256) is None:
            raise ClaimError("submission manifest digest is not a lowercase SHA-256")
        if self.package_name != f"mcp-cpp-sdk.{self.version}.nupkg":
            raise ClaimError("submission package name differs from its source tag")
        if _SHA256.fullmatch(self.package_sha256) is None:
            raise ClaimError("submission package digest is not a lowercase SHA-256")
        try:
            request_id = UUID(self.request_uuid)
        except ValueError as error:
            raise ClaimError("submission request UUID is invalid") from error
        if str(request_id) != self.request_uuid:
            raise ClaimError("submission request UUID is not canonical lowercase")

    @property
    def version(self) -> str:
        return self.source_tag[1:]

    def as_mapping(self) -> dict[str, str]:
        return {
            "source_tag": self.source_tag,
            "source_commit_sha": self.source_commit_sha,
            "release_manifest_sha256": self.release_manifest_sha256,
            "package_name": self.package_name,
            "package_sha256": self.package_sha256,
            "request_uuid": self.request_uuid,
        }


@dataclass(frozen=True)
class PublisherRun:
    run_id: str
    run_attempt: str
    workflow_sha: str

    def __post_init__(self) -> None:
        _positive(self.run_id, "publisher workflow run ID")
        _positive(self.run_attempt, "publisher workflow run attempt")
        if _SHA1.fullmatch(self.workflow_sha) is None:
            raise ClaimError("publisher workflow SHA is not a lowercase full SHA")

    @classmethod
    def from_mapping(cls, value: object) -> "PublisherRun":
        if not isinstance(value, Mapping) or set(value) != {
            "run_id",
            "run_attempt",
            "workflow_sha",
        }:
            raise ClaimError("publisher run fields are not exact")
        if any(not isinstance(item, str) for item in value.values()):
            raise ClaimError("publisher run values must be strings")
        return cls(
            run_id=value["run_id"],
            run_attempt=value["run_attempt"],
            workflow_sha=value["workflow_sha"],
        )

    def as_mapping(self) -> dict[str, str]:
        return {
            "run_id": self.run_id,
            "run_attempt": self.run_attempt,
            "workflow_sha": self.workflow_sha,
        }


@dataclass(frozen=True)
class SubmissionClaim:
    state: str
    identity: SubmissionIdentity
    publisher_run: PublisherRun

    def __post_init__(self) -> None:
        if self.state not in {PREPARING, SUBMITTED}:
            raise ClaimError("submission claim state is unsupported")

    @classmethod
    def from_mapping(cls, value: object) -> "SubmissionClaim":
        if not isinstance(value, Mapping) or set(value) != {
            "schema_version",
            "state",
            "identity",
            "publisher_run",
        }:
            raise ClaimError("submission claim schema fields are not exact")
        if value["schema_version"] != SCHEMA_VERSION or not isinstance(value["state"], str):
            raise ClaimError("submission claim schema version or state is invalid")
        return cls(
            state=value["state"],
            identity=SubmissionIdentity.from_mapping(value["identity"]),
            publisher_run=PublisherRun.from_mapping(value["publisher_run"]),
        )

    def as_mapping(self) -> dict[str, object]:
        return {
            "schema_version": SCHEMA_VERSION,
            "state": self.state,
            "identity": self.identity.as_mapping(),
            "publisher_run": self.publisher_run.as_mapping(),
        }


def issue_title(identity: SubmissionIdentity) -> str:
    return f"[chocolatey] mcp-cpp-sdk {identity.version}"


def issue_marker(identity: SubmissionIdentity) -> str:
    return (
        "<!-- chocolatey-submission:v2 package=mcp-cpp-sdk "
        f"version={identity.version} -->"
    )


def render_body(claim: SubmissionClaim) -> str:
    display = PREPARING if claim.state == PREPARING else "SUBMITTED_PENDING_MODERATION"
    encoded = json.dumps(claim.as_mapping(), sort_keys=True, separators=(",", ":"))
    return (
        issue_marker(claim.identity)
        + f"\n\nChocolatey submission state: **{display}**\n\n"
        + "<!-- chocolatey-submission-json\n"
        + encoded
        + "\nchocolatey-submission-json -->\n"
    )


def parse_body(body: object) -> SubmissionClaim:
    if not isinstance(body, str) or len(body.encode("utf-8")) > 64 * 1024:
        raise ClaimError("submission claim body is missing or oversized")
    prefix = "<!-- chocolatey-submission-json\n"
    suffix = "\nchocolatey-submission-json -->\n"
    if body.count(prefix) != 1 or body.count(suffix) != 1 or not body.endswith(suffix):
        raise ClaimError("submission claim body delimiters are malformed")
    encoded = body.split(prefix, 1)[1].split(suffix, 1)[0]
    try:
        claim = SubmissionClaim.from_mapping(
            json.loads(encoded, object_pairs_hook=_strict_pairs)
        )
    except json.JSONDecodeError as error:
        raise ClaimError("submission claim body contains malformed JSON") from error
    if body != render_body(claim):
        raise ClaimError("submission claim body is not canonical")
    return claim


def _issue_number(issue: Mapping[str, object]) -> int:
    number = issue.get("number")
    if type(number) is not int or number < 1:
        raise ClaimError("submission claim issue number is invalid")
    return number


def validate_automation_identity(user_id: int, login: str) -> None:
    if type(user_id) is not int or user_id < 1:
        raise ClaimError("automation user ID must be positive")
    if not isinstance(login, str) or _BOT_LOGIN.fullmatch(login) is None:
        raise ClaimError("automation bot login is not canonical")


def validate_issue(
    value: object,
    *,
    identity: SubmissionIdentity,
    automation_user_id: int,
    automation_login: str,
    expected_state: str | None = None,
    expected_run: PublisherRun | None = None,
) -> SubmissionClaim:
    validate_automation_identity(automation_user_id, automation_login)
    if not isinstance(value, Mapping):
        raise ClaimError("submission claim issue is not an object")
    _issue_number(value)
    labels = value.get("labels")
    user = value.get("user")
    if (
        "pull_request" in value
        or value.get("state") != "open"
        or value.get("locked") is not False
        or value.get("title") != issue_title(identity)
        or not isinstance(labels, list)
        or len(labels) != 1
        or not isinstance(labels[0], Mapping)
        or labels[0].get("name") != LABEL
        or not isinstance(user, Mapping)
        or user.get("id") != automation_user_id
        or user.get("login") != automation_login
        or user.get("type") != "Bot"
    ):
        raise ClaimError("submission claim issue metadata is not exact automation output")
    claim = parse_body(value.get("body"))
    if claim.identity != identity:
        raise ClaimError("submission claim conflicts with this release identity")
    if expected_state is not None and claim.state != expected_state:
        raise ClaimError("submission claim state changed unexpectedly")
    if expected_run is not None and claim.publisher_run != expected_run:
        raise ClaimError("submission claim publisher run changed unexpectedly")
    return claim


@dataclass(frozen=True)
class Inspection:
    action: str
    issue_number: int | None = None
    prior_run: PublisherRun | None = None


def inspect_issue_pages(
    value: object,
    *,
    identity: SubmissionIdentity,
    current_run: PublisherRun,
    automation_user_id: int,
    automation_login: str,
) -> Inspection:
    if not isinstance(value, list) or any(not isinstance(page, list) for page in value):
        raise ClaimError("GitHub issue pages are malformed")
    issues = [issue for page in value for issue in page]
    if any(not isinstance(issue, Mapping) for issue in issues):
        raise ClaimError("GitHub issue record is malformed")
    ids = [issue.get("id") for issue in issues]
    if any(type(issue_id) is not int or issue_id < 1 for issue_id in ids):
        raise ClaimError("GitHub issue numeric identity is malformed")
    if len(ids) != len(set(ids)):
        raise ClaimError("GitHub issue pages contain duplicate identities")
    title = issue_title(identity)
    probe = f"package=mcp-cpp-sdk version={identity.version}"
    candidates = [
        issue
        for issue in issues
        if issue.get("title") == title
        or (
            isinstance(issue.get("body"), str)
            and "chocolatey-submission:" in issue["body"]
            and probe in issue["body"]
        )
    ]
    if len(candidates) > 1:
        raise ClaimError("submission claim identity is ambiguous")
    if not candidates:
        return Inspection("create")
    issue = candidates[0]
    claim = validate_issue(
        issue,
        identity=identity,
        automation_user_id=automation_user_id,
        automation_login=automation_login,
    )
    number = _issue_number(issue)
    if claim.state == SUBMITTED:
        return Inspection("submitted", number, claim.publisher_run)
    if claim.publisher_run == current_run:
        raise ClaimError("current workflow attempt already owns a PREPARING claim")
    return Inspection("reconcile", number, claim.publisher_run)


def reconcile_job_pages(
    value: object, *, run_value: object, prior_run: PublisherRun
) -> str:
    if not isinstance(run_value, Mapping):
        raise ClaimError("prior workflow attempt is not an object")
    if (
        run_value.get("id") != int(prior_run.run_id)
        or run_value.get("run_attempt") != int(prior_run.run_attempt)
        or run_value.get("head_sha") != prior_run.workflow_sha
        or run_value.get("event") != "workflow_dispatch"
        or run_value.get("head_branch") != "main"
        or run_value.get("path") != ".github/workflows/publish.yml"
        or run_value.get("status") != "completed"
        or run_value.get("conclusion")
        not in {"success", "failure", "cancelled", "timed_out"}
    ):
        raise ClaimError("prior workflow attempt identity is incomplete or conflicting")
    if not isinstance(value, list) or any(not isinstance(page, Mapping) for page in value):
        raise ClaimError("prior workflow job pages are malformed")
    jobs: list[Mapping[str, object]] = []
    total_counts: set[int] = set()
    for page in value:
        page_jobs = page.get("jobs")
        if type(page.get("total_count")) is not int or not isinstance(page_jobs, list):
            raise ClaimError("prior workflow job page schema is malformed")
        total_counts.add(page["total_count"])
        if any(not isinstance(job, Mapping) for job in page_jobs):
            raise ClaimError("prior workflow job record is malformed")
        jobs.extend(page_jobs)
    if total_counts != {len(jobs)}:
        raise ClaimError("prior workflow job pagination is incomplete or conflicting")
    ids = [job.get("id") for job in jobs]
    if any(type(job_id) is not int or job_id < 1 for job_id in ids) or len(ids) != len(set(ids)):
        raise ClaimError("prior workflow jobs have malformed or duplicate identities")
    matches = [job for job in jobs if job.get("name") == PUBLISH_JOB]
    if len(matches) != 1:
        raise ClaimError("prior publish job evidence is missing or ambiguous; reconcile manually")
    job = matches[0]
    if (
        job.get("run_id") != int(prior_run.run_id)
        or job.get("head_sha") != prior_run.workflow_sha
    ):
        raise ClaimError("prior publish job identity conflicts with the PREPARING claim")
    steps = job.get("steps")
    if job.get("status") == "completed" and job.get("conclusion") == "success":
        if not isinstance(steps, list) or any(not isinstance(step, Mapping) for step in steps):
            raise ClaimError("successful prior publish job has malformed step evidence")
        push_steps = [step for step in steps if step.get("name") == PUSH_STEP]
        if (
            len(push_steps) != 1
            or push_steps[0].get("status") != "completed"
            or push_steps[0].get("conclusion") != "success"
        ):
            raise ClaimError("prior publish success lacks exact successful push-step evidence")
        return "submitted"
    if (
        job.get("status") == "completed"
        and job.get("conclusion") == "skipped"
        and steps in (None, [])
        and job.get("runner_id") is None
    ):
        return "resume"
    raise ClaimError(
        "prior publish job may have used the Chocolatey credential; reconcile manually"
    )


def issue_request(claim: SubmissionClaim, *, create: bool) -> dict[str, object]:
    value: dict[str, object] = {"body": render_body(claim)}
    if create:
        value = {
            "title": issue_title(claim.identity),
            "body": render_body(claim),
            "labels": [LABEL],
        }
    return value


def write_issue_request(path: Path, claim: SubmissionClaim, *, create: bool) -> None:
    value = issue_request(claim, create=create)
    path.write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _identity_from_args(args: argparse.Namespace) -> SubmissionIdentity:
    return SubmissionIdentity.from_mapping(
        {
            "source_tag": args.source_tag,
            "source_commit_sha": args.source_commit_sha,
            "release_manifest_sha256": args.release_manifest_sha256,
            "package_name": args.package_name,
            "package_sha256": args.package_sha256,
            "request_uuid": args.request_uuid,
        }
    )


def _run_from_args(args: argparse.Namespace, prefix: str = "") -> PublisherRun:
    return PublisherRun(
        run_id=getattr(args, f"{prefix}run_id"),
        run_attempt=getattr(args, f"{prefix}run_attempt"),
        workflow_sha=getattr(args, f"{prefix}workflow_sha"),
    )


def _add_identity(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--source-tag", required=True)
    parser.add_argument("--source-commit-sha", required=True)
    parser.add_argument("--release-manifest-sha256", required=True)
    parser.add_argument("--package-name", required=True)
    parser.add_argument("--package-sha256", required=True)
    parser.add_argument("--request-uuid", required=True)


def _add_run(parser: argparse.ArgumentParser, prefix: str = "") -> None:
    option = prefix.replace("_", "-")
    parser.add_argument(f"--{option}run-id", dest=f"{prefix}run_id", required=True)
    parser.add_argument(
        f"--{option}run-attempt", dest=f"{prefix}run_attempt", required=True
    )
    parser.add_argument(
        f"--{option}workflow-sha", dest=f"{prefix}workflow_sha", required=True
    )


def _add_automation(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--automation-user-id", type=int, required=True)
    parser.add_argument("--automation-login", required=True)


def _write_output(path: Path, values: Mapping[str, object]) -> None:
    with path.open("a", encoding="ascii") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect")
    inspect.add_argument("--issues", type=Path, required=True)
    inspect.add_argument("--request-output", type=Path, required=True)
    inspect.add_argument("--github-output", type=Path, required=True)
    _add_identity(inspect)
    _add_run(inspect)
    _add_automation(inspect)

    reconcile = commands.add_parser("reconcile")
    reconcile.add_argument("--run", type=Path, required=True)
    reconcile.add_argument("--jobs", type=Path, required=True)
    reconcile.add_argument("--github-output", type=Path, required=True)
    _add_run(reconcile, "prior_")

    render = commands.add_parser("render")
    render.add_argument("--state", choices=(PREPARING, SUBMITTED), required=True)
    render.add_argument("--request-output", type=Path, required=True)
    render.add_argument("--create", action="store_true")
    _add_identity(render)
    _add_run(render)

    verify = commands.add_parser("verify")
    verify.add_argument("--issue", type=Path, required=True)
    verify.add_argument("--state", choices=(PREPARING, SUBMITTED), required=True)
    _add_identity(verify)
    _add_run(verify)
    _add_automation(verify)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "inspect":
            identity = _identity_from_args(args)
            current_run = _run_from_args(args)
            result = inspect_issue_pages(
                load_json(args.issues),
                identity=identity,
                current_run=current_run,
                automation_user_id=args.automation_user_id,
                automation_login=args.automation_login,
            )
            values: dict[str, object] = {"action": result.action}
            if result.issue_number is not None:
                values["issue_number"] = result.issue_number
            if result.prior_run is not None:
                values.update(
                    {
                        "prior_run_id": result.prior_run.run_id,
                        "prior_run_attempt": result.prior_run.run_attempt,
                        "prior_workflow_sha": result.prior_run.workflow_sha,
                    }
                )
            if result.action == "create":
                write_issue_request(
                    args.request_output,
                    SubmissionClaim(PREPARING, identity, current_run),
                    create=True,
                )
            _write_output(args.github_output, values)
        elif args.command == "reconcile":
            action = reconcile_job_pages(
                load_json(args.jobs),
                run_value=load_json(args.run),
                prior_run=_run_from_args(args, "prior_"),
            )
            _write_output(args.github_output, {"reconcile_action": action})
        elif args.command == "render":
            write_issue_request(
                args.request_output,
                SubmissionClaim(args.state, _identity_from_args(args), _run_from_args(args)),
                create=args.create,
            )
        else:
            validate_issue(
                load_json(args.issue),
                identity=_identity_from_args(args),
                automation_user_id=args.automation_user_id,
                automation_login=args.automation_login,
                expected_state=args.state,
                expected_run=_run_from_args(args),
            )
    except (ClaimError, OSError, UnicodeError) as error:
        print(f"submission-claim: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
