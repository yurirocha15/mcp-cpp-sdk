from __future__ import annotations

import base64
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from release.github_provider import RepositoryTarget
from release.provider_preflight import (
    ProviderPreflightError,
    _control_manifest,
    preflight_conan_fork,
    preflight_workflow,
    require_selected_preflights,
    verify_control_snapshot,
    verify_embedded_control_snapshot,
)
from release.artifacts import canonical_json_bytes, load_native_targets
from release.publication_contract import build_publication_contract


HEAD = "a" * 40
CONTROL_TAG = "release-control-v1"
UPSTREAM = "conan-io/conan-center-index"


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


def contents(path: str, text: str) -> dict[str, object]:
    return {
        "type": "file",
        "path": path,
        "encoding": "base64",
        "content": base64.b64encode(text.encode("utf-8")).decode("ascii"),
    }


class ProviderPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.requirements = Path(self.temporary.name) / "requirements.json"
        self.requirements.write_text(
            json.dumps(
                {
                    "boost": "boost/1.86.0",
                    "nlohmann_json": "nlohmann_json/3.12.0",
                    "openssl": "openssl/3.6.3",
                }
            ),
            encoding="utf-8",
        )
        self.coordinate = "owner/conan-center-index"

    @staticmethod
    def control_entry(*, directory: str = "publisher") -> dict[str, object]:
        return {
            "repository": "owner/publisher",
            "control_tag": CONTROL_TAG,
            "files": {
                "publisher/publish.py": "bootstrap/publisher/publish.py"
            },
            "exact_directories": [directory],
        }

    @classmethod
    def control_manifest(
        cls, *, directory: str = "publisher"
    ) -> dict[str, object]:
        entry = cls.control_entry(directory=directory)
        return {
            "schema_version": 1,
            "providers": {
                provider: dict(entry)
                for provider in ("chocolatey", "conan", "homebrew")
            },
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def responses(self):
        value = {
            ("GET", f"repos/{self.coordinate}"): {
                "id": 17,
                "full_name": self.coordinate,
                "default_branch": "master",
                "fork": True,
                "archived": False,
                "disabled": False,
                "parent": {"id": 29, "full_name": UPSTREAM},
            },
            ("GET", f"repos/{UPSTREAM}"): {
                "id": 29,
                "full_name": UPSTREAM,
                "default_branch": "master",
                "fork": False,
                "archived": False,
                "disabled": False,
            },
            ("GET", f"repos/{self.coordinate}/git/ref/heads/master"): {
                "ref": "refs/heads/master",
                "object": {"type": "commit", "sha": HEAD},
            },
            ("GET", f"repos/{UPSTREAM}/git/ref/heads/master"): {
                "ref": "refs/heads/master",
                "object": {"type": "commit", "sha": HEAD},
            },
        }
        for package, version in (
            ("boost", "1.86.0"),
            ("nlohmann_json", "3.12.0"),
            ("openssl", "3.6.3"),
        ):
            path = f"recipes/{package}/config.yml"
            value[("GET", f"repos/{UPSTREAM}/contents/{path}?ref={HEAD}")] = contents(
                path, f'versions:\n  "{version}":\n    folder: all\n'
            )
            value[
                ("GET", f"repos/{UPSTREAM}/contents/recipes/{package}/all?ref={HEAD}")
            ] = [{"name": "conanfile.py", "type": "file"}]
        return value

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
        self.assertEqual(
            preflight_workflow(api, target=target, workflow="publish.yml"), 12
        )
        self.assertTrue(all(method == "GET" for method, _endpoint, _body in api.calls))

    def test_conan_preflight_binds_fork_upstream_head_and_dependencies(self) -> None:
        api = FakeApi(self.responses())
        self.assertEqual(
            preflight_conan_fork(
                api,
                owner="owner",
                repository="conan-center-index",
                expected_repository_id=17,
                requirements_path=self.requirements,
            ),
            HEAD,
        )
        self.assertEqual(len(api.calls), 10)
        self.assertTrue(all(method == "GET" for method, _endpoint, _body in api.calls))

    def test_conan_preflight_rejects_wrong_parent_or_diverged_fork(self) -> None:
        for mutation in ("parent", "head"):
            responses = self.responses()
            if mutation == "parent":
                responses[("GET", f"repos/{self.coordinate}")]["parent"] = {
                    "id": 29,
                    "full_name": "attacker/conan-center-index",
                }
            else:
                responses[("GET", f"repos/{self.coordinate}/git/ref/heads/master")][
                    "object"
                ]["sha"] = "b" * 40
            with self.subTest(mutation=mutation), self.assertRaises(
                ProviderPreflightError
            ):
                preflight_conan_fork(
                    FakeApi(responses),
                    owner="owner",
                    repository="conan-center-index",
                    expected_repository_id=17,
                    requirements_path=self.requirements,
                )

    def test_conan_preflight_rejects_missing_version_or_recipe_folder(self) -> None:
        for mutation in ("version", "folder"):
            responses = self.responses()
            config_endpoint = (
                "GET",
                f"repos/{UPSTREAM}/contents/recipes/boost/config.yml?ref={HEAD}",
            )
            if mutation == "version":
                responses[config_endpoint] = contents(
                    "recipes/boost/config.yml",
                    'versions:\n  "1.85.0":\n    folder: all\n',
                )
            else:
                responses[
                    (
                        "GET",
                        f"repos/{UPSTREAM}/contents/recipes/boost/all?ref={HEAD}",
                    )
                ] = []
            with self.subTest(mutation=mutation), self.assertRaises(
                ProviderPreflightError
            ):
                preflight_conan_fork(
                    FakeApi(responses),
                    owner="owner",
                    repository="conan-center-index",
                    expected_repository_id=17,
                    requirements_path=self.requirements,
                )

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
            with self.subTest(results=results), self.assertRaises(
                ProviderPreflightError
            ):
                require_selected_preflights(targets=targets, results=results)

    def test_downstream_controls_match_exact_files_and_directory_inventory(self) -> None:
        root = Path(self.temporary.name)
        packaging = root / "packaging"
        controls = root / "bootstrap/publisher"
        packaging.mkdir()
        controls.mkdir(parents=True)
        source = controls / "publish.py"
        source.write_bytes(b"print('reviewed')\n")
        manifest = packaging / "provider-controls.json"
        manifest.write_text(
            json.dumps(self.control_manifest()),
            encoding="utf-8",
        )
        encoded = base64.b64encode(source.read_bytes()).decode("ascii")
        blob = __import__("hashlib").sha1(
            f"blob {source.stat().st_size}\0".encode("ascii") + source.read_bytes(),
            usedforsecurity=False,
        ).hexdigest()
        target = RepositoryTarget("owner", "publisher", 11)
        responses = {
            ("GET", f"repos/owner/publisher/releases/tags/{CONTROL_TAG}"): {
                "id": 19,
                "tag_name": CONTROL_TAG,
                "target_commitish": HEAD,
                "draft": False,
                "prerelease": False,
                "immutable": True,
                "assets": [],
            },
            ("GET", f"repos/owner/publisher/git/ref/tags/{CONTROL_TAG}"): {
                "ref": f"refs/tags/{CONTROL_TAG}",
                "object": {"type": "commit", "sha": HEAD},
            },
            ("GET", f"repos/owner/publisher/contents/publisher?ref={HEAD}"): [
                {"type": "file", "path": "publisher/publish.py"}
            ],
            ("GET", f"repos/owner/publisher/contents/publisher/publish.py?ref={HEAD}"): {
                "type": "file",
                "path": "publisher/publish.py",
                "encoding": "base64",
                "content": encoded,
                "sha": blob,
            },
        }
        snapshot = verify_control_snapshot(
            FakeApi(responses),
            target=target,
            manifest_path=manifest,
            provider="homebrew",
        )
        self.assertEqual((snapshot.tag, snapshot.commit), (CONTROL_TAG, HEAD))
        responses[("GET", f"repos/owner/publisher/contents/publisher?ref={HEAD}")].append(
            {"type": "file", "path": "publisher/unreviewed.py"}
        )
        with self.assertRaises(ProviderPreflightError):
            verify_control_snapshot(
                FakeApi(responses),
                target=target,
                manifest_path=manifest,
                provider="homebrew",
            )

    def test_downstream_controls_reject_unsafe_or_unbound_exact_directories(self) -> None:
        root = Path(self.temporary.name)
        packaging = root / "packaging"
        source = root / "bootstrap/publisher/publish.py"
        packaging.mkdir()
        source.parent.mkdir(parents=True)
        source.write_text("pass\n", encoding="utf-8")
        manifest = packaging / "provider-controls.json"
        for directory in ("../publisher", "/publisher", ".", "missing", "publisher/"):
            manifest.write_text(
                json.dumps(self.control_manifest(directory=directory)),
                encoding="utf-8",
            )
            with self.subTest(directory=directory), self.assertRaises(
                ProviderPreflightError
            ):
                verify_control_snapshot(
                    FakeApi({}),
                    target=RepositoryTarget("owner", "publisher", 11),
                    manifest_path=manifest,
                    provider="homebrew",
                )

    def test_downstream_controls_require_exact_immutable_release_and_direct_tag(self) -> None:
        root = Path(self.temporary.name)
        packaging = root / "packaging"
        source = root / "bootstrap/publisher/publish.py"
        packaging.mkdir()
        source.parent.mkdir(parents=True)
        source.write_text("pass\n", encoding="utf-8")
        manifest = packaging / "provider-controls.json"
        manifest.write_text(
            json.dumps(self.control_manifest()),
            encoding="utf-8",
        )
        encoded = base64.b64encode(source.read_bytes()).decode("ascii")
        blob = __import__("hashlib").sha1(
            f"blob {source.stat().st_size}\0".encode("ascii") + source.read_bytes(),
            usedforsecurity=False,
        ).hexdigest()
        base = {
            ("GET", f"repos/owner/publisher/releases/tags/{CONTROL_TAG}"): {
                "id": 19,
                "tag_name": CONTROL_TAG,
                "target_commitish": HEAD,
                "draft": False,
                "prerelease": False,
                "immutable": True,
                "assets": [],
            },
            ("GET", f"repos/owner/publisher/git/ref/tags/{CONTROL_TAG}"): {
                "ref": f"refs/tags/{CONTROL_TAG}",
                "object": {"type": "commit", "sha": HEAD},
            },
            ("GET", f"repos/owner/publisher/contents/publisher?ref={HEAD}"): [
                {"type": "file", "path": "publisher/publish.py"}
            ],
            ("GET", f"repos/owner/publisher/contents/publisher/publish.py?ref={HEAD}"): {
                "type": "file",
                "path": "publisher/publish.py",
                "encoding": "base64",
                "content": encoded,
                "sha": blob,
            },
        }
        for mutation in ("mutable", "draft", "asset", "target", "annotated", "ref"):
            responses = json.loads(json.dumps({str(key): value for key, value in base.items()}))
            # Preserve tuple route keys after making independent deep copies.
            responses = {
                key: responses[str(key)]
                for key in base
            }
            release = responses[("GET", f"repos/owner/publisher/releases/tags/{CONTROL_TAG}")]
            tag = responses[("GET", f"repos/owner/publisher/git/ref/tags/{CONTROL_TAG}")]
            if mutation == "mutable":
                release["immutable"] = False
            elif mutation == "draft":
                release["draft"] = True
            elif mutation == "asset":
                release["assets"] = [{"id": 1}]
            elif mutation == "target":
                release["target_commitish"] = "b" * 40
            elif mutation == "annotated":
                tag["object"]["type"] = "tag"
            else:
                tag["object"]["sha"] = "b" * 40
            with self.subTest(mutation=mutation), self.assertRaises(
                ProviderPreflightError
            ):
                verify_control_snapshot(
                    FakeApi(responses),
                    target=RepositoryTarget("owner", "publisher", 11),
                    manifest_path=manifest,
                    provider="homebrew",
                )

    def test_signed_contract_replays_exact_historical_provider_snapshot(self) -> None:
        root = Path(__file__).resolve().parents[2]
        builders = tuple(
            {
                "id": target.id,
                "image": "ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders",
                "image_digest": f"sha256:{'9' * 64}",
            }
            for target in load_native_targets(root / "packaging/targets.json")
        )
        with mock.patch(
            "release.publication_contract.load_builder_lock", return_value=builders
        ):
            contract = build_publication_contract(
                root=root,
                control_commits={
                    "homebrew": HEAD,
                    "chocolatey": "b" * 40,
                    "conan": "c" * 40,
                },
                destinations={
                    "cloudsmith": {
                        "namespace": "mcp-cpp-sdk",
                        "repository": "mcp-cpp-sdk",
                        "preflight_username": "preflight",
                        "publish_username": "publisher",
                    },
                    "homebrew": {
                        "repository": "yurirocha15/homebrew-mcp-cpp-sdk",
                        "repository_id": "11",
                        "bot_login": "homebrew[bot]",
                    },
                    "chocolatey": {
                        "repository": "yurirocha15/mcp-cpp-sdk-chocolatey-publisher",
                        "repository_id": "12",
                    },
                    "conan_fork": {
                        "repository": "yurirocha15/conan-center-index",
                        "repository_id": "13",
                        "upstream": "conan-io/conan-center-index",
                    },
                    "conan_control": {
                        "repository": "yurirocha15/mcp-cpp-sdk-release-control",
                        "repository_id": "14",
                    },
                },
            )
        content = b"print('historical')\n"
        blob = __import__("hashlib").sha1(
            f"blob {len(content)}\0".encode("ascii") + content,
            usedforsecurity=False,
        ).hexdigest()
        contract["provider_controls"]["homebrew"] = {
            "repository": "owner/publisher",
            "control_tag": CONTROL_TAG,
            "control_commit": HEAD,
            "workflow": "publish-bottles.yml",
            "files": {"publisher/publish.py": blob},
            "exact_directories": ["publisher"],
        }
        contract["destinations"]["homebrew"] = {
            "repository": "owner/publisher",
            "repository_id": "11",
            "bot_login": "homebrew[bot]",
        }
        contract_path = Path(self.temporary.name) / "release-publication-contract.json"
        contract_path.write_bytes(canonical_json_bytes(contract))
        responses = {
            ("GET", f"repos/owner/publisher/releases/tags/{CONTROL_TAG}"): {
                "id": 19,
                "tag_name": CONTROL_TAG,
                "target_commitish": HEAD,
                "draft": False,
                "prerelease": False,
                "immutable": True,
                "assets": [],
            },
            ("GET", f"repos/owner/publisher/git/ref/tags/{CONTROL_TAG}"): {
                "ref": f"refs/tags/{CONTROL_TAG}",
                "object": {"type": "commit", "sha": HEAD},
            },
            ("GET", f"repos/owner/publisher/contents/publisher?ref={HEAD}"): [
                {"type": "file", "path": "publisher/publish.py"}
            ],
            ("GET", f"repos/owner/publisher/contents/publisher/publish.py?ref={HEAD}"): {
                "type": "file",
                "path": "publisher/publish.py",
                "sha": blob,
            },
        }
        snapshot = verify_embedded_control_snapshot(
            FakeApi(responses),
            target=RepositoryTarget("owner", "publisher", 11),
            contract_path=contract_path,
            provider="homebrew",
            workflow="publish-bottles.yml",
        )
        self.assertEqual((snapshot.tag, snapshot.commit), (CONTROL_TAG, HEAD))


class ProductionControlManifestTests(unittest.TestCase):
    ROOT = Path(__file__).resolve().parents[2]
    MANIFEST = ROOT / "packaging/provider-controls.json"
    PROVIDERS = {
        "homebrew": ROOT / "bootstrap/homebrew-tap",
        "chocolatey": ROOT / "bootstrap/chocolatey-publisher",
        "conan": ROOT / "bootstrap/conan-release-control",
    }

    def test_manifest_covers_every_reviewed_bootstrap_control_file(self) -> None:
        value = json.loads(self.MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(value["schema_version"], 1)
        self.assertEqual(set(value["providers"]), set(self.PROVIDERS))
        for provider, bootstrap in self.PROVIDERS.items():
            entry = value["providers"][provider]
            self.assertEqual(entry["control_tag"], "release-control-v1")
            actual_sources = set(entry["files"].values())
            expected_sources = {
                path.relative_to(self.ROOT).as_posix()
                for path in bootstrap.rglob("*")
                if path.is_file() and "__pycache__" not in path.parts
            }
            if provider in {"chocolatey", "conan"}:
                expected_sources.add("keys/release-signing-key.asc")
            self.assertEqual(actual_sources, expected_sources, provider)
            self.assertEqual(
                list(entry["files"]), sorted(entry["files"]), provider
            )
            self.assertEqual(
                len(actual_sources), len(entry["files"]), provider
            )

    def test_exact_directories_cover_every_executable_inventory(self) -> None:
        value = json.loads(self.MANIFEST.read_text(encoding="utf-8"))
        for provider, entry in value["providers"].items():
            expected_directories = [
                ".",
                ".github",
                ".github/workflows",
                "publisher",
                "tests",
            ]
            if provider in {"chocolatey", "conan"}:
                expected_directories.insert(3, "keys")
            self.assertEqual(entry["exact_directories"], expected_directories)
            for directory in expected_directories:
                remote_files = {
                    path
                    for path in entry["files"]
                    if str(Path(path).parent).replace("\\", "/") == directory
                }
                self.assertTrue(remote_files, (provider, directory))

    def test_manifest_parser_rejects_provider_schema_and_duplicate_mutations(self) -> None:
        source = self.MANIFEST.read_text(encoding="utf-8")
        mutations: dict[str, str] = {}
        missing = json.loads(source)
        del missing["providers"]["conan"]
        mutations["missing provider"] = json.dumps(missing)
        bad_tag = json.loads(source)
        bad_tag["providers"]["homebrew"]["control_tag"] = "main"
        mutations["mutable branch"] = json.dumps(bad_tag)
        duplicate = source.replace(
            '"schema_version": 1,',
            '"schema_version": 1, "schema_version": 1,',
            1,
        )
        mutations["duplicate field"] = duplicate
        unsorted = json.loads(source)
        files = unsorted["providers"]["homebrew"]["files"]
        unsorted["providers"]["homebrew"]["files"] = dict(
            reversed(list(files.items()))
        )
        mutations["unsorted files"] = json.dumps(unsorted)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "provider-controls.json"
            for label, mutation in mutations.items():
                path.write_text(mutation, encoding="utf-8")
                with self.subTest(label=label), self.assertRaises(
                    ProviderPreflightError
                ):
                    _control_manifest(path, "homebrew")


if __name__ == "__main__":
    unittest.main()
