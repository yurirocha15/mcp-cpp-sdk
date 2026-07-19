#!/usr/bin/env python3
"""Create or verify the one immutable Homebrew formula pull request."""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import subprocess
import sys
from typing import Mapping, Sequence
from urllib.parse import urlencode

from .github_provider import (
    ApiRequest,
    GhApi,
    GitHubProviderError,
    RepositoryTarget,
    validate_repository,
)
from .model import SemVer, ValidationError


_SHA1 = re.compile(r"[0-9a-f]{40}")
_BOT_LOGIN = re.compile(r"[A-Za-z0-9][A-Za-z0-9-]{0,79}\[bot\]")
_FORMULA_PATH = "Formula/mcp-cpp-sdk.rb"
_MAX_FORMULA_BYTES = 1_000_000


@dataclass(frozen=True)
class FormulaPullRequest:
    number: int
    branch: str
    head_sha: str


def _mapping(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise GitHubProviderError(f"{label} is not a JSON object")
    return value


def _sha(value: object, label: str) -> str:
    if not isinstance(value, str) or _SHA1.fullmatch(value) is None:
        raise GitHubProviderError(f"{label} is not a lowercase full SHA")
    return value


def _ref_endpoint(target: RepositoryTarget, branch: str) -> str:
    return f"repos/{target.full_name}/git/matching-refs/heads/{branch}"


def _exact_branch_head(value: object, branch: str) -> str | None:
    if not isinstance(value, list) or any(not isinstance(item, Mapping) for item in value):
        raise GitHubProviderError("matching branch refs response is malformed")
    expected_ref = f"refs/heads/{branch}"
    exact = [item for item in value if item.get("ref") == expected_ref]
    if len(exact) > 1:
        raise GitHubProviderError("Homebrew formula branch identity is ambiguous")
    if not exact:
        return None
    ref_object = _mapping(exact[0].get("object"), "formula branch ref object")
    if ref_object.get("type") != "commit":
        raise GitHubProviderError("formula branch does not point to a commit")
    return _sha(ref_object.get("sha"), "formula branch head")


def _base_commit(api: ApiRequest, target: RepositoryTarget) -> tuple[str, str]:
    value = _mapping(
        api("GET", f"repos/{target.full_name}/git/ref/heads/{target.default_branch}"),
        "default branch ref",
    )
    ref_object = _mapping(value.get("object"), "default branch ref object")
    if (
        value.get("ref") != f"refs/heads/{target.default_branch}"
        or ref_object.get("type") != "commit"
    ):
        raise GitHubProviderError("default branch ref is not an exact commit ref")
    commit_sha = _sha(ref_object.get("sha"), "default branch head")
    commit = _mapping(
        api("GET", f"repos/{target.full_name}/git/commits/{commit_sha}"),
        "default branch commit",
    )
    if commit.get("sha") != commit_sha:
        raise GitHubProviderError("default branch commit response changed identity")
    tree = _mapping(commit.get("tree"), "default branch tree")
    return commit_sha, _sha(tree.get("sha"), "default branch tree")


def _git_blob_sha(content: bytes) -> str:
    header = f"blob {len(content)}\0".encode("ascii")
    return hashlib.sha1(header + content, usedforsecurity=False).hexdigest()


def _create_formula_branch(
    api: ApiRequest,
    target: RepositoryTarget,
    *,
    branch: str,
    tag: str,
    formula: bytes,
) -> str:
    base_sha, base_tree = _base_commit(api, target)
    encoded = base64.b64encode(formula).decode("ascii")
    blob = _mapping(
        api(
            "POST",
            f"repos/{target.full_name}/git/blobs",
            {"content": encoded, "encoding": "base64"},
        ),
        "created formula blob",
    )
    blob_sha = _sha(blob.get("sha"), "created formula blob")
    if blob_sha != _git_blob_sha(formula):
        raise GitHubProviderError("created formula blob digest differs from local bytes")
    tree = _mapping(
        api(
            "POST",
            f"repos/{target.full_name}/git/trees",
            {
                "base_tree": base_tree,
                "tree": [
                    {
                        "path": _FORMULA_PATH,
                        "mode": "100644",
                        "type": "blob",
                        "sha": blob_sha,
                    }
                ],
            },
        ),
        "created formula tree",
    )
    tree_sha = _sha(tree.get("sha"), "created formula tree")
    message = f"release: prepare {tag}"
    commit = _mapping(
        api(
            "POST",
            f"repos/{target.full_name}/git/commits",
            {"message": message, "tree": tree_sha, "parents": [base_sha]},
        ),
        "created formula commit",
    )
    head = _sha(commit.get("sha"), "created formula commit")
    commit_tree = _mapping(commit.get("tree"), "created formula commit tree")
    parents = commit.get("parents")
    if (
        commit.get("message") != message
        or commit_tree.get("sha") != tree_sha
        or not isinstance(parents, list)
        or len(parents) != 1
        or not isinstance(parents[0], Mapping)
        or parents[0].get("sha") != base_sha
    ):
        raise GitHubProviderError("created formula commit differs from the requested commit")
    ref = _mapping(
        api(
            "POST",
            f"repos/{target.full_name}/git/refs",
            {"ref": f"refs/heads/{branch}", "sha": head},
        ),
        "created formula ref",
    )
    ref_object = _mapping(ref.get("object"), "created formula ref object")
    if (
        ref.get("ref") != f"refs/heads/{branch}"
        or ref_object.get("type") != "commit"
        or ref_object.get("sha") != head
    ):
        raise GitHubProviderError("created formula ref differs from the requested ref")
    live_head = _exact_branch_head(api("GET", _ref_endpoint(target, branch)), branch)
    if live_head != head:
        raise GitHubProviderError("formula branch changed while it was being created")
    return head


def _verify_formula_content(
    api: ApiRequest,
    target: RepositoryTarget,
    *,
    branch: str,
    formula: bytes,
) -> None:
    endpoint = (
        f"repos/{target.full_name}/contents/{_FORMULA_PATH}?"
        + urlencode({"ref": branch})
    )
    value = _mapping(api("GET", endpoint), "remote Homebrew formula")
    if (
        value.get("type") != "file"
        or value.get("path") != _FORMULA_PATH
        or value.get("encoding") != "base64"
    ):
        raise GitHubProviderError("remote Homebrew formula identity is malformed")
    content = value.get("content")
    if not isinstance(content, str):
        raise GitHubProviderError("remote Homebrew formula has no encoded content")
    try:
        remote = base64.b64decode("".join(content.splitlines()), validate=True)
    except ValueError as error:
        raise GitHubProviderError("remote Homebrew formula is not canonical base64") from error
    if remote != formula:
        raise GitHubProviderError("existing Homebrew formula branch has conflicting bytes")


def _verify_formula_commit(
    api: ApiRequest,
    target: RepositoryTarget,
    *,
    branch: str,
    tag: str,
    head: str,
) -> None:
    compare = _mapping(
        api(
            "GET",
            f"repos/{target.full_name}/compare/{target.default_branch}..."
            f"{head}",
        ),
        "formula branch comparison",
    )
    commits = compare.get("commits")
    files = compare.get("files")
    merge_base = _mapping(compare.get("merge_base_commit"), "formula merge base")
    if (
        compare.get("ahead_by") != 1
        or compare.get("total_commits") != 1
        or not isinstance(commits, list)
        or len(commits) != 1
        or not isinstance(commits[0], Mapping)
        or commits[0].get("sha") != head
        or not isinstance(files, list)
        or len(files) != 1
        or not isinstance(files[0], Mapping)
        or files[0].get("filename") != _FORMULA_PATH
        or files[0].get("status") not in {"added", "modified"}
    ):
        raise GitHubProviderError("formula branch contains an unexpected commit or path")
    merge_base_sha = _sha(merge_base.get("sha"), "formula branch merge base")
    commit = _mapping(
        api("GET", f"repos/{target.full_name}/git/commits/{head}"),
        "formula branch commit",
    )
    parents = commit.get("parents")
    if (
        commit.get("sha") != head
        or commit.get("message") != f"release: prepare {tag}"
        or not isinstance(parents, list)
        or len(parents) != 1
        or not isinstance(parents[0], Mapping)
        or parents[0].get("sha") != merge_base_sha
    ):
        raise GitHubProviderError("formula branch commit is not the exact release commit")


def _verify_pull(
    value: object,
    *,
    target: RepositoryTarget,
    tag: str,
    branch: str,
    head: str,
    expected_bot_login: str,
) -> int:
    pull = _mapping(value, "formula pull request")
    number = pull.get("number")
    base = _mapping(pull.get("base"), "formula pull request base")
    pull_head = _mapping(pull.get("head"), "formula pull request head")
    base_repo = _mapping(base.get("repo"), "formula pull request base repository")
    head_repo = _mapping(pull_head.get("repo"), "formula pull request head repository")
    user = _mapping(pull.get("user"), "formula pull request author")
    expected_body = (
        f"Prepare the immutable {tag} source for trusted bottle construction."
    )
    if (
        type(number) is not int
        or number < 1
        or pull.get("state") != "open"
        or pull.get("draft") is not False
        or pull.get("title") != f"mcp-cpp-sdk {tag}"
        or pull.get("body") != expected_body
        or base.get("ref") != target.default_branch
        or base_repo.get("full_name") != target.full_name
        or pull_head.get("ref") != branch
        or pull_head.get("sha") != head
        or head_repo.get("full_name") != target.full_name
        or user.get("type") != "Bot"
        or user.get("login") != expected_bot_login
    ):
        raise GitHubProviderError("formula pull request identity is not exact")
    return number


def _create_or_verify_pull(
    api: ApiRequest,
    target: RepositoryTarget,
    *,
    tag: str,
    branch: str,
    head: str,
    expected_bot_login: str,
) -> int:
    query = urlencode(
        {
            "state": "all",
            "head": f"{target.owner}:{branch}",
            "base": target.default_branch,
            "per_page": "100",
        }
    )
    pulls = api("GET", f"repos/{target.full_name}/pulls?{query}")
    if not isinstance(pulls, list) or any(not isinstance(item, Mapping) for item in pulls):
        raise GitHubProviderError("formula pull request listing is malformed")
    if len(pulls) > 1:
        raise GitHubProviderError("formula pull request identity is ambiguous")
    if not pulls:
        created = _mapping(
            api(
                "POST",
                f"repos/{target.full_name}/pulls",
                {
                    "title": f"mcp-cpp-sdk {tag}",
                    "head": branch,
                    "base": target.default_branch,
                    "body": (
                        f"Prepare the immutable {tag} source for trusted bottle construction."
                    ),
                },
            ),
            "created formula pull request",
        )
        number = created.get("number")
        if type(number) is not int or number < 1:
            raise GitHubProviderError("created formula pull request has no numeric identity")
    else:
        number = pulls[0].get("number")
        if type(number) is not int or number < 1:
            raise GitHubProviderError("existing formula pull request has no numeric identity")
    live = api("GET", f"repos/{target.full_name}/pulls/{number}")
    return _verify_pull(
        live,
        target=target,
        tag=tag,
        branch=branch,
        head=head,
        expected_bot_login=expected_bot_login,
    )


def prepare_formula_pull_request(
    api: ApiRequest,
    *,
    target: RepositoryTarget,
    tag: str,
    formula_path: Path,
    expected_bot_login: str,
) -> FormulaPullRequest:
    SemVer.from_tag(tag, stable_only=True)
    if _BOT_LOGIN.fullmatch(expected_bot_login) is None:
        raise GitHubProviderError("expected Homebrew App bot login is not canonical")
    if formula_path.is_symlink() or not formula_path.is_file():
        raise GitHubProviderError("Homebrew formula input is missing or unsafe")
    formula = formula_path.read_bytes()
    if not formula or len(formula) > _MAX_FORMULA_BYTES:
        raise GitHubProviderError("Homebrew formula input size is invalid")
    try:
        formula.decode("utf-8")
    except UnicodeDecodeError as error:
        raise GitHubProviderError("Homebrew formula input is not UTF-8") from error
    validate_repository(api, target)
    branch = f"release/mcp-cpp-sdk-{tag}"
    head = _exact_branch_head(api("GET", _ref_endpoint(target, branch)), branch)
    if head is None:
        head = _create_formula_branch(
            api, target, branch=branch, tag=tag, formula=formula
        )
    _verify_formula_content(api, target, branch=branch, formula=formula)
    _verify_formula_commit(api, target, branch=branch, tag=tag, head=head)
    number = _create_or_verify_pull(
        api,
        target,
        tag=tag,
        branch=branch,
        head=head,
        expected_bot_login=expected_bot_login,
    )
    if _exact_branch_head(api("GET", _ref_endpoint(target, branch)), branch) != head:
        raise GitHubProviderError("formula branch changed while the pull request was prepared")
    return FormulaPullRequest(number=number, branch=branch, head_sha=head)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--expected-repository-id", type=int, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--formula", type=Path, required=True)
    parser.add_argument("--expected-bot-login", required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = prepare_formula_pull_request(
            GhApi(),
            target=RepositoryTarget(
                args.owner, args.repository, args.expected_repository_id
            ),
            tag=args.tag,
            formula_path=args.formula,
            expected_bot_login=args.expected_bot_login,
        )
        with args.github_output.open("a", encoding="ascii") as output:
            output.write(f"formula_pr_number={result.number}\n")
            output.write(f"formula_branch={result.branch}\n")
            output.write(f"formula_head_sha={result.head_sha}\n")
    except (
        GitHubProviderError,
        ValidationError,
        OSError,
        UnicodeError,
        subprocess.SubprocessError,
    ) as error:
        print(f"homebrew-formula: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
