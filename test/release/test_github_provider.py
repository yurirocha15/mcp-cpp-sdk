from __future__ import annotations

from pathlib import Path
import unittest
from unittest import mock

from release import github_provider
from release.github_provider import (
    GhApi,
    GitHubProviderError,
    RepositoryTarget,
    build_dispatch_inputs,
    control_from_contract,
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
CONTROL_TAG = "release-control-v3"
MANIFEST = "2" * 64


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


def publisher_api(
    target: RepositoryTarget,
    *,
    dispatch_response: object | None = None,
    control_sha: str = CONTROL_SHA,
) -> FakeApi:
    endpoint = f"repos/{target.full_name}/actions/workflows/publish.yml"
    return FakeApi(
        {
            ("GET", f"repos/{target.full_name}"): repository(target),
            ("GET", endpoint): {
                "id": 29,
                "path": ".github/workflows/publish.yml",
                "state": "active",
            },
            ("GET", f"repos/{target.full_name}/git/ref/tags/{CONTROL_TAG}"): {
                "ref": f"refs/tags/{CONTROL_TAG}",
                "object": {"type": "commit", "sha": control_sha},
            },
            ("POST", f"{endpoint}/dispatches"): dispatch_response,
        }
    )


class DispatchInputTests(unittest.TestCase):
    def test_workflow_cli_contract_parses_for_every_channel(self) -> None:
        common = [
            "--owner", "owner", "--repository", "publisher",
            "--expected-repository-id", "17", "--workflow", "publish.yml",
            "--source-repository", SOURCE, "--source-tag", TAG,
            "--source-commit-sha", COMMIT,
            "--source-workflow-head-sha", WORKFLOW_HEAD,
            "--contract", "release-publication-contract.json",
            "--github-release-id", "17", "--source-workflow-run-id", "23",
            "--release-manifest-sha256", MANIFEST,
            "--github-output", "github-output",
        ]
        for channel in ("homebrew", "chocolatey", "conan2"):
            with self.subTest(channel=channel):
                parsed = github_provider._parser().parse_args([channel, *common])
                self.assertEqual(parsed.channel, channel)

    def common(self, channel: str) -> dict[str, str]:
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

    def test_channels_share_one_exact_dispatch_contract(self) -> None:
        expected = {
            "source_tag",
            "source_commit_sha",
            "source_workflow_head_sha",
            "provider_control_sha",
            "github_release_id",
            "source_workflow_run_id",
            "release_manifest_sha256",
            "request_uuid",
        }
        for channel in ("homebrew", "chocolatey", "conan2"):
            with self.subTest(channel=channel):
                self.assertEqual(set(self.common(channel)), expected)

    def test_rejects_prerelease_malformed_and_cross_channel_fields(self) -> None:
        invalid = [
            {"source_tag": "v0.2.0-rc.1"},
            {"source_commit_sha": "A" * 40},
            {"github_release_id": "01"},
            {"release_manifest_sha256": "2" * 63},
            {"channel": "unsupported"},
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


class SignedControlTests(unittest.TestCase):
    def test_dispatch_control_comes_from_signed_contract(self) -> None:
        target = RepositoryTarget("owner", "publisher", 17)
        contract = mock.Mock(
            provider_controls={
                "homebrew": {
                    "repository": target.full_name,
                    "workflow": "publish.yml",
                    "control_tag": CONTROL_TAG,
                    "control_commit": CONTROL_SHA,
                }
            },
            destinations={
                "homebrew": {
                    "repository": target.full_name,
                    "repository_id": str(target.repository_id),
                }
            },
        )
        with mock.patch(
            "release.github_provider.load_publication_contract",
            return_value=contract,
        ):
            self.assertEqual(
                control_from_contract(
                    Path("contract.json"), channel="homebrew",
                    target=target, workflow="publish.yml",
                ),
                (CONTROL_TAG, CONTROL_SHA),
            )
            contract.destinations["homebrew"]["repository_id"] = "18"
            with self.assertRaises(GitHubProviderError):
                control_from_contract(
                    Path("contract.json"), channel="homebrew",
                    target=target, workflow="publish.yml",
                )


class ProviderIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.target = RepositoryTarget("owner", "publisher", 17)

    def test_validates_exact_repository_and_active_workflow_before_dispatch(self) -> None:
        run_id = 123
        response = {
            "workflow_run_id": run_id,
            "run_url": f"https://api.github.com/repos/{self.target.full_name}/actions/runs/{run_id}",
            "html_url": f"https://github.com/{self.target.full_name}/actions/runs/{run_id}",
        }
        api = publisher_api(self.target, dispatch_response=response)
        inputs = {
            "source_tag": TAG,
            "request_uuid": "one",
            "provider_control_sha": CONTROL_SHA,
        }
        receipt = dispatch_workflow(
            api,
            target=self.target,
            workflow="publish.yml",
            control_tag=CONTROL_TAG,
            control_sha=CONTROL_SHA,
            inputs=inputs,
        )
        self.assertEqual(receipt.run_id, str(run_id))
        self.assertEqual(
            api.calls[-1],
            (
                "POST",
                f"repos/{self.target.full_name}/actions/workflows/publish.yml/dispatches",
                {"ref": CONTROL_TAG, "inputs": inputs},
            ),
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
        with self.assertRaises(GitHubProviderError):
            dispatch_workflow(
                publisher_api(self.target, dispatch_response={}),
                target=self.target,
                workflow="publish.yml",
                control_tag=CONTROL_TAG,
                control_sha=CONTROL_SHA,
                inputs={"source_tag": TAG, "provider_control_sha": CONTROL_SHA},
            )

    def test_dispatch_accepts_and_binds_current_documented_run_response(self) -> None:
        run_id = 123
        response = {
            "workflow_run_id": run_id,
            "run_url": f"https://api.github.com/repos/{self.target.full_name}/actions/runs/{run_id}",
            "html_url": f"https://github.com/{self.target.full_name}/actions/runs/{run_id}",
        }
        receipt = dispatch_workflow(
            publisher_api(self.target, dispatch_response=response),
            target=self.target,
            workflow="publish.yml",
            control_tag=CONTROL_TAG,
            control_sha=CONTROL_SHA,
            inputs={"provider_control_sha": CONTROL_SHA},
        )
        self.assertEqual(receipt.run_id, str(run_id))
        self.assertEqual(receipt.html_url, response["html_url"])

    def test_dispatch_rejects_missing_or_mismatched_current_response(self) -> None:
        base = {
            "workflow_run_id": 123,
            "run_url": f"https://api.github.com/repos/{self.target.full_name}/actions/runs/123",
            "html_url": f"https://github.com/{self.target.full_name}/actions/runs/123",
        }
        receipt = dispatch_workflow(
            publisher_api(self.target, dispatch_response={**base, "added_later": "value"}),
            target=self.target,
            workflow="publish.yml",
            control_tag=CONTROL_TAG,
            control_sha=CONTROL_SHA,
            inputs={"provider_control_sha": CONTROL_SHA},
        )
        self.assertEqual(receipt.run_id, "123")
        for field in base:
            response = dict(base)
            del response[field]
            with self.subTest(missing=field), self.assertRaises(GitHubProviderError):
                dispatch_workflow(
                    publisher_api(self.target, dispatch_response=response),
                    target=self.target,
                    workflow="publish.yml",
                    control_tag=CONTROL_TAG,
                    control_sha=CONTROL_SHA,
                    inputs={"provider_control_sha": CONTROL_SHA},
                )
        for field, replacement in (
            ("workflow_run_id", 0),
            ("run_url", "https://attacker.invalid/run"),
            ("html_url", "https://attacker.invalid/run"),
        ):
            response = dict(base)
            response[field] = replacement
            with self.subTest(field=field), self.assertRaises(GitHubProviderError):
                dispatch_workflow(
                    publisher_api(self.target, dispatch_response=response),
                    target=self.target,
                    workflow="publish.yml",
                    control_tag=CONTROL_TAG,
                    control_sha=CONTROL_SHA,
                    inputs={"provider_control_sha": CONTROL_SHA},
                )

    def test_dispatch_rejects_changed_or_mismatched_control_tag(self) -> None:
        for inputs_sha, expected_sha in (
            (CONTROL_SHA, CONTROL_SHA),
            ("8" * 40, CONTROL_SHA),
        ):
            with self.subTest(inputs_sha=inputs_sha), self.assertRaises(
                GitHubProviderError
            ):
                dispatch_workflow(
                    publisher_api(self.target, control_sha="8" * 40),
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
