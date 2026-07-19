#!/usr/bin/env python3
"""Verify or update the one fixed Homebrew formula branch by GitHub API."""

from __future__ import annotations

import argparse
import base64
import http.client
import json
import os
from pathlib import Path
import re
import sys
import urllib.parse
from collections.abc import Mapping


OWNER = "yurirocha15"
REPOSITORY = "homebrew-mcp-cpp-sdk"
FORMULA = "Formula/mcp-cpp-sdk.rb"
BOT = "mcp-cpp-sdk-homebrew-publisher[bot]"
SHA = re.compile(r"[0-9a-f]{40}")
DECIMAL = re.compile(r"[1-9][0-9]*")
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
BRANCH = re.compile(r"release/mcp-cpp-sdk-v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


class FormulaBranchError(ValueError):
    """Raised when formula branch state is not the one authorized state."""


class Client:
    def __init__(self, token: str) -> None:
        if not token or len(token) > 8192:
            raise FormulaBranchError("GitHub token is missing or malformed")
        self._token = token

    def request(
        self, method: str, path: str, body: Mapping[str, object] | None = None
    ) -> object:
        encoded = None if body is None else json.dumps(body, separators=(",", ":")).encode("utf-8")
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "User-Agent": "mcp-cpp-sdk-homebrew-publisher/2",
            "X-GitHub-Api-Version": "2026-03-10",
        }
        if encoded is not None:
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPSConnection("api.github.com", timeout=30)
        try:
            connection.request(method, path, body=encoded, headers=headers)
            response = connection.getresponse()
            raw = response.read(1024 * 1024 + 1)
            if len(raw) > 1024 * 1024 or 300 <= response.status < 400:
                raise FormulaBranchError("GitHub API response is oversized or redirected")
            if not 200 <= response.status < 300:
                raise FormulaBranchError(f"GitHub API request failed with status {response.status}")
            return json.loads(raw)
        except json.JSONDecodeError as error:
            raise FormulaBranchError("GitHub API response is not JSON") from error
        finally:
            connection.close()


def _message(tag: str) -> str:
    if TAG.fullmatch(tag) is None:
        raise FormulaBranchError("source tag is malformed")
    return f"brew: add bottles for {tag}"


def _pull(value: object, *, pr: str, branch: str) -> str:
    if DECIMAL.fullmatch(pr) is None or BRANCH.fullmatch(branch) is None:
        raise FormulaBranchError("formula pull request identity is malformed")
    if not isinstance(value, Mapping):
        raise FormulaBranchError("formula pull request response is malformed")
    base = value.get("base")
    head = value.get("head")
    user = value.get("user")
    if (
        not isinstance(base, Mapping)
        or not isinstance(head, Mapping)
        or not isinstance(user, Mapping)
    ):
        raise FormulaBranchError("formula pull request refs are missing")
    repository = head.get("repo")
    if (
        value.get("number") != int(pr)
        or value.get("state") != "open"
        or value.get("draft") is not False
        or base.get("ref") != "main"
        or head.get("ref") != branch
        or not isinstance(repository, Mapping)
        or repository.get("full_name") != f"{OWNER}/{REPOSITORY}"
        or user.get("login") != BOT
        or user.get("type") != "Bot"
        or SHA.fullmatch(str(head.get("sha", ""))) is None
    ):
        raise FormulaBranchError("formula pull request identity changed")
    return str(head["sha"])


def _pull_files(value: object) -> None:
    if (
        not isinstance(value, list)
        or len(value) != 1
        or not isinstance(value[0], Mapping)
        or value[0].get("filename") != FORMULA
    ):
        raise FormulaBranchError("formula pull request changes files outside the formula")


def _prior_commit(value: object, *, head: str, expected: str, tag: str) -> None:
    if not isinstance(value, Mapping) or value.get("sha") != head:
        raise FormulaBranchError("prior bottled commit response is malformed")
    parents = value.get("parents")
    files = value.get("files")
    author = value.get("author")
    committer = value.get("committer")
    commit = value.get("commit")
    if (
        not isinstance(parents, list)
        or len(parents) != 1
        or not isinstance(parents[0], Mapping)
        or parents[0].get("sha") != expected
        or not isinstance(files, list)
        or len(files) != 1
        or not isinstance(files[0], Mapping)
        or files[0].get("filename") != FORMULA
        or files[0].get("status") != "modified"
        or not isinstance(author, Mapping)
        or author.get("login") != BOT
        or author.get("type") != "Bot"
        or not isinstance(committer, Mapping)
        or committer.get("login") != BOT
        or committer.get("type") != "Bot"
        or not isinstance(commit, Mapping)
        or commit.get("message") != _message(tag)
        or not isinstance(commit.get("verification"), Mapping)
        or commit["verification"].get("verified") is not True
    ):
        raise FormulaBranchError("prior bottled branch commit is not the exact idempotent state")


def verify_head(
    client: Client, *, pr: str, branch: str, expected_head: str, tag: str
) -> str:
    if SHA.fullmatch(expected_head) is None:
        raise FormulaBranchError("expected formula head is malformed")
    if branch != f"release/mcp-cpp-sdk-{tag}":
        raise FormulaBranchError("formula branch does not match the source tag")
    pull = client.request("GET", f"/repos/{OWNER}/{REPOSITORY}/pulls/{pr}")
    head = _pull(pull, pr=pr, branch=branch)
    files = client.request(
        "GET", f"/repos/{OWNER}/{REPOSITORY}/pulls/{pr}/files?per_page=100"
    )
    _pull_files(files)
    if head != expected_head:
        commit = client.request("GET", f"/repos/{OWNER}/{REPOSITORY}/commits/{head}")
        _prior_commit(commit, head=head, expected=expected_head, tag=tag)
    return head


def _content(value: object) -> tuple[str, bytes]:
    if not isinstance(value, Mapping) or value.get("path") != FORMULA:
        raise FormulaBranchError("formula content response is malformed")
    blob_sha = value.get("sha")
    encoded = value.get("content")
    if SHA.fullmatch(str(blob_sha or "")) is None or not isinstance(encoded, str):
        raise FormulaBranchError("formula blob identity is malformed")
    try:
        compact = "".join(encoded.split())
        decoded = base64.b64decode(compact, validate=True)
    except (UnicodeError, ValueError) as error:
        raise FormulaBranchError("formula content is not canonical base64") from error
    if not decoded:
        raise FormulaBranchError("formula content is empty")
    return str(blob_sha), decoded


def update(
    client: Client,
    *,
    pr: str,
    branch: str,
    expected_head: str,
    tag: str,
    desired: bytes,
) -> str:
    current = verify_head(
        client, pr=pr, branch=branch, expected_head=expected_head, tag=tag
    )
    query = urllib.parse.urlencode({"ref": branch})
    file_value = client.request(
        "GET", f"/repos/{OWNER}/{REPOSITORY}/contents/{FORMULA}?{query}"
    )
    blob_sha, existing = _content(file_value)
    if current != expected_head:
        if existing != desired:
            raise FormulaBranchError("prior bottled formula differs from this publication bundle")
        return current
    result = client.request(
        "PUT",
        f"/repos/{OWNER}/{REPOSITORY}/contents/{FORMULA}",
        {
            "message": _message(tag),
            "content": base64.b64encode(desired).decode("ascii"),
            "sha": blob_sha,
            "branch": branch,
        },
    )
    commit = result.get("commit") if isinstance(result, Mapping) else None
    new_head = commit.get("sha") if isinstance(commit, Mapping) else None
    if not isinstance(new_head, str) or SHA.fullmatch(new_head) is None:
        raise FormulaBranchError("formula update did not return a commit SHA")
    confirmed = client.request("GET", f"/repos/{OWNER}/{REPOSITORY}/pulls/{pr}")
    if _pull(confirmed, pr=pr, branch=branch) != new_head:
        raise FormulaBranchError("formula pull request did not advance to the update")
    return new_head


def merge(client: Client, *, pr: str, expected_head: str, branch: str) -> None:
    pull = client.request("GET", f"/repos/{OWNER}/{REPOSITORY}/pulls/{pr}")
    if _pull(pull, pr=pr, branch=branch) != expected_head:
        raise FormulaBranchError("formula pull request head changed before merge")
    result = client.request(
        "PUT",
        f"/repos/{OWNER}/{REPOSITORY}/pulls/{pr}/merge",
        {"merge_method": "squash", "sha": expected_head},
    )
    if not isinstance(result, Mapping) or result.get("merged") is not True:
        raise FormulaBranchError("formula pull request was not merged")


def _client() -> Client:
    return Client(os.environ["GH_TOKEN"])


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("verify-head", "update", "merge"):
        command = commands.add_parser(name)
        command.add_argument("--pr", required=True)
        command.add_argument("--branch", required=True)
        command.add_argument("--expected-head", required=True)
        if name != "merge":
            command.add_argument("--tag", required=True)
        if name == "update":
            command.add_argument("--formula", type=Path, required=True)
            command.add_argument("--ledger", type=Path, required=True)
            command.add_argument("--output-ledger", type=Path, required=True)
            command.add_argument("--github-output", type=Path, required=True)
            command.add_argument("--run-id", required=True)
            command.add_argument("--run-attempt", required=True)
    args = parser.parse_args()
    try:
        if args.command == "verify-head":
            verify_head(
                _client(),
                pr=args.pr,
                branch=args.branch,
                expected_head=args.expected_head,
                tag=args.tag,
            )
        elif args.command == "update":
            desired = args.formula.read_bytes()
            head = update(
                _client(),
                pr=args.pr,
                branch=args.branch,
                expected_head=args.expected_head,
                tag=args.tag,
                desired=desired,
            )
            ledger = json.loads(args.ledger.read_text(encoding="ascii"))
            if (
                not isinstance(ledger, dict)
                or ledger.get("run_id") != args.run_id
                or DECIMAL.fullmatch(args.run_id) is None
                or DECIMAL.fullmatch(args.run_attempt) is None
            ):
                raise FormulaBranchError("GHCR ledger is malformed")
            ledger["bottled_head_sha"] = head
            ledger["publish_run_attempt"] = args.run_attempt
            args.output_ledger.write_text(
                json.dumps(ledger, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="ascii",
            )
            with args.github_output.open("a", encoding="utf-8") as output:
                output.write(f"bottled_head_sha={head}\n")
                output.write(f"publish_run_attempt={args.run_attempt}\n")
        else:
            merge(
                _client(),
                pr=args.pr,
                branch=args.branch,
                expected_head=args.expected_head,
            )
    except (
        FormulaBranchError,
        KeyError,
        OSError,
        UnicodeError,
        json.JSONDecodeError,
    ) as error:
        print(f"formula-branch: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
