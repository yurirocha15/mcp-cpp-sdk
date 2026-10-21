from __future__ import annotations

from pathlib import Path
import unittest

from release.model import ValidationError
from release.repository_readiness import (
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


if __name__ == "__main__":
    unittest.main()
