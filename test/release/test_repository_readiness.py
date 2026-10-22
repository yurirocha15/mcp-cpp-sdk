from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import unittest
from unittest import mock

from release.model import ValidationError
from release.repository_readiness import (
    validate_repository,
    validate_tag_position,
)
from release.trusted_signers import load_trusted_signers


TAGGED = "a" * 40
HEAD = "b" * 40
ROOT = Path(__file__).resolve().parents[2]


class RepositoryReadinessPositionTests(unittest.TestCase):
    def test_fresh_release_requires_the_current_dispatch_head(self) -> None:
        validate_tag_position(
            commit=TAGGED,
            dispatch_head_sha=TAGGED,
            anchor_exists=False,
            is_ancestor=True,
        )
        with self.assertRaisesRegex(ValidationError, "fresh release tag"):
            validate_tag_position(
                commit=TAGGED,
                dispatch_head_sha=HEAD,
                anchor_exists=False,
                is_ancestor=True,
            )

    def test_existing_anchor_accepts_an_older_tagged_ancestor(self) -> None:
        validate_tag_position(
            commit=TAGGED,
            dispatch_head_sha=HEAD,
            anchor_exists=True,
            is_ancestor=True,
        )

    def test_every_mode_rejects_a_tag_outside_protected_main(self) -> None:
        for anchor_exists in (False, True):
            with self.subTest(anchor_exists=anchor_exists), self.assertRaisesRegex(
                ValidationError, "not reachable"
            ):
                validate_tag_position(
                    commit=TAGGED,
                    dispatch_head_sha=HEAD,
                    anchor_exists=anchor_exists,
                    is_ancestor=False,
                )

    def test_rejects_malformed_or_non_boolean_state(self) -> None:
        with self.assertRaises(ValidationError):
            validate_tag_position(
                commit="short",
                dispatch_head_sha=HEAD,
                anchor_exists=True,
                is_ancestor=True,
            )

    def test_checked_in_historical_signer_registry_retains_active_key(self) -> None:
        registry = load_trusted_signers(
            ROOT / "release/trusted-release-signers.json"
        )
        self.assertEqual(registry.active.identifier, "release-key-2026-07")
        self.assertEqual(len(registry.signers), 1)
        with self.assertRaises(ValidationError):
            validate_tag_position(
                commit=TAGGED,
                dispatch_head_sha=HEAD,
                anchor_exists=1,  # type: ignore[arg-type]
                is_ancestor=True,
            )


class RepositoryReadinessDryRunTests(unittest.TestCase):
    def arguments(self, **changes: str) -> argparse.Namespace:
        active = load_trusted_signers(ROOT / "release/trusted-release-signers.json").active
        values = {
            "tag": "v0.2.0",
            "version": "0.2.0",
            "dispatch_head_sha": HEAD,
            "anchor_exists": "false",
            "dry_run": "true",
            "primary_fingerprint": active.primary_fingerprint,
            "tag_fingerprint": active.tag_subkey_fingerprint,
            "artifact_fingerprint": active.artifact_subkey_fingerprint,
            "public_key": ROOT / "keys/release-signing-key.asc",
            "trusted_signers": ROOT / "release/trusted-release-signers.json",
            "targets": ROOT / "packaging/targets.json",
            "native_builder_lock": ROOT / "release/native-builders/lock.json",
            **changes,
        }
        return argparse.Namespace(**values)

    def fake_git(self, calls: list[tuple[str, ...]]):
        outputs = {
            ("merge-base", "--is-ancestor", HEAD, "origin/main"): "",
            ("show", f"{HEAD}:VERSION"): "0.2.0\n",
            ("show", f"{HEAD}:CHANGELOG.md"): "# Changelog\n\n## [0.2.0] - 2026-01-02\n",
            ("show", "-s", "--format=%ct", HEAD): "1767312000\n",
        }

        def run(*arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
            calls.append(arguments)
            if any("refs/tags" in argument or argument == "verify-tag" for argument in arguments):
                raise AssertionError(f"a dry run read the tag: {arguments}")
            return subprocess.CompletedProcess(arguments, 0, stdout=outputs[arguments], stderr="")

        return run

    def test_dry_run_validates_main_head_without_the_tag(self) -> None:
        calls: list[tuple[str, ...]] = []
        with mock.patch("release.repository_readiness._git", self.fake_git(calls)):
            result = validate_repository(self.arguments())
        self.assertEqual(result["commit"], HEAD)
        self.assertEqual(result["tag_object_sha"], "")
        self.assertEqual(result["source_date_epoch"], "1767312000")
        self.assertEqual(result["tag_fingerprint"], self.arguments().tag_fingerprint)
        self.assertNotEqual(result["native_matrix"], "[]")
        self.assertIn(("show", f"{HEAD}:CHANGELOG.md"), calls)

    def test_dry_run_still_requires_version_signer_and_no_existing_release(self) -> None:
        for changes, message in (
            ({"anchor_exists": "true"}, "no release exists"),
            ({"tag": "v0.2.1"}, "requested version"),
            ({"tag_fingerprint": "0" * 40}, "active trusted signer"),
        ):
            with self.subTest(changes=changes), mock.patch(
                "release.repository_readiness._git", self.fake_git([])
            ), self.assertRaisesRegex(ValidationError, message):
                validate_repository(self.arguments(**changes))


if __name__ == "__main__":
    unittest.main()
