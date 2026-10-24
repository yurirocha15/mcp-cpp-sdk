from __future__ import annotations

import copy
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock

from release.github_provider import RepositoryTarget
from release.provider_preflight import (
    ProviderPreflightError,
    _control_manifest,
    preflight_workflow,
    require_selected_preflights,
    verify_control_snapshot,
    verify_embedded_control_snapshot,
)


HEAD = "a" * 40
CONTROL_TAG = "release-control-v2"
WORKFLOWS = {
    "homebrew": "publish-bottles.yml",
    "chocolatey": "publish.yml",
    "conan": "conan-center-pr.yml",
}


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


def control_document(repository: str = "owner/publisher") -> dict[str, object]:
    return {
        "schema_version": 1,
        "providers": {
            provider: {
                "repository": repository,
                "control_tag": CONTROL_TAG,
                "workflow": workflow,
            }
            for provider, workflow in WORKFLOWS.items()
        },
    }


def immutable_control_responses() -> dict[tuple[str, str], object]:
    return {
        ("GET", f"repos/owner/publisher/git/ref/tags/{CONTROL_TAG}"): {
            "ref": f"refs/tags/{CONTROL_TAG}",
            "object": {"type": "commit", "sha": HEAD},
        },
        ("GET", f"repos/owner/publisher/releases/tags/{CONTROL_TAG}"): {
            "id": 19,
            "tag_name": CONTROL_TAG,
            "target_commitish": HEAD,
            "draft": False,
            "prerelease": False,
            "immutable": True,
            "assets": [],
        },
    }


class ProviderPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_generic_workflow_preflight_is_read_only(self) -> None:
        target = RepositoryTarget("owner", "publisher", 11)
        api = FakeApi(
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

    def test_control_manifest_and_immutable_release(self) -> None:
        path = Path(self.temporary.name) / "provider-controls.json"
        path.write_text(json.dumps(control_document()), encoding="utf-8")
        snapshot = verify_control_snapshot(
            FakeApi(immutable_control_responses()),
            target=RepositoryTarget("owner", "publisher", 11),
            manifest_path=path,
            provider="homebrew",
            workflow="publish-bottles.yml",
        )
        self.assertEqual(
            (snapshot.repository, snapshot.tag, snapshot.commit, snapshot.workflow),
            ("owner/publisher", CONTROL_TAG, HEAD, "publish-bottles.yml"),
        )

    def test_control_manifest_rejects_mutable_or_mismatched_controls(self) -> None:
        path = Path(self.temporary.name) / "provider-controls.json"
        for mutation in ("tag", "workflow", "mutable", "annotated", "target"):
            document = control_document()
            responses = copy.deepcopy(immutable_control_responses())
            if mutation == "tag":
                document["providers"]["homebrew"]["control_tag"] = "main"
            elif mutation == "workflow":
                document["providers"]["homebrew"]["workflow"] = "other.yml"
            elif mutation == "mutable":
                responses[("GET", f"repos/owner/publisher/releases/tags/{CONTROL_TAG}")]["immutable"] = False
            elif mutation == "annotated":
                responses[("GET", f"repos/owner/publisher/git/ref/tags/{CONTROL_TAG}")]["object"]["type"] = "tag"
            else:
                responses[("GET", f"repos/owner/publisher/releases/tags/{CONTROL_TAG}")]["target_commitish"] = "b" * 40
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.subTest(mutation=mutation), self.assertRaises(ProviderPreflightError):
                verify_control_snapshot(
                    FakeApi(responses),
                    target=RepositoryTarget("owner", "publisher", 11),
                    manifest_path=path,
                    provider="homebrew",
                    workflow="publish-bottles.yml",
                )

    def test_embedded_control_replays_signed_commit(self) -> None:
        contract = SimpleNamespace(
            provider_controls={
                "homebrew": {
                    "repository": "owner/publisher",
                    "control_tag": CONTROL_TAG,
                    "control_commit": HEAD,
                    "workflow": "publish-bottles.yml",
                }
            },
            destinations={
                "homebrew": {"repository": "owner/publisher", "repository_id": "11"}
            },
        )
        with mock.patch("release.provider_preflight.load_publication_contract", return_value=contract):
            snapshot = verify_embedded_control_snapshot(
                FakeApi(immutable_control_responses()),
                target=RepositoryTarget("owner", "publisher", 11),
                contract_path=Path("contract.json"),
                provider="homebrew",
                workflow="publish-bottles.yml",
            )
        self.assertEqual(snapshot.commit, HEAD)


class ProductionControlManifestTests(unittest.TestCase):
    MANIFEST = Path(__file__).resolve().parents[2] / "packaging/provider-controls.json"

    def test_manifest_is_compact_and_pins_only_provider_interfaces(self) -> None:
        value = json.loads(self.MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(value["schema_version"], 1)
        self.assertEqual(set(value["providers"]), set(WORKFLOWS))
        for provider, workflow in WORKFLOWS.items():
            entry = value["providers"][provider]
            self.assertEqual(set(entry), {"repository", "control_tag", "workflow"})
            self.assertEqual(entry["control_tag"], CONTROL_TAG)
            self.assertEqual(entry["workflow"], workflow)
            self.assertEqual(_control_manifest(self.MANIFEST, provider).workflow, workflow)

    def test_manifest_rejects_duplicate_fields(self) -> None:
        source = self.MANIFEST.read_text(encoding="utf-8")
        mutation = source.replace(
            '"schema_version": 1,',
            '"schema_version": 1, "schema_version": 1,',
            1,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "provider-controls.json"
            path.write_text(mutation, encoding="utf-8")
            with self.assertRaises(ProviderPreflightError):
                _control_manifest(path, "homebrew")


if __name__ == "__main__":
    unittest.main()
