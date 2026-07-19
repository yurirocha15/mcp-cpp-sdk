from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from release.github_anchor import (
    _download_release,
    _release_metadata,
    download_and_verify_anchor,
)
from release.model import ValidationError

ROOT = Path(__file__).resolve().parents[2]

class GitHubAnchorOrchestrationTests(unittest.TestCase):
    def metadata(self) -> tuple[object, object, object]:
        return (
            {
                "databaseId": 9,
                "isDraft": False,
                "isImmutable": True,
                "isPrerelease": False,
                "tagName": "v0.2.0",
            },
            {
                "id": 9,
                "tag_name": "v0.2.0",
                "draft": False,
                "prerelease": False,
                "immutable": True,
            },
            [[]],
        )

    def arguments(self, root: Path) -> argparse.Namespace:
        return argparse.Namespace(
            directory=root / "verified",
            public_key=ROOT / "keys/release-signing-key.asc",
            trusted_signers=ROOT / "release/trusted-release-signers.json",
            tag="v0.2.0",
            version="0.2.0",
            commit="2" * 40,
            ledger_issue="17",
            repository="yurirocha15/mcp-cpp-sdk",
            tag_object_sha="1" * 40,
            primary_fingerprint="32F4760A898CAA62344F81B078DDAF6A80105366",
            tag_fingerprint="C017D850B03960E8EF1951EC680119C224E973B1",
            artifact_fingerprint="AC02DB2D7C09871E31DB352042C5770080B00549",
            prior_manifest_sha256="",
            anchor_existed="false",
            targets=root / "native-targets.json",
            target_catalog=root / "targets.json",
            native_builder_lock=root / "native-builder-lock.json",
            conan_requirements=root / "requirements.json",
            github_output=root / "github-output",
        )

    @mock.patch("release.github_publication.subprocess.run")
    def test_release_metadata_uses_paginated_slurped_asset_readback(self, run):
        graphql = {
            "databaseId": 9,
            "isDraft": False,
            "isImmutable": True,
            "isPrerelease": False,
            "tagName": "v0.2.0",
        }
        rest = {"id": 9, "tag_name": "v0.2.0"}
        assets = [[{"name": "asset", "digest": f"sha256:{'a' * 64}"}]]
        run.side_effect = [
            mock.Mock(returncode=0, stdout=json.dumps(graphql)),
            mock.Mock(returncode=0, stdout=json.dumps(rest)),
            mock.Mock(returncode=0, stdout=json.dumps(assets)),
        ]
        self.assertEqual(
            _release_metadata(repository="example/mcp-cpp-sdk", tag="v0.2.0"),
            (graphql, rest, assets),
        )
        asset_command = run.call_args_list[-1].args[0]
        self.assertEqual(asset_command[:4], ["gh", "api", "--paginate", "--slurp"])
        self.assertIn("releases/9/assets?per_page=100", asset_command[-1])

    @mock.patch("release.github_anchor.subprocess.run")
    def test_download_rejects_existing_directory_and_command_failure(self, run):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            existing = root / "existing"
            existing.mkdir()
            with self.assertRaisesRegex(ValidationError, "must not already exist"):
                _download_release(
                    directory=existing,
                    repository="example/mcp-cpp-sdk",
                    tag="v0.2.0",
                )
            run.assert_not_called()

            run.return_value = mock.Mock(returncode=1)
            with self.assertRaisesRegex(ValidationError, "download failed"):
                _download_release(
                    directory=root / "new",
                    repository="example/mcp-cpp-sdk",
                    tag="v0.2.0",
                )

    @mock.patch("release.github_anchor.verify_live_github_tag")
    def test_tag_version_mismatch_fails_before_public_reads(self, live_tag):
        with tempfile.TemporaryDirectory() as directory:
            args = self.arguments(Path(directory))
            args.version = "0.2.1"
            with self.assertRaisesRegex(ValidationError, "tag and version disagree"):
                download_and_verify_anchor(args)
            live_tag.assert_not_called()

    @mock.patch("release.github_anchor.verify_anchor")
    @mock.patch("release.github_anchor.verify_candidate")
    @mock.patch("release.github_anchor._release_metadata")
    @mock.patch("release.github_anchor._download_release")
    @mock.patch("release.github_anchor.verify_live_github_tag")
    def test_download_and_verify_delegates_every_policy_boundary(
        self,
        live_tag,
        download,
        metadata,
        candidate,
        anchor,
    ):
        digest = "d" * 64
        metadata.return_value = self.metadata()
        candidate.return_value = digest
        anchor.return_value = (9, digest)
        with tempfile.TemporaryDirectory() as directory:
            args = self.arguments(Path(directory))
            download.side_effect = lambda **values: values["directory"].mkdir()
            self.assertEqual(download_and_verify_anchor(args), (9, digest))
            live_tag.assert_called_once_with(
                repository=args.repository,
                tag=args.tag,
                tag_object_sha=args.tag_object_sha,
                commit=args.commit,
            )
            download.assert_called_once_with(
                directory=args.directory,
                repository=args.repository,
                tag=args.tag,
            )
            candidate.assert_called_once()
            anchor.assert_called_once()

    @mock.patch("release.github_anchor.verify_anchor")
    @mock.patch("release.github_anchor.verify_historical_candidate_v2")
    @mock.patch("release.github_anchor.verify_candidate")
    @mock.patch("release.github_anchor._release_metadata")
    @mock.patch("release.github_anchor._download_release")
    @mock.patch("release.github_anchor.verify_live_github_tag")
    def test_existing_anchor_selects_historical_embedded_policy(
        self,
        _live_tag,
        download,
        metadata,
        candidate,
        historical,
        anchor,
    ):
        digest = "d" * 64
        metadata.return_value = self.metadata()
        candidate.return_value = digest
        historical.return_value = digest
        anchor.return_value = (9, digest)
        with tempfile.TemporaryDirectory() as directory:
            args = self.arguments(Path(directory))
            args.anchor_existed = "true"
            args.public_key = Path(directory) / "rotated-current-key.asc"
            args.primary_fingerprint = "D" * 40
            args.tag_fingerprint = "E" * 40
            args.artifact_fingerprint = "F" * 40

            def create_anchor(**values):
                target = values["directory"]
                target.mkdir()
                (target / "release-signing-key.asc").write_bytes(
                    (ROOT / "keys/release-signing-key.asc").read_bytes()
                )
                (target / "release-manifest.json").write_text(
                    json.dumps(
                        {
                            "signers": {
                                "primary_fingerprint": "32F4760A898CAA62344F81B078DDAF6A80105366",
                                "tag_subkey_fingerprint": "C017D850B03960E8EF1951EC680119C224E973B1",
                                "artifact_subkey_fingerprint": "AC02DB2D7C09871E31DB352042C5770080B00549",
                            }
                        }
                    ),
                    encoding="utf-8",
                )

            download.side_effect = create_anchor
            self.assertEqual(download_and_verify_anchor(args), (9, digest))
            candidate.assert_not_called()
            self.assertEqual(
                historical.call_args.kwargs["public_key"],
                args.directory / "release-signing-key.asc",
            )

    @mock.patch("release.github_anchor.verify_anchor", return_value=(9, "e" * 64))
    @mock.patch("release.github_anchor.verify_candidate", return_value="d" * 64)
    @mock.patch("release.github_anchor._release_metadata")
    @mock.patch("release.github_anchor._download_release")
    @mock.patch("release.github_anchor.verify_live_github_tag")
    def test_candidate_and_anchor_digest_mismatch_is_rejected(
        self,
        _live_tag,
        download,
        metadata,
        _candidate,
        _anchor,
    ):
        with tempfile.TemporaryDirectory() as directory:
            metadata.return_value = self.metadata()
            args = self.arguments(Path(directory))
            download.side_effect = lambda **values: values["directory"].mkdir()
            with self.assertRaisesRegex(ValidationError, "digests disagree"):
                download_and_verify_anchor(args)


if __name__ == "__main__":
    unittest.main()
