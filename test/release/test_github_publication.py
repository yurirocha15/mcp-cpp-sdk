from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from release.github_publication import (
    PublicationPolicyError,
    classify_existing_release,
    create_release,
    probe_existing_release,
    verify_release_identity,
    verify_anchor_handoff,
    verify_tag_objects,
    verify_tag_ruleset,
)


TAG_OBJECT = "1" * 40
COMMIT = "2" * 40


class GithubPublicationPolicyTest(unittest.TestCase):
    def existing_release_response(self, release):
        return {
            "data": {
                "repository": {
                    "databaseId": 101740085,
                    "nameWithOwner": "example/mcp-cpp-sdk",
                    "release": release,
                }
            }
        }

    def ruleset(self):
        return {
            "id": 42,
            "target": "tag",
            "enforcement": "active",
            "source_type": "Repository",
            "source": "example/mcp-cpp-sdk",
            "updated_at": "2026-07-19T10:00:00Z",
            "bypass_actors": [],
            "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
            "rules": [{"type": "deletion"}, {"type": "update"}],
        }

    def ref(self):
        return {"ref": "refs/tags/v0.2.0", "object": {"type": "tag", "sha": TAG_OBJECT}}

    def tag_object(self):
        return {"sha": TAG_OBJECT, "tag": "v0.2.0", "object": {"type": "commit", "sha": COMMIT}}

    def test_accepts_exact_non_bypassable_tag_boundary(self):
        verify_tag_ruleset(
            self.ruleset(), ruleset_id=42, repository="example/mcp-cpp-sdk",
            expected_updated_at="2026-07-19T10:00:00Z",
        )
        verify_tag_objects(
            self.ref(), self.tag_object(), tag="v0.2.0",
            tag_object_sha=TAG_OBJECT, commit_sha=COMMIT,
        )

    def test_rejects_ruleset_weakening_mutations(self):
        mutations = []
        for path, value in (
            (("enforcement",), "disabled"),
            (("target",), "branch"),
            (("bypass_actors",), [{"actor_type": "OrganizationAdmin"}]),
            (("conditions", "ref_name", "include"), ["refs/tags/release-test*"]),
            (("conditions", "ref_name", "exclude"), ["refs/tags/v0.2.0"]),
            (("rules",), [{"type": "deletion"}]),
            (("rules",), [{"type": "deletion"}, {"type": "update"}, {"type": "creation"}]),
            (("updated_at",), "2026-07-19T10:01:00Z"),
        ):
            candidate = copy.deepcopy(self.ruleset())
            target = candidate
            for component in path[:-1]:
                target = target[component]
            target[path[-1]] = value
            mutations.append(candidate)
        for candidate in mutations:
            with self.subTest(candidate=candidate), self.assertRaises(PublicationPolicyError):
                verify_tag_ruleset(
                    candidate, ruleset_id=42, repository="example/mcp-cpp-sdk",
                    expected_updated_at="2026-07-19T10:00:00Z",
                )
        missing_bypass = self.ruleset()
        del missing_bypass["bypass_actors"]
        with self.assertRaises(PublicationPolicyError):
            verify_tag_ruleset(
                missing_bypass,
                ruleset_id=42,
                repository="example/mcp-cpp-sdk",
                expected_updated_at="2026-07-19T10:00:00Z",
            )

    def test_rejects_moved_or_lightweight_tag(self):
        moved = self.ref()
        moved["object"]["sha"] = "3" * 40
        lightweight = self.ref()
        lightweight["object"]["type"] = "commit"
        wrong_commit = self.tag_object()
        wrong_commit["object"]["sha"] = "4" * 40
        for ref, tag_object in ((moved, self.tag_object()), (lightweight, self.tag_object()), (self.ref(), wrong_commit)):
            with self.subTest(ref=ref), self.assertRaises(PublicationPolicyError):
                verify_tag_objects(
                    ref, tag_object, tag="v0.2.0",
                    tag_object_sha=TAG_OBJECT, commit_sha=COMMIT,
                )

    def test_release_views_must_share_the_exact_immutable_id(self):
        graphql = {
            "databaseId": 99, "isDraft": False, "isImmutable": True,
            "isPrerelease": False, "tagName": "v0.2.0",
        }
        rest = {"id": 99, "tag_name": "v0.2.0", "draft": False, "prerelease": False, "immutable": True}
        self.assertEqual(verify_release_identity(graphql, rest, tag="v0.2.0", prerelease=False), 99)
        for field, value in (("id", 100), ("immutable", False), ("tag_name", "v0.2.1")):
            candidate = dict(rest)
            candidate[field] = value
            with self.subTest(field=field), self.assertRaises(PublicationPolicyError):
                verify_release_identity(graphql, candidate, tag="v0.2.0", prerelease=False)

    def test_existing_release_probe_distinguishes_absent_and_immutable_anchor(self):
        self.assertFalse(
            classify_existing_release(
                self.existing_release_response(None),
                repository="example/mcp-cpp-sdk",
                repository_id=101740085,
                tag="v0.2.0",
            )
        )
        immutable = {
            "databaseId": 99,
            "isDraft": False,
            "isImmutable": True,
            "isPrerelease": False,
            "tagName": "v0.2.0",
        }
        self.assertTrue(
            classify_existing_release(
                self.existing_release_response(immutable),
                repository="example/mcp-cpp-sdk",
                repository_id=101740085,
                tag="v0.2.0",
            )
        )

    def test_existing_release_probe_fails_closed_for_draft_or_mutable_release(self):
        base = {
            "databaseId": 99,
            "isDraft": False,
            "isImmutable": True,
            "isPrerelease": False,
            "tagName": "v0.2.0",
        }
        for field, value in (
            ("isDraft", True),
            ("isImmutable", False),
            ("isPrerelease", True),
            ("tagName", "v0.2.1"),
        ):
            candidate = dict(base)
            candidate[field] = value
            with self.subTest(field=field), self.assertRaises(PublicationPolicyError):
                classify_existing_release(
                    self.existing_release_response(candidate),
                    repository="example/mcp-cpp-sdk",
                    repository_id=101740085,
                    tag="v0.2.0",
                )
        draft = dict(base, isDraft=True)
        with self.assertRaisesRegex(PublicationPolicyError, "BLOCKED_MANUAL_ACTION"):
            classify_existing_release(
                self.existing_release_response(draft),
                repository="example/mcp-cpp-sdk",
                repository_id=101740085,
                tag="v0.2.0",
            )

    @mock.patch("release.github_publication.subprocess.run")
    def test_probe_uses_fixed_graphql_query_and_rejects_command_failure(self, run):
        run.return_value = mock.Mock(
            returncode=0,
            stdout=__import__("json").dumps(self.existing_release_response(None)),
            stderr="",
        )
        self.assertFalse(
            probe_existing_release(
                repository="example/mcp-cpp-sdk", repository_id=101740085, tag="v0.2.0"
            )
        )
        arguments = run.call_args.args[0]
        self.assertEqual(arguments[:3], ["gh", "api", "graphql"])
        self.assertIn("release(tagName:$tag)", arguments[4])
        run.return_value = mock.Mock(returncode=1, stdout="", stderr="secret-free error")
        with self.assertRaises(PublicationPolicyError):
            probe_existing_release(
                repository="example/mcp-cpp-sdk", repository_id=101740085, tag="v0.2.0"
            )

    @mock.patch("release.github_publication.subprocess.run")
    def test_create_release_uses_an_exact_asset_inventory_and_rc_flags(self, run):
        for tag, release_kind, prerelease_flags in (
            ("v0.2.0", "stable", []),
            ("v0.2.0-rc.1", "rc", ["--prerelease", "--latest=false"]),
        ):
            with self.subTest(tag=tag), tempfile.TemporaryDirectory() as directory:
                bundle = Path(directory)
                (bundle / "RELEASE_NOTES.md").write_text("notes\n", encoding="utf-8")
                (bundle / "asset.tar.gz").write_bytes(b"asset")
                ref = self.ref()
                ref["ref"] = f"refs/tags/{tag}"
                tag_object = self.tag_object()
                tag_object["tag"] = tag
                run.side_effect = [
                    mock.Mock(returncode=0, stdout=json.dumps(ref)),
                    mock.Mock(returncode=0, stdout=json.dumps(tag_object)),
                    mock.Mock(
                        returncode=0,
                        stdout=json.dumps(self.existing_release_response(None)),
                        stderr="",
                    ),
                    mock.Mock(returncode=0),
                ]
                create_release(
                    directory=bundle,
                    repository="example/mcp-cpp-sdk",
                    repository_id=101740085,
                    tag=tag,
                    release_kind=release_kind,
                    tag_object_sha=TAG_OBJECT,
                    commit=COMMIT,
                )
                command = run.call_args_list[-1].args[0]
                self.assertEqual(command[:4], ["gh", "release", "create", tag])
                self.assertIn(str(bundle / "asset.tar.gz"), command)
                self.assertNotIn(str(bundle / "RELEASE_NOTES.md"), command[: command.index("--notes-file")])
                if prerelease_flags:
                    self.assertEqual(command[-len(prerelease_flags) :], prerelease_flags)
                else:
                    self.assertNotIn("--prerelease", command)

    @mock.patch("release.github_publication.subprocess.run")
    def test_create_release_fails_closed_before_write_when_anchor_exists(self, run):
        immutable = {
            "databaseId": 99,
            "isDraft": False,
            "isImmutable": True,
            "isPrerelease": False,
            "tagName": "v0.2.0",
        }
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory)
            (bundle / "RELEASE_NOTES.md").write_text("notes\n", encoding="utf-8")
            (bundle / "asset.tar.gz").write_bytes(b"asset")
            run.side_effect = [
                mock.Mock(returncode=0, stdout=json.dumps(self.ref())),
                mock.Mock(returncode=0, stdout=json.dumps(self.tag_object())),
                mock.Mock(
                    returncode=0,
                    stdout=json.dumps(self.existing_release_response(immutable)),
                    stderr="",
                ),
            ]
            with self.assertRaisesRegex(PublicationPolicyError, "already exists"):
                create_release(
                    directory=bundle,
                    repository="example/mcp-cpp-sdk",
                    repository_id=101740085,
                    tag="v0.2.0",
                    release_kind="stable",
                    tag_object_sha=TAG_OBJECT,
                    commit=COMMIT,
                )
            self.assertEqual(run.call_count, 3)

    def test_create_release_rejects_kind_mismatch_and_unsafe_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory)
            (bundle / "RELEASE_NOTES.md").write_text("notes\n", encoding="utf-8")
            (bundle / "asset.tar.gz").write_bytes(b"asset")
            with self.assertRaisesRegex(PublicationPolicyError, "release kind"):
                create_release(
                    directory=bundle,
                    repository="example/mcp-cpp-sdk",
                    repository_id=101740085,
                    tag="v0.2.0-rc.1",
                    release_kind="stable",
                    tag_object_sha=TAG_OBJECT,
                    commit=COMMIT,
                )
            (bundle / "unsafe directory").mkdir()
            with self.assertRaisesRegex(PublicationPolicyError, "unsafe entry"):
                create_release(
                    directory=bundle,
                    repository="example/mcp-cpp-sdk",
                    repository_id=101740085,
                    tag="v0.2.0",
                    release_kind="stable",
                    tag_object_sha=TAG_OBJECT,
                    commit=COMMIT,
                )
    def test_anchor_handoff_binds_tag_and_manifest(self):
        anchor = {
            "schema_version": 1,
            "repository": "example/mcp-cpp-sdk",
            "release_id": "42",
            "tag": "v0.2.0",
            "commit": COMMIT,
            "manifest_sha256": "a" * 64,
        }
        verify_anchor_handoff(anchor, tag="v0.2.0", manifest_sha256="a" * 64)
        with self.assertRaises(PublicationPolicyError):
            verify_anchor_handoff(anchor, tag="v0.2.1", manifest_sha256="a" * 64)


if __name__ == "__main__":
    unittest.main()
