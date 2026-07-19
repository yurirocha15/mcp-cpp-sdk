from __future__ import annotations

import base64
import json
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from publisher.bottle_handoff import ROOT_URL, artifact_name, create, discover
from publisher.consumer_test import sources, verify_linkage
from publisher.formula_branch import FormulaBranchError, update, verify_head
from publisher.formula_audit import audit
from publisher.ghcr import Client as GhcrClient, GhcrError, record, verify_recorded
from publisher.publication import (
    bind_artifact_ids,
    verify_checksums,
    verify_finalization,
    write_checksums,
)
from publisher.source_run import poll, verify_release, verify_repository
from publisher.validate_dispatch import validate
from publisher.verify_handoffs import verify as verify_handoffs
from publisher import verify_source_release


class DispatchTests(unittest.TestCase):
    def test_stable_request(self) -> None:
        request = {
            "source_tag": "v0.2.0",
            "source_commit_sha": "0" * 40,
            "source_workflow_head_sha": "3" * 40,
            "provider_control_sha": "4" * 40,
            "github_release_id": "1",
            "source_workflow_run_id": "2",
            "release_manifest_sha256": "1" * 64,
            "formula_pr_number": "3",
            "formula_branch": "release/mcp-cpp-sdk-v0.2.0",
            "formula_head_sha": "2" * 40,
            "request_uuid": "00000000-0000-0000-0000-000000000001",
        }
        self.assertEqual(validate(request), request)

    def test_rejects_wrong_branch(self) -> None:
        with self.assertRaises(ValueError):
            validate({"formula_branch": "main"})


class ReleaseBundleTests(unittest.TestCase):
    def test_rejects_legacy_incomplete_manifest_before_payload_use(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "release-manifest.json"
            manifest.write_text(
                json.dumps({"schema_version": 1, "tag": "v0.2.0", "commit": "0" * 40}),
                encoding="utf-8",
            )
            with mock.patch.object(verify_source_release, "_verify_key"), self.assertRaisesRegex(
                ValueError, "complete stable channel set"
            ):
                verify_source_release.verify(
                    root,
                    tag="v0.2.0",
                    commit="0" * 40,
                    manifest_sha256=verify_source_release.sha256(manifest),
                    primary_fingerprint="A" * 40,
                    artifact_fingerprint="B" * 40,
                )


class SourceRunTests(unittest.TestCase):
    def test_binds_source_repository_and_immutable_release(self) -> None:
        verify_repository(
            {
                "id": 9,
                "full_name": "yurirocha15/mcp-cpp-sdk",
                "private": False,
                "archived": False,
                "disabled": False,
                "default_branch": "main",
                "owner": {"id": 8, "login": "yurirocha15", "type": "User"},
            },
            repository_id="9",
            owner_id="8",
        )
        verify_release(
            {
                "id": 7,
                "tag_name": "v0.2.0",
                "draft": False,
                "prerelease": False,
                "immutable": True,
            },
            tag="v0.2.0",
            release_id="7",
        )

        with self.assertRaises(ValueError):
            verify_release(
                {
                    "id": 7,
                    "tag_name": "v0.2.0",
                    "draft": False,
                    "prerelease": False,
                    "immutable": False,
                },
                tag="v0.2.0",
                release_id="7",
            )

    def test_waits_for_manual_release_run_for_up_to_an_hour(self) -> None:
        responses = iter(
            [
                {"status": "waiting"},
                {
                    "id": 7,
                    "event": "workflow_dispatch",
                    "status": "completed",
                    "conclusion": "success",
                    "head_sha": "a" * 40,
                    "head_branch": "main",
                    "path": ".github/workflows/release.yml",
                    "repository": {"id": 9},
                },
            ]
        )
        clock = iter([0.0, 0.0, 20.0])
        sleeps = []
        result = poll(
            lambda: next(responses),
            run_id="7",
            workflow_head_sha="a" * 40,
            repository_id="9",
            timeout_seconds=3600,
            monotonic=lambda: next(clock),
            sleep=sleeps.append,
        )
        self.assertEqual(result["conclusion"], "success")
        self.assertEqual(sleeps, [20])


class BottleHandoffTests(unittest.TestCase):
    def test_bottle_discovery_rejects_missing_or_duplicated_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ValueError):
                discover(root)
            bottle = root / "mcp-cpp-sdk--0.2.0.x.bottle.tar.gz"
            metadata = root / "mcp-cpp-sdk--0.2.0.x.bottle.json"
            bottle.write_bytes(b"bottle")
            metadata.write_text("{}", encoding="utf-8")
            self.assertEqual(discover(root), (bottle, metadata))
            (root / "mcp-cpp-sdk--0.2.0.y.bottle.tar.gz").write_bytes(b"duplicate")
            with self.assertRaises(ValueError):
                discover(root)

    def _bundle(self, root: Path, tag: str, attempt: str, formula: bytes = b"formula\n") -> Path:
        source = root / f"source-{tag}-{attempt}"
        source.mkdir()
        bottle = source / f"mcp-cpp-sdk--0.2.0.{tag}.bottle.tar.gz"
        bottle.write_bytes(f"bottle-{tag}-{attempt}".encode())
        bottle_digest = hashlib.sha256(bottle.read_bytes()).hexdigest()
        metadata = source / f"mcp-cpp-sdk--0.2.0.{tag}.bottle.json"
        metadata.write_text(
            json.dumps({"root_url": ROOT_URL, "tag": tag, "sha256": bottle_digest})
        )
        formula_path = source / "mcp-cpp-sdk.rb"
        formula_path.write_bytes(formula)
        output = root / artifact_name(tag, "17", attempt)
        create(
            bottle=bottle,
            metadata=metadata,
            formula=formula_path,
            output=output,
            tag=tag,
            run_id="17",
            run_attempt=attempt,
        )
        return output

    def test_partial_rerun_selects_latest_attempt_per_bottle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._bundle(root, "sequoia", "1")
            self._bundle(root, "sequoia", "2")
            self._bundle(root, "arm64_sequoia", "1")
            formula_output = root / "Formula/mcp-cpp-sdk.rb"
            ledger = verify_handoffs(
                root,
                "17",
                "2",
                {"sequoia", "arm64_sequoia"},
                formula_output=formula_output,
            )
            attempts = {item["bottle_tag"]: item["run_attempt"] for item in ledger["bottles"]}
            self.assertEqual(attempts, {"sequoia": "2", "arm64_sequoia": "1"})
            self.assertEqual(ledger["assembly_run_attempt"], "2")
            self.assertEqual(formula_output.read_bytes(), b"formula\n")

    def test_artifact_api_binding_is_exact_and_paginated(self) -> None:
        ledger = {
            "schema_version": 2,
            "run_id": "17",
            "assembly_run_attempt": "2",
            "bottles": [
                {"artifact_name": f"bottle-{tag}-17-1", "run_attempt": "1"}
                for tag in ("a", "b", "c", "d")
            ],
        }
        pages = [
            {"artifacts": [{"id": index, "name": item["artifact_name"], "expired": False}]}
            for index, item in enumerate(ledger["bottles"], 1)
        ]
        self.assertEqual(len(bind_artifact_ids(ledger, pages)["artifact_ids"]), 4)
        pages[0]["artifacts"][0]["expired"] = True
        with self.assertRaises(ValueError):
            bind_artifact_ids(ledger, pages)

    def test_recursive_publication_checksums_fail_on_tampering(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "publisher").mkdir()
            (root / "formula.rb").write_text("formula\n")
            (root / "publisher/tool.py").write_text("pass\n")
            checksums = root / "SHA256SUMS"
            write_checksums(root, checksums)
            self.assertEqual(len(verify_checksums(root, checksums)), 2)
            (root / "publisher/tool.py").write_text("tampered\n")
            with self.assertRaises(ValueError):
                verify_checksums(root, checksums)

    def test_finalization_ledger_is_bound_to_run_attempt_and_head(self) -> None:
        ledger = {
            "run_id": "17",
            "publish_run_attempt": "2",
            "bottled_head_sha": "a" * 40,
        }
        verify_finalization(
            ledger, run_id="17", publish_attempt="2", expected_head="a" * 40
        )
        with self.assertRaises(ValueError):
            verify_finalization(
                {**ledger, "publish_run_attempt": "1"},
                run_id="17",
                publish_attempt="2",
                expected_head="a" * 40,
            )


class ConsumerAndRegistryTests(unittest.TestCase):
    def test_consumer_binds_both_version_apis_and_linkage_modes(self) -> None:
        cmake, source = sources("0.2.0")
        self.assertIn("mcp::sdk_shared", cmake)
        self.assertIn("mcp::sdk_static", cmake)
        self.assertIn("mcp::g_VERSION", source)
        self.assertIn('expected{"0.2.0"}', source)
        verify_linkage(
            "libmcp-cpp-sdk.so.0.2.0 => /prefix/lib/libmcp-cpp-sdk.so.0.2.0",
            "libcrypto.so.3 => /lib/libcrypto.so.3",
            system="Linux",
            version="0.2.0",
        )
        verify_linkage(
            "/prefix/lib/libmcp-cpp-sdk.0.2.0.dylib",
            "/usr/lib/libc++.1.dylib",
            system="Darwin",
            version="0.2.0",
        )
        with self.assertRaises(ValueError):
            verify_linkage(
                "libmcp-cpp-sdk.so.0.2.0",
                "libmcp-cpp-sdk.so.0.2.0",
                system="Linux",
                version="0.2.0",
            )
        with self.assertRaisesRegex(ValueError, "wrong SDK"):
            verify_linkage(
                "libmcp-cpp-sdk.so.0.2.1",
                "libcrypto.so.3",
                system="Linux",
                version="0.2.0",
            )

    def test_every_recorded_tag_is_resolved_again_before_merge(self) -> None:
        ledger = {
            "schema_version": 2,
            "bottles": [
                {"bottle_tag": tag, "bottle_sha256": tag * 64}
                for tag in ("a", "b", "c", "d")
            ],
        }
        digest = "sha256:" + "a" * 64
        recorded = record(ledger, "v0.2.0", lambda _tag, _bottle: digest)
        calls = []
        verify_recorded(recorded, lambda tag, _bottle: calls.append(tag) or digest)
        self.assertEqual(len(calls), 4)
        with self.assertRaises(GhcrError):
            verify_recorded(recorded, lambda _tag, _bottle: "sha256:" + "b" * 64)

    def test_public_manifest_must_annotate_the_exact_bottle_digest(self) -> None:
        bottle = "b" * 64
        manifest_digest = "sha256:" + "a" * 64

        class Client(GhcrClient):
            def token(self):
                return "token"

            def _manifest(self, reference, token):
                if reference.startswith("sha256:"):
                    return reference, {}
                return manifest_digest, {
                    "annotations": {"sh.brew.bottle.digest": bottle}
                }

        self.assertEqual(Client().resolve("0.2.0.sequoia", bottle), manifest_digest)
        with self.assertRaises(GhcrError):
            Client().resolve("0.2.0.sequoia", "c" * 64)


class FormulaBranchTests(unittest.TestCase):
    def test_formula_audit_allows_only_initial_empty_main_and_runs_brew(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            formula = Path(directory) / "Formula/mcp-cpp-sdk.rb"
            self.assertFalse(
                audit(formula, event_name="push", ref_name="main", runner=mock.Mock())
            )
            with self.assertRaises(ValueError):
                audit(
                    formula,
                    event_name="pull_request",
                    ref_name="feature",
                    runner=mock.Mock(),
                )
            formula.parent.mkdir()
            formula.write_text("class McpCppSdk < Formula\nend\n", encoding="utf-8")
            runner = mock.Mock()
            self.assertTrue(
                audit(formula, event_name="pull_request", ref_name="main", runner=runner)
            )
            self.assertEqual(
                [call.args[0][1] for call in runner.call_args_list], ["style", "audit"]
            )

    class FakeClient:
        def __init__(self, responses):
            self.responses = iter(responses)

        def request(self, method, path, body=None):
            return next(self.responses)

    @staticmethod
    def pull(head: str) -> dict[str, object]:
        actor = {"login": "mcp-cpp-sdk-homebrew-publisher[bot]", "type": "Bot"}
        return {
            "number": 3,
            "state": "open",
            "draft": False,
            "base": {"ref": "main"},
            "head": {
                "ref": "release/mcp-cpp-sdk-v0.2.0",
                "sha": head,
                "repo": {"full_name": "yurirocha15/homebrew-mcp-cpp-sdk"},
            },
            "user": actor,
        }

    @staticmethod
    def prior(head: str, parent: str) -> dict[str, object]:
        actor = {"login": "mcp-cpp-sdk-homebrew-publisher[bot]", "type": "Bot"}
        return {
            "sha": head,
            "parents": [{"sha": parent}],
            "files": [{"filename": "Formula/mcp-cpp-sdk.rb", "status": "modified"}],
            "author": actor,
            "committer": actor,
            "commit": {
                "message": "brew: add bottles for v0.2.0",
                "verification": {"verified": True},
            },
        }

    def test_accepts_only_exact_already_bottled_child_and_identical_formula(self) -> None:
        parent, head = "a" * 40, "b" * 40
        files = [{"filename": "Formula/mcp-cpp-sdk.rb"}]
        client = self.FakeClient([self.pull(head), files, self.prior(head, parent)])
        self.assertEqual(
            verify_head(
                client,
                pr="3",
                branch="release/mcp-cpp-sdk-v0.2.0",
                expected_head=parent,
                tag="v0.2.0",
            ),
            head,
        )
        content = base64.b64encode(b"formula\n").decode()
        client = self.FakeClient(
            [self.pull(head), files, self.prior(head, parent), {"path": "Formula/mcp-cpp-sdk.rb", "sha": "c" * 40, "content": content}]
        )
        self.assertEqual(
            update(
                client,
                pr="3",
                branch="release/mcp-cpp-sdk-v0.2.0",
                expected_head=parent,
                tag="v0.2.0",
                desired=b"formula\n",
            ),
            head,
        )
        client = self.FakeClient(
            [self.pull(head), files, self.prior(head, parent), {"path": "Formula/mcp-cpp-sdk.rb", "sha": "c" * 40, "content": content}]
        )
        with self.assertRaises(FormulaBranchError):
            update(
                client,
                pr="3",
                branch="release/mcp-cpp-sdk-v0.2.0",
                expected_head=parent,
                tag="v0.2.0",
                desired=b"different\n",
            )
