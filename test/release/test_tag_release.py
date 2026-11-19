from __future__ import annotations

import ast
import json
from pathlib import Path
import unittest

from release.model import SemVer
from release.tag_release import (
    CHANNELS,
    TagReleaseError,
    active_signer,
    confirm,
    release_dispatch,
    signature_fingerprints,
    successful_dry_run,
)


ROOT = Path(__file__).resolve().parents[2]
COMMIT = "6e72df9b5a6db687d78b8cb43fafdacd3b1e6891"


class TagReleaseTests(unittest.TestCase):
    def test_active_signer_comes_from_the_trusted_signers(self) -> None:
        text = (ROOT / "release/trusted-release-signers.json").read_text(encoding="utf-8")
        document = json.loads(text)
        signer = active_signer(text)
        expected = next(s for s in document["signers"] if s["id"] == document["active_signer_id"])
        self.assertEqual(signer["tag_subkey_fingerprint"], expected["tag_subkey_fingerprint"])
        self.assertEqual(signer["public_key"], expected["public_key"])
        document["active_signer_id"] = "missing"
        with self.assertRaises(TagReleaseError):
            active_signer(json.dumps(document))

    def test_signature_fingerprints_read_the_good_signature(self) -> None:
        status = (
            "[GNUPG:] NEWSIG\n"
            "[GNUPG:] GOODSIG 680119C224E973B1 MCP C++ SDK Release\n"
            "[GNUPG:] VALIDSIG C017D850B03960E8EF1951EC680119C224E973B1 2026-11-19 1795046784 0 4 0 22 10 00 "
            "32F4760A898CAA62344F81B078DDAF6A80105366\n"
        )
        self.assertEqual(
            signature_fingerprints(status),
            ("C017D850B03960E8EF1951EC680119C224E973B1", "32F4760A898CAA62344F81B078DDAF6A80105366"),
        )
        self.assertIsNone(signature_fingerprints("[GNUPG:] BADSIG 680119C224E973B1 MCP C++ SDK Release\n"))

    def test_only_a_successful_dry_run_of_the_commit_counts(self) -> None:
        def entry(title: str, sha: str, conclusion: str) -> dict[str, str]:
            return {"displayTitle": title, "headSha": sha, "conclusion": conclusion, "url": f"{title}/{sha}"}

        runs = [
            entry("release v0.3.0 (dry-run)", "0" * 40, "success"),
            entry("release v0.3.0 (dry-run)", COMMIT, "failure"),
            entry("release v0.3.0 (publish-all)", COMMIT, "success"),
            entry("release v0.2.0 (dry-run)", COMMIT, "success"),
        ]
        self.assertIsNone(successful_dry_run(json.dumps(runs), "v0.3.0", COMMIT))
        runs.append(entry("release v0.3.0 (dry-run)", COMMIT, "success"))
        self.assertEqual(successful_dry_run(json.dumps(runs), "v0.3.0", COMMIT), f"release v0.3.0 (dry-run)/{COMMIT}")

    def test_a_stable_release_publishes_the_variable_channels(self) -> None:
        self.assertEqual(
            release_dispatch(SemVer(0, 4, 0), "chocolatey apt  rpm\n"),
            ["-f", "tag=v0.4.0", "-f", "operation=publish-selected",
             "-f", "apt=true", "-f", "rpm=true", "-f", "chocolatey=true"],
        )
        for value in (None, "", "  ", "apt conan", "apt apt"):
            with self.subTest(value=value), self.assertRaises(TagReleaseError):
                release_dispatch(SemVer(0, 4, 0), value)

    def test_release_candidates_publish_only_the_github_release(self) -> None:
        self.assertEqual(
            release_dispatch(SemVer(0, 4, 0, rc=1), None),
            ["-f", "tag=v0.4.0-rc.1", "-f", "operation=publish-selected"],
        )

    def test_channels_are_the_dispatch_contract_channels(self) -> None:
        contract = ast.parse((ROOT / "scripts/release_dispatch_contract.py").read_text(encoding="utf-8"))
        channels = next(
            ast.literal_eval(node.value)
            for node in contract.body
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "EXTERNAL_CHANNELS" for target in node.targets)
        )
        self.assertEqual(CHANNELS, channels)

    def test_confirm_needs_an_explicit_yes(self) -> None:
        for reply, expected in (("y", True), ("YES", True), ("", False), ("n", False), ("yep", False)):
            with self.subTest(reply=reply):
                self.assertEqual(confirm("Push?", lambda _prompt: reply), expected)


if __name__ == "__main__":
    unittest.main()
