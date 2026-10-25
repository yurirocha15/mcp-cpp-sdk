from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest import mock

from release.github_provider import RepositoryTarget
from release.github_repository import GitHubProviderError
from release.provider_preflight import (
    ProviderPreflightError,
    main,
    preflight_workflow,
    require_selected_preflights,
)


class FakeApi:
    def __init__(self, responses):
        self.responses = dict(responses)
        self.calls = []

    def __call__(self, method, endpoint, payload=None):
        self.calls.append((method, endpoint, payload))
        key = (method, endpoint)
        if payload is not None or key not in self.responses:
            raise AssertionError(f"unexpected API request: {(method, endpoint, payload)}")
        return self.responses[key]


def publisher_api() -> FakeApi:
    return FakeApi(
        {
            ("GET", "repos/owner/publisher"): {
                "id": 11,
                "full_name": "owner/publisher",
                "default_branch": "main",
                "archived": False,
                "disabled": False,
            },
            ("GET", "repos/owner/publisher/actions/workflows/publish.yml"): {
                "id": 12,
                "path": ".github/workflows/publish.yml",
                "state": "active",
            },
        }
    )


class ProviderPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_generic_workflow_preflight_is_read_only(self) -> None:
        target = RepositoryTarget("owner", "publisher", 11)
        api = publisher_api()
        self.assertEqual(preflight_workflow(api, target=target, workflow="publish.yml"), 12)
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_gate_requires_selected_success_and_unselected_skip(self) -> None:
        targets = {"cloudsmith": True, "aur": False}
        require_selected_preflights(
            targets=targets,
            results={"cloudsmith": "success", "aur": "skipped"},
        )
        for results in (
            {"cloudsmith": "skipped", "aur": "skipped"},
            {"cloudsmith": "success", "aur": "success"},
            {"cloudsmith": "success"},
        ):
            with self.subTest(results=results), self.assertRaises(ProviderPreflightError):
                require_selected_preflights(targets=targets, results=results)

    def test_workflow_command_exports_only_the_verified_repository(self) -> None:
        output = Path(self.temporary.name) / "github-output"
        with mock.patch(
            "release.provider_preflight.GhApi", return_value=publisher_api()
        ):
            status = main(
                [
                    "workflow", "--owner", "owner", "--repository", "publisher",
                    "--expected-repository-id", "11", "--workflow", "publish.yml",
                    "--github-output", str(output),
                ]
            )
        self.assertEqual(status, 0)
        self.assertEqual(
            output.read_text(encoding="ascii"),
            "repository=owner/publisher\nrepository_id=11\n",
        )

    def test_workflow_preflight_rejects_another_default_branch(self) -> None:
        api = publisher_api()
        api.responses[("GET", "repos/owner/publisher")]["default_branch"] = "release"
        with self.assertRaises(GitHubProviderError):
            preflight_workflow(
                api,
                target=RepositoryTarget("owner", "publisher", 11),
                workflow="publish.yml",
            )


if __name__ == "__main__":
    unittest.main()
