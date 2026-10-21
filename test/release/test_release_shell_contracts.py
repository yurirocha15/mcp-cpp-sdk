from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]

class ReleaseShellContractTests(unittest.TestCase):
    def test_signing_script_keeps_secrets_out_of_arguments_and_cleans_them(self) -> None:
        script = (ROOT / "scripts/release/sign_release.sh").read_text(encoding="utf-8")
        for fragment in (
            "set -euo pipefail",
            "set +x",
            "umask 077",
            "mktemp -d",
            "trap cleanup EXIT",
            "release.signing prepare",
            "release.signing finalize",
            "release.signing verify",
            '--local-user "${ARTIFACT_SUBKEY_FINGERPRINT}!"',
            "--passphrase-fd 0",
            "unset RELEASE_GPG_PRIVATE_KEY_B64",
            "unset RELEASE_GPG_PASSPHRASE",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, script)
        self.assertNotIn("--passphrase ${RELEASE_GPG_PASSPHRASE}", script)
        self.assertNotIn("<<'PY'", script)

    def test_aur_script_uses_module_generated_exported_symbol_consumers(self) -> None:
        script = (ROOT / "scripts/release/validate_aur_packages.sh").read_text(
            encoding="utf-8"
        )
        for fragment in (
            "collect_build_identity.py",
            "makepkg --verifysource --noconfirm",
            "makepkg --cleanbuild --noconfirm",
            "release.aur_validation packages",
            "release.aur_validation consumer",
            "readelf --dynamic --wide",
            "release.aur_validation elf",
            "sudo /usr/local/libexec/mcp-install-aur-packages",
            '"${check}/consumer-build/shared-consumer"',
            '"${check}/consumer-build/static-consumer"',
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, script)
        self.assertNotIn("<<'PY'", script)
