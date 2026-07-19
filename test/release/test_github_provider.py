from __future__ import annotations

import unittest

from release.github_provider import (
    GhApi,
    GitHubProviderError,
    RepositoryTarget,
    build_dispatch_inputs,
    deterministic_request_uuid,
    dispatch_workflow,
    validate_repository,
    validate_workflow,
)


SOURCE = "yurirocha15/mcp-cpp-sdk"
TAG = "v0.2.0"
COMMIT = "1" * 40
WORKFLOW_HEAD = "6" * 40
CONTROL_SHA = "7" * 40
CONTROL_TAG = "release-control-v1"
MANIFEST = "2" * 64
FORMULA_HEAD = "3" * 40
CONAN_HEAD = "4" * 40
TREE = "5" * 64


class FakeApi:
    def __init__(self, responses: dict[tuple[str, str], object | None]) -> None:
        self.responses = responses
        self.calls: list[tuple[str, str, object | None]] = []

    def __call__(self, method: str, endpoint: str, payload=None):
        self.calls.append((method, endpoint, payload))
        key = (method, endpoint)
        if key not in self.responses:
            raise AssertionError(f"unexpected API request: {key}")
        return self.responses[key]


def repository(target: RepositoryTarget) -> dict[str, object]:
    return {
        "id": target.repository_id,
        "full_name": target.full_name,
        "default_branch": "main",
        "archived": False,
        "disabled": False,
    }


class DispatchInputTests(unittest.TestCase):
    def common(self, channel: str, **extra: str) -> dict[str, str]:
        return build_dispatch_inputs(
            channel=channel,
            source_repository=SOURCE,
            source_tag=TAG,
            source_commit_sha=COMMIT,
            source_workflow_head_sha=WORKFLOW_HEAD,
            provider_control_sha=CONTROL_SHA,
            github_release_id="17",
            source_workflow_run_id="23",
            release_manifest_sha256=MANIFEST,
            **extra,
        )

    def test_request_uuid_is_stable_and_channel_scoped(self) -> None:
        homebrew = deterministic_request_uuid(
            source_repository=SOURCE,
            source_tag=TAG,
            manifest_sha256=MANIFEST,
            channel="homebrew",
        )
        self.assertEqual(homebrew, "97673f66-28d2-5a16-ad5d-e159f138b983")
        self.assertEqual(
            homebrew,
            deterministic_request_uuid(
                source_repository=SOURCE,
                source_tag=TAG,
                manifest_sha256=MANIFEST,
                channel="homebrew",
            ),
        )
        self.assertNotEqual(
            homebrew,
            deterministic_request_uuid(
                source_repository=SOURCE,
                source_tag=TAG,
                manifest_sha256=MANIFEST,
                channel="chocolatey",
            ),
        )

    def test_builds_exact_homebrew_contract(self) -> None:
        value = self.common(
            "homebrew",
            formula_pr_number="7",
            formula_branch=f"release/mcp-cpp-sdk-{TAG}",
            formula_head_sha=FORMULA_HEAD,
        )
        self.assertEqual(
            set(value),
            {
                "source_tag",
                "source_commit_sha",
                "source_workflow_head_sha",
                "provider_control_sha",
                "github_release_id",
                "source_workflow_run_id",
                "release_manifest_sha256",
                "formula_pr_number",
                "formula_branch",
                "formula_head_sha",
                "request_uuid",
            },
        )

    def test_builds_exact_chocolatey_and_conan_contracts(self) -> None:
        chocolatey = self.common("chocolatey")
        self.assertEqual(
            set(chocolatey),
            {
                "source_tag",
                "source_commit_sha",
                "source_workflow_head_sha",
                "provider_control_sha",
                "github_release_id",
                "source_workflow_run_id",
                "release_manifest_sha256",
                "request_uuid",
            },
        )
        conan = self.common(
            "conan2",
            fork_branch="package/mcp-cpp-sdk-0.2.0",
            fork_head_sha=CONAN_HEAD,
            recipe_tree_sha256=TREE,
        )
        self.assertEqual(
            set(conan) - set(chocolatey),
            {"fork_branch", "fork_head_sha", "recipe_tree_sha256"},
        )

    def test_rejects_prerelease_malformed_and_cross_channel_fields(self) -> None:
        invalid = [
            {"source_tag": "v0.2.0-rc.1"},
            {"source_commit_sha": "A" * 40},
            {"github_release_id": "01"},
            {"release_manifest_sha256": "2" * 63},
            {"formula_branch": "release/mcp-cpp-sdk-v0.2.0"},
        ]
        for changes in invalid:
            arguments = {
                "channel": "chocolatey",
                "source_repository": SOURCE,
                "source_tag": TAG,
                "source_commit_sha": COMMIT,
                "source_workflow_head_sha": WORKFLOW_HEAD,
                "provider_control_sha": CONTROL_SHA,
                "github_release_id": "17",
                "source_workflow_run_id": "23",
                "release_manifest_sha256": MANIFEST,
            }
            arguments.update(changes)
            with self.subTest(changes=changes), self.assertRaises(
                (GitHubProviderError, ValueError)
            ):
                build_dispatch_inputs(**arguments)


class ProviderIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.target = RepositoryTarget("owner", "publisher", 17)

    def test_validates_exact_repository_and_active_workflow_before_dispatch(self) -> None:
        workflow = "publish.yml"
        dispatch_endpoint = (
            f"repos/{self.target.full_name}/actions/workflows/{workflow}/dispatches"
        )
        api = FakeApi(
            {
                ("GET", f"repos/{self.target.full_name}"): repository(self.target),
                (
                    "GET",
                    f"repos/{self.target.full_name}/actions/workflows/{workflow}",
                ): {
                    "id": 29,
                    "path": f".github/workflows/{workflow}",
                    "state": "active",
                },
                ("POST", dispatch_endpoint): None,
                (
                    "GET",
                    f"repos/{self.target.full_name}/git/ref/tags/{CONTROL_TAG}",
                ): {
                    "ref": f"refs/tags/{CONTROL_TAG}",
                    "object": {"type": "commit", "sha": CONTROL_SHA},
                },
            }
        )
        inputs = {
            "source_tag": TAG,
            "request_uuid": "one",
            "provider_control_sha": CONTROL_SHA,
        }
        self.assertIsNone(dispatch_workflow(
            api,
            target=self.target,
            workflow=workflow,
            control_tag=CONTROL_TAG,
            control_sha=CONTROL_SHA,
            inputs=inputs,
        ))
        self.assertEqual(
            api.calls[-1],
            ("POST", dispatch_endpoint, {"ref": CONTROL_TAG, "inputs": inputs}),
        )

    def test_repository_identity_checks_fail_closed(self) -> None:
        base = repository(self.target)
        mutations = {
            "id": 18,
            "full_name": "owner/other",
            "default_branch": "master",
            "archived": True,
            "disabled": True,
        }
        for field, replacement in mutations.items():
            value = dict(base)
            value[field] = replacement
            api = FakeApi({("GET", f"repos/{self.target.full_name}"): value})
            with self.subTest(field=field), self.assertRaises(GitHubProviderError):
                validate_repository(api, self.target)

    def test_workflow_identity_checks_fail_closed(self) -> None:
        endpoint = f"repos/{self.target.full_name}/actions/workflows/publish.yml"
        invalid = (
            {"id": 0, "path": ".github/workflows/publish.yml", "state": "active"},
            {"id": 1, "path": ".github/workflows/other.yml", "state": "active"},
            {"id": 1, "path": ".github/workflows/publish.yml", "state": "disabled_manually"},
        )
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(GitHubProviderError):
                validate_workflow(FakeApi({("GET", endpoint): value}), self.target, "publish.yml")

    def test_dispatch_never_treats_a_response_body_as_success(self) -> None:
        endpoint = f"repos/{self.target.full_name}/actions/workflows/publish.yml"
        api = FakeApi(
            {
                ("GET", f"repos/{self.target.full_name}"): repository(self.target),
                ("GET", endpoint): {
                    "id": 1,
                    "path": ".github/workflows/publish.yml",
                    "state": "active",
                },
                ("POST", f"{endpoint}/dispatches"): {},
                (
                    "GET",
                    f"repos/{self.target.full_name}/git/ref/tags/{CONTROL_TAG}",
                ): {
                    "ref": f"refs/tags/{CONTROL_TAG}",
                    "object": {"type": "commit", "sha": CONTROL_SHA},
                },
            }
        )
        with self.assertRaises(GitHubProviderError):
            dispatch_workflow(
                api,
                target=self.target,
                workflow="publish.yml",
                control_tag=CONTROL_TAG,
                control_sha=CONTROL_SHA,
                inputs={"source_tag": TAG, "provider_control_sha": CONTROL_SHA},
            )

    def test_dispatch_accepts_and_binds_current_documented_run_response(self) -> None:
        workflow = "publish.yml"
        endpoint = f"repos/{self.target.full_name}/actions/workflows/{workflow}"
        run_id = 123
        response = {
            "workflow_run_id": run_id,
            "run_url": (
                f"https://api.github.com/repos/{self.target.full_name}/actions/runs/{run_id}"
            ),
            "html_url": (
                f"https://github.com/{self.target.full_name}/actions/runs/{run_id}"
            ),
        }
        api = FakeApi(
            {
                ("GET", f"repos/{self.target.full_name}"): repository(self.target),
                ("GET", endpoint): {
                    "id": 1,
                    "path": f".github/workflows/{workflow}",
                    "state": "active",
                },
                (
                    "GET",
                    f"repos/{self.target.full_name}/git/ref/tags/{CONTROL_TAG}",
                ): {
                    "ref": f"refs/tags/{CONTROL_TAG}",
                    "object": {"type": "commit", "sha": CONTROL_SHA},
                },
                ("POST", f"{endpoint}/dispatches"): response,
            }
        )
        self.assertEqual(
            dispatch_workflow(
                api,
                target=self.target,
                workflow=workflow,
                control_tag=CONTROL_TAG,
                control_sha=CONTROL_SHA,
                inputs={"provider_control_sha": CONTROL_SHA},
            ),
            str(run_id),
        )

    def test_dispatch_rejects_extra_or_mismatched_current_response(self) -> None:
        workflow = "publish.yml"
        endpoint = f"repos/{self.target.full_name}/actions/workflows/{workflow}"
        base_response = {
            "workflow_run_id": 123,
            "run_url": (
                f"https://api.github.com/repos/{self.target.full_name}/actions/runs/123"
            ),
            "html_url": f"https://github.com/{self.target.full_name}/actions/runs/123",
        }
        for mutation in ("extra", "run_id", "run_url", "html_url"):
            response = dict(base_response)
            if mutation == "extra":
                response["unexpected"] = "value"
            elif mutation == "run_id":
                response["workflow_run_id"] = 0
            else:
                response[mutation] = "https://attacker.invalid/run"
            api = FakeApi(
                {
                    ("GET", f"repos/{self.target.full_name}"): repository(self.target),
                    ("GET", endpoint): {
                        "id": 1,
                        "path": f".github/workflows/{workflow}",
                        "state": "active",
                    },
                    (
                        "GET",
                        f"repos/{self.target.full_name}/git/ref/tags/{CONTROL_TAG}",
                    ): {
                        "ref": f"refs/tags/{CONTROL_TAG}",
                        "object": {"type": "commit", "sha": CONTROL_SHA},
                    },
                    ("POST", f"{endpoint}/dispatches"): response,
                }
            )
            with self.subTest(mutation=mutation), self.assertRaises(
                GitHubProviderError
            ):
                dispatch_workflow(
                    api,
                    target=self.target,
                    workflow=workflow,
                    control_tag=CONTROL_TAG,
                    control_sha=CONTROL_SHA,
                    inputs={"provider_control_sha": CONTROL_SHA},
                )

    def test_dispatch_rejects_changed_or_mismatched_control_tag(self) -> None:
        endpoint = f"repos/{self.target.full_name}/actions/workflows/publish.yml"
        base = {
            ("GET", f"repos/{self.target.full_name}"): repository(self.target),
            ("GET", endpoint): {
                "id": 1,
                "path": ".github/workflows/publish.yml",
                "state": "active",
            },
            (
                "GET",
                f"repos/{self.target.full_name}/git/ref/tags/{CONTROL_TAG}",
            ): {
                "ref": f"refs/tags/{CONTROL_TAG}",
                "object": {"type": "commit", "sha": "8" * 40},
            },
        }
        for inputs_sha, expected_sha in (
            (CONTROL_SHA, CONTROL_SHA),
            ("8" * 40, CONTROL_SHA),
        ):
            with self.subTest(inputs_sha=inputs_sha), self.assertRaises(
                GitHubProviderError
            ):
                dispatch_workflow(
                    FakeApi(base),
                    target=self.target,
                    workflow="publish.yml",
                    control_tag=CONTROL_TAG,
                    control_sha=expected_sha,
                    inputs={"provider_control_sha": inputs_sha},
                )

    def test_target_and_workflow_coordinates_reject_path_injection(self) -> None:
        for owner, repo in (("../owner", "repo"), ("owner", "repo/name")):
            with self.subTest(owner=owner, repo=repo), self.assertRaises(
                GitHubProviderError
            ):
                RepositoryTarget(owner, repo, 1)
        with self.assertRaises(GitHubProviderError):
            validate_workflow(lambda *_: None, self.target, "../publish.yml")


class GhApiTests(unittest.TestCase):
    def test_uses_no_shell_or_credential_argument_and_canonical_json(self) -> None:
        calls: list[tuple[list[str], bytes | None]] = []

        def runner(arguments, stdin):
            calls.append((list(arguments), stdin))
            return b'{"state":"active"}\n'

        value = GhApi(runner)("POST", "repos/owner/repo/example", {"z": 1, "a": "x"})
        self.assertEqual(value, {"state": "active"})
        self.assertEqual(
            calls,
            [
                (
                    [
                        "gh",
                        "api",
                        "--method",
                        "POST",
                        "-H",
                        "X-GitHub-Api-Version: 2026-03-10",
                        "repos/owner/repo/example",
                        "--input",
                        "-",
                    ],
                    b'{"a":"x","z":1}\n',
                )
            ],
        )
        self.assertNotIn("token", " ".join(calls[0][0]).lower())

    def test_rejects_duplicate_json_fields(self) -> None:
        with self.assertRaises(GitHubProviderError):
            GhApi(lambda _arguments, _stdin: b'{"id":1,"id":2}')(
                "GET", "repos/owner/repo"
            )


if __name__ == "__main__":
    unittest.main()
