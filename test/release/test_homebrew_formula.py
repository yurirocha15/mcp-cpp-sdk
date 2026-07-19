from __future__ import annotations

import base64
import hashlib
from pathlib import Path
import tempfile
import unittest

from release.github_provider import GitHubProviderError, RepositoryTarget
from release.homebrew_formula import prepare_formula_pull_request


TAG = "v0.2.0"
BRANCH = f"release/mcp-cpp-sdk-{TAG}"
HEAD = "1" * 40
BASE = "2" * 40
BASE_TREE = "3" * 40
TREE = "4" * 40
BOT = "mcp-cpp-sdk-homebrew-publisher[bot]"
FORMULA = b'class McpCppSdk < Formula\nend\n'


class ScriptedApi:
    def __init__(self, steps):
        self.steps = list(steps)
        self.calls = []

    def __call__(self, method, endpoint, payload=None):
        self.calls.append((method, endpoint, payload))
        if not self.steps:
            raise AssertionError(f"unexpected API request: {(method, endpoint, payload)}")
        expected_method, expected_endpoint, expected_payload, response = self.steps.pop(0)
        if (method, endpoint, payload) != (
            expected_method,
            expected_endpoint,
            expected_payload,
        ):
            raise AssertionError(
                f"request mismatch\nexpected: "
                f"{(expected_method, expected_endpoint, expected_payload)}\n"
                f"actual: {(method, endpoint, payload)}"
            )
        return response


def repo(target: RepositoryTarget) -> dict[str, object]:
    return {
        "id": target.repository_id,
        "full_name": target.full_name,
        "default_branch": "main",
        "archived": False,
        "disabled": False,
    }


def ref_endpoint(target: RepositoryTarget) -> str:
    return (
        f"repos/{target.full_name}/git/matching-refs/heads/"
        "release/mcp-cpp-sdk-v0.2.0"
    )


def refs(head: str = HEAD) -> list[dict[str, object]]:
    return [
        {
            "ref": f"refs/heads/{BRANCH}",
            "object": {"type": "commit", "sha": head},
        }
    ]


def encoded_formula(content: bytes = FORMULA) -> dict[str, object]:
    return {
        "type": "file",
        "path": "Formula/mcp-cpp-sdk.rb",
        "encoding": "base64",
        "content": base64.b64encode(content).decode("ascii"),
    }


def compare(head: str = HEAD, *, files=None) -> dict[str, object]:
    return {
        "ahead_by": 1,
        "total_commits": 1,
        "merge_base_commit": {"sha": BASE},
        "commits": [{"sha": head}],
        "files": files
        if files is not None
        else [{"filename": "Formula/mcp-cpp-sdk.rb", "status": "modified"}],
    }


def commit(head: str = HEAD) -> dict[str, object]:
    return {
        "sha": head,
        "message": f"release: prepare {TAG}",
        "parents": [{"sha": BASE}],
    }


def pull(target: RepositoryTarget, number: int = 7, *, login: str = BOT):
    return {
        "number": number,
        "state": "open",
        "draft": False,
        "title": f"mcp-cpp-sdk {TAG}",
        "body": f"Prepare the immutable {TAG} source for trusted bottle construction.",
        "base": {"ref": "main", "repo": {"full_name": target.full_name}},
        "head": {
            "ref": BRANCH,
            "sha": HEAD,
            "repo": {"full_name": target.full_name},
        },
        "user": {"type": "Bot", "login": login},
    }


class HomebrewFormulaTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.formula = Path(self.temporary.name) / "mcp-cpp-sdk.rb"
        self.formula.write_bytes(FORMULA)
        self.target = RepositoryTarget("owner", "homebrew-tap", 17)
        root = f"repos/{self.target.full_name}"
        self.root = root
        self.content_endpoint = (
            f"{root}/contents/Formula/mcp-cpp-sdk.rb?"
            "ref=release%2Fmcp-cpp-sdk-v0.2.0"
        )
        self.compare_endpoint = (
            f"{root}/compare/main...{HEAD}"
        )
        self.pulls_endpoint = (
            f"{root}/pulls?state=all&head=owner%3Arelease%2Fmcp-cpp-sdk-v0.2.0"
            "&base=main&per_page=100"
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def existing_steps(self, *, content=FORMULA, comparison=None, live_pull=None):
        return [
            ("GET", self.root, None, repo(self.target)),
            ("GET", ref_endpoint(self.target), None, refs()),
            ("GET", self.content_endpoint, None, encoded_formula(content)),
            (
                "GET",
                self.compare_endpoint,
                None,
                compare() if comparison is None else comparison,
            ),
            ("GET", f"{self.root}/git/commits/{HEAD}", None, commit()),
            ("GET", self.pulls_endpoint, None, [{"number": 7}]),
            (
                "GET",
                f"{self.root}/pulls/7",
                None,
                pull(self.target) if live_pull is None else live_pull,
            ),
            ("GET", ref_endpoint(self.target), None, refs()),
        ]

    def test_existing_identical_branch_and_pull_are_idempotent(self) -> None:
        api = ScriptedApi(self.existing_steps())
        result = prepare_formula_pull_request(
            api,
            target=self.target,
            tag=TAG,
            formula_path=self.formula,
            expected_bot_login=BOT,
        )
        self.assertEqual((result.number, result.branch, result.head_sha), (7, BRANCH, HEAD))
        self.assertEqual(api.steps, [])
        self.assertFalse(any(method == "POST" for method, _endpoint, _body in api.calls))

    def test_creates_exact_single_commit_branch_and_pull(self) -> None:
        blob_sha = hashlib.sha1(
            f"blob {len(FORMULA)}\0".encode("ascii") + FORMULA,
            usedforsecurity=False,
        ).hexdigest()
        root = self.root
        steps = [
            ("GET", root, None, repo(self.target)),
            ("GET", ref_endpoint(self.target), None, []),
            (
                "GET",
                f"{root}/git/ref/heads/main",
                None,
                {
                    "ref": "refs/heads/main",
                    "object": {"type": "commit", "sha": BASE},
                },
            ),
            (
                "GET",
                f"{root}/git/commits/{BASE}",
                None,
                {"sha": BASE, "tree": {"sha": BASE_TREE}},
            ),
            (
                "POST",
                f"{root}/git/blobs",
                {
                    "content": base64.b64encode(FORMULA).decode("ascii"),
                    "encoding": "base64",
                },
                {"sha": blob_sha},
            ),
            (
                "POST",
                f"{root}/git/trees",
                {
                    "base_tree": BASE_TREE,
                    "tree": [
                        {
                            "path": "Formula/mcp-cpp-sdk.rb",
                            "mode": "100644",
                            "type": "blob",
                            "sha": blob_sha,
                        }
                    ],
                },
                {"sha": TREE},
            ),
            (
                "POST",
                f"{root}/git/commits",
                {
                    "message": f"release: prepare {TAG}",
                    "tree": TREE,
                    "parents": [BASE],
                },
                {
                    "sha": HEAD,
                    "message": f"release: prepare {TAG}",
                    "tree": {"sha": TREE},
                    "parents": [{"sha": BASE}],
                },
            ),
            (
                "POST",
                f"{root}/git/refs",
                {"ref": f"refs/heads/{BRANCH}", "sha": HEAD},
                {
                    "ref": f"refs/heads/{BRANCH}",
                    "object": {"type": "commit", "sha": HEAD},
                },
            ),
            ("GET", ref_endpoint(self.target), None, refs()),
            ("GET", self.content_endpoint, None, encoded_formula()),
            ("GET", self.compare_endpoint, None, compare()),
            ("GET", f"{root}/git/commits/{HEAD}", None, commit()),
            ("GET", self.pulls_endpoint, None, []),
            (
                "POST",
                f"{root}/pulls",
                {
                    "title": f"mcp-cpp-sdk {TAG}",
                    "head": BRANCH,
                    "base": "main",
                    "body": (
                        f"Prepare the immutable {TAG} source for trusted bottle construction."
                    ),
                },
                {"number": 7},
            ),
            ("GET", f"{root}/pulls/7", None, pull(self.target)),
            ("GET", ref_endpoint(self.target), None, refs()),
        ]
        api = ScriptedApi(steps)
        result = prepare_formula_pull_request(
            api,
            target=self.target,
            tag=TAG,
            formula_path=self.formula,
            expected_bot_login=BOT,
        )
        self.assertEqual(result.head_sha, HEAD)
        self.assertEqual(api.steps, [])

    def test_rejects_conflicting_formula_bytes_before_any_mutation(self) -> None:
        api = ScriptedApi(self.existing_steps(content=b"conflict\n")[:3])
        with self.assertRaises(GitHubProviderError):
            prepare_formula_pull_request(
                api,
                target=self.target,
                tag=TAG,
                formula_path=self.formula,
                expected_bot_login=BOT,
            )
        self.assertFalse(any(method == "POST" for method, _endpoint, _body in api.calls))

    def test_rejects_ambiguous_branch_identity(self) -> None:
        duplicate = refs() + refs()
        api = ScriptedApi(
            [
                ("GET", self.root, None, repo(self.target)),
                ("GET", ref_endpoint(self.target), None, duplicate),
            ]
        )
        with self.assertRaises(GitHubProviderError):
            prepare_formula_pull_request(
                api,
                target=self.target,
                tag=TAG,
                formula_path=self.formula,
                expected_bot_login=BOT,
            )

    def test_rejects_extra_branch_paths_and_wrong_bot(self) -> None:
        extra = compare(
            files=[
                {"filename": "Formula/mcp-cpp-sdk.rb", "status": "modified"},
                {"filename": "README.md", "status": "modified"},
            ]
        )
        api = ScriptedApi(self.existing_steps(comparison=extra)[:4])
        with self.assertRaises(GitHubProviderError):
            prepare_formula_pull_request(
                api,
                target=self.target,
                tag=TAG,
                formula_path=self.formula,
                expected_bot_login=BOT,
            )

        wrong_pull = pull(self.target, login="different-app[bot]")
        api = ScriptedApi(self.existing_steps(live_pull=wrong_pull)[:-1])
        with self.assertRaises(GitHubProviderError):
            prepare_formula_pull_request(
                api,
                target=self.target,
                tag=TAG,
                formula_path=self.formula,
                expected_bot_login=BOT,
            )

    def test_rejects_ref_race_after_pull_verification(self) -> None:
        steps = self.existing_steps()
        steps[-1] = (
            "GET",
            ref_endpoint(self.target),
            None,
            refs("9" * 40),
        )
        api = ScriptedApi(steps)
        with self.assertRaises(GitHubProviderError):
            prepare_formula_pull_request(
                api,
                target=self.target,
                tag=TAG,
                formula_path=self.formula,
                expected_bot_login=BOT,
            )

    def test_rejects_symlink_and_prerelease_without_api_calls(self) -> None:
        symlink = self.formula.with_name("formula-link.rb")
        symlink.symlink_to(self.formula)
        for path, tag in ((symlink, TAG), (self.formula, "v0.2.0-rc.1")):
            api = ScriptedApi([])
            with self.subTest(path=path, tag=tag), self.assertRaises(
                (GitHubProviderError, ValueError)
            ):
                prepare_formula_pull_request(
                    api,
                    target=self.target,
                    tag=tag,
                    formula_path=path,
                    expected_bot_login=BOT,
                )
            self.assertEqual(api.calls, [])


if __name__ == "__main__":
    unittest.main()
