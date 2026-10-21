"""Read-only, fail-closed GitHub repository and workflow identity checks."""

from __future__ import annotations

from dataclasses import dataclass
import json
import re
import subprocess
from typing import Callable, Mapping, Protocol, Sequence
from urllib.parse import quote

from .model import ValidationError
from .strict_json import loads_strict


_REPOSITORY_PART = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}")
_WORKFLOW_FILE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,94}\.ya?ml")


class GitHubProviderError(ValueError):
    """Raised when live GitHub state does not prove the intended operation."""


class ApiRequest(Protocol):
    def __call__(
        self, method: str, endpoint: str, payload: Mapping[str, object] | None = None
    ) -> object | None: ...


CommandRunner = Callable[[Sequence[str], bytes | None], bytes]


def _run_command(arguments: Sequence[str], stdin: bytes | None) -> bytes:
    return subprocess.run(
        list(arguments),
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        timeout=120,
    ).stdout


def mapping(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise GitHubProviderError(f"{label} is not a JSON object")
    return value


class GhApi:
    """Call ``gh api`` without a shell and parse duplicate-free JSON."""

    def __init__(self, runner: CommandRunner = _run_command) -> None:
        self._runner = runner

    def __call__(
        self, method: str, endpoint: str, payload: Mapping[str, object] | None = None
    ) -> object | None:
        if method not in {"GET", "POST"} or not endpoint or endpoint.startswith("/"):
            raise GitHubProviderError("invalid internal GitHub API request")
        arguments = [
            "gh",
            "api",
            "--method",
            method,
            "-H",
            "X-GitHub-Api-Version: 2026-03-10",
            endpoint,
        ]
        stdin = None
        if payload is not None:
            arguments.extend(("--input", "-"))
            stdin = (
                json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n"
            ).encode("utf-8")
        raw = self._runner(arguments, stdin)
        if not raw.strip():
            return None
        try:
            return loads_strict(raw, label="GitHub API response")
        except ValidationError as error:
            raise GitHubProviderError(str(error)) from error


@dataclass(frozen=True)
class RepositoryTarget:
    owner: str
    repository: str
    repository_id: int
    default_branch: str = "main"

    def __post_init__(self) -> None:
        for label, value in (("owner", self.owner), ("repository", self.repository)):
            if (
                not isinstance(value, str)
                or _REPOSITORY_PART.fullmatch(value) is None
                or ".." in value
            ):
                raise GitHubProviderError(f"GitHub {label} is not canonical")
        if type(self.repository_id) is not int or self.repository_id < 1:
            raise GitHubProviderError("expected repository ID must be positive")
        if self.default_branch != "main":
            raise GitHubProviderError("publisher workflow must use protected main")

    @property
    def full_name(self) -> str:
        return f"{self.owner}/{self.repository}"


def validate_repository(api: ApiRequest, target: RepositoryTarget) -> None:
    repository = mapping(api("GET", f"repos/{target.full_name}"), "repository")
    if repository.get("id") != target.repository_id:
        raise GitHubProviderError("repository numeric identity differs from configuration")
    if repository.get("full_name") != target.full_name:
        raise GitHubProviderError("repository full name differs from configuration")
    if repository.get("default_branch") != target.default_branch:
        raise GitHubProviderError("repository default branch is not protected main")
    if repository.get("archived") is not False or repository.get("disabled") is not False:
        raise GitHubProviderError("publisher repository is archived, disabled, or ambiguous")


def validate_workflow(api: ApiRequest, target: RepositoryTarget, workflow: str) -> int:
    if (
        not isinstance(workflow, str)
        or _WORKFLOW_FILE.fullmatch(workflow) is None
        or ".." in workflow
    ):
        raise GitHubProviderError("publisher workflow filename is not canonical")
    value = mapping(
        api(
            "GET",
            f"repos/{target.full_name}/actions/workflows/{quote(workflow, safe='')}",
        ),
        "publisher workflow",
    )
    workflow_id = value.get("id")
    if type(workflow_id) is not int or workflow_id < 1:
        raise GitHubProviderError("publisher workflow has no stable numeric identity")
    if value.get("path") != f".github/workflows/{workflow}":
        raise GitHubProviderError("publisher workflow path differs from configuration")
    if value.get("state") != "active":
        raise GitHubProviderError("publisher workflow is not active")
    return workflow_id
