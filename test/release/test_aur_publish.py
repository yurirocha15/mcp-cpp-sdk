from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest import mock

from release.aur_preflight import AurPreflightError, verify_public_state
from release.aur_publish import verify_existing_version, verify_tree_inventory
from release.construct_core import _aur_srcinfo
from release.model import SemVer, ValidationError
from release.publication_contract import PublicationContract, require_channel_target


ROOT = Path(__file__).resolve().parents[2]


class AurPublishPolicyTests(unittest.TestCase):
    def test_signed_contract_binds_exact_aur_package_and_repository(self) -> None:
        contract = mock.Mock(spec=PublicationContract)
        contract.channel_targets = {
            "aur": {"package_base": "mcp-cpp-sdk", "architectures": ["x86_64"]}
        }
        observed = {
            "package_base": "mcp-cpp-sdk",
            "architectures": ["x86_64"],
            "repository": "ssh://aur@aur.archlinux.org/mcp-cpp-sdk.git",
        }
        with mock.patch(
            "release.publication_contract.load_publication_contract",
            return_value=contract,
        ):
            require_channel_target(Path("unused"), name="aur", observed=observed)
            for field, value in (
                ("package_base", "attacker-target"),
                ("architectures", ["aarch64"]),
                ("repository", "ssh://aur@aur.archlinux.org/attacker-target.git"),
            ):
                mutated = dict(observed)
                mutated[field] = value
                with self.subTest(field=field), self.assertRaises(ValidationError):
                    require_channel_target(
                        Path("unused"), name="aur", observed=mutated
                    )

    def test_existing_version_rejects_newer_or_malformed_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "PKGBUILD"
            path.write_text("pkgver=0.3.0\n", encoding="utf-8")
            with self.assertRaises(ValidationError):
                verify_existing_version(path, "0.2.0")
            path.write_text("pkgver=${VERSION}\n", encoding="utf-8")
            with self.assertRaises(ValidationError):
                verify_existing_version(path, "0.2.0")

    def test_missing_or_older_metadata_is_safe_to_advance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "PKGBUILD"
            verify_existing_version(path, "0.2.0")
            path.write_text("pkgver=0.1.9\n", encoding="utf-8")
            verify_existing_version(path, "0.2.0")

    def test_tree_inventory_is_exact_and_rejects_stale_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "tree-files"
            path.write_text(".SRCINFO\nPKGBUILD\n", encoding="utf-8")
            verify_tree_inventory(path)
            for value in (
                "PKGBUILD\n.SRCINFO\n",
                ".SRCINFO\nPKGBUILD\nREADME.md\n",
                ".SRCINFO\n",
            ):
                path.write_text(value, encoding="utf-8")
                with self.subTest(value=value), self.assertRaises(ValidationError):
                    verify_tree_inventory(path)

    def test_shell_adapter_drops_key_material_and_verifies_remote_commit(self) -> None:
        script = (ROOT / "scripts/release/publish_aur.sh").read_text(encoding="utf-8")
        loader = (ROOT / "scripts/release/aur_ssh_agent.sh").read_text(encoding="utf-8")
        decode = loader.index("base64 --decode")
        unset_key = loader.index("unset AUR_SSH_PRIVATE_KEY_B64")
        add = loader.index('ssh-add "${AUR_SSH_PRIVATE_KEY_FILE}"')
        remove = loader.index('rm -f "${AUR_SSH_PRIVATE_KEY_FILE}"', add)
        first_git = script.index("git clone")
        self.assertLess(decode, unset_key)
        self.assertLess(add, remove)
        self.assertIn("aur_ssh_setup aur-publish", script[:first_git])
        self.assertIn("HostKeyAlgorithms=ssh-ed25519", loader)
        self.assertIn("PasswordAuthentication=no", loader)
        self.assertIn("release.aur_ssh", loader)
        self.assertNotIn("checkout HEAD -- . || true", script)
        self.assertIn("verify_remote_state", script)
        self.assertIn("ls-remote --refs origin refs/heads/master", script)
        self.assertIn("refs/remotes/origin/release-verify^{tree}", script)
        self.assertIn("ls-tree -r --name-only refs/remotes/origin/release-verify", script)
        self.assertIn("--tree-inventory remote-tree-files", script)
        self.assertIn("cmp --silent release-assets/aur-PKGBUILD", script)
        self.assertIn("cmp --silent release-assets/aur-SRCINFO", script)

    def test_read_only_preflight_uses_public_https_without_credentials(self) -> None:
        script = (ROOT / "scripts/release/preflight_aur.sh").read_text(encoding="utf-8")
        module = (ROOT / "release/aur_preflight.py").read_text(encoding="utf-8")
        self.assertIn("release.aur_preflight", script)
        self.assertIn("git\", \"ls-remote\", \"--refs", module)
        self.assertIn("https://aur.archlinux.org/rpc/v5/info", module)
        for mutation in ("AUR_SSH_PRIVATE_KEY", "git push", "git commit", "git clone"):
            self.assertNotIn(mutation, script + module)

    def test_authenticated_preflight_runs_only_the_documented_help_command(self) -> None:
        script = (ROOT / "scripts/release/preflight_aur_auth.sh").read_text(encoding="utf-8")
        self.assertIn("aur_ssh_setup aur-preflight", script)
        self.assertIn("aur@aur.archlinux.org help", script)
        self.assertIn(">/dev/null 2>&1", script)
        for mutation in ("git push", "git commit", "git clone", " vote ", " unvote "):
            self.assertNotIn(mutation, script)

    def test_read_only_preflight_accepts_absent_first_use_or_one_existing_ref(self) -> None:
        absent = {"version": 5, "type": "multiinfo", "resultcount": 0, "results": []}
        self.assertEqual(verify_public_state("mcp-cpp-sdk", "", absent), "absent")
        existing = {
            "version": 5,
            "type": "multiinfo",
            "resultcount": 1,
            "results": [{"Name": "mcp-cpp-sdk", "PackageBase": "mcp-cpp-sdk"}],
        }
        self.assertEqual(
            verify_public_state(
                "mcp-cpp-sdk", f"{'a' * 40}\trefs/heads/master\n", existing
            ),
            "existing",
        )

    def test_read_only_preflight_rejects_malformed_or_disagreeing_views(self) -> None:
        absent = {"version": 5, "type": "multiinfo", "resultcount": 0, "results": []}
        existing = {
            "version": 5,
            "type": "multiinfo",
            "resultcount": 1,
            "results": [{"Name": "mcp-cpp-sdk", "PackageBase": "mcp-cpp-sdk"}],
        }
        for remote, rpc in (
            (f"{'a' * 39}\trefs/heads/master\n", existing),
            (f"{'a' * 40}\trefs/heads/main\n", existing),
            (f"{'a' * 40}\trefs/heads/master\n", absent),
            ("", existing),
        ):
            with self.subTest(remote=remote, rpc=rpc), self.assertRaises(AurPreflightError):
                verify_public_state("mcp-cpp-sdk", remote, rpc)

    def test_pkgbuild_and_srcinfo_declare_python_for_signature_policy(self) -> None:
        template = (ROOT / "packaging/aur/PKGBUILD.in").read_text(encoding="utf-8")
        renderer = (ROOT / "release/construct_core.py").read_text(encoding="utf-8")
        self.assertIn("'python'", template)
        self.assertIn('makedepends = python\\n', renderer)

    def test_dependency_metadata_matches_the_public_header_interface(self) -> None:
        template = (ROOT / "packaging/aur/PKGBUILD.in").read_text(encoding="utf-8")
        self.assertIn(
            "# Maintainer: mcp-cpp-sdk maintainers <releases@yurirocha.com>",
            template,
        )
        self.assertIn(
            "depends=('boost>=1.74' 'nlohmann-json>=3.10.5' 'openssl>=3.0')",
            template,
        )
        self.assertIn(
            "makedepends=('cmake>=3.20' 'gtest' 'ninja' 'python')",
            template,
        )
        self.assertNotIn("boost-libs", template)
        self.assertEqual(template.count("boost>=1.74"), 1)

        srcinfo = _aur_srcinfo(
            version=SemVer.parse("0.2.0"),
            repository="yurirocha15/mcp-cpp-sdk",
            source_url=(
                "https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/"
                "v0.2.0/mcp-cpp-sdk-0.2.0.tar.gz"
            ),
            source_sha256="a" * 64,
            fingerprint="A" * 40,
        )
        base = srcinfo.split("\npkgname = mcp-cpp-sdk\n", 1)[0]
        self.assertEqual(
            [
                line.removeprefix("\tmakedepends = ")
                for line in base.splitlines()
                if line.startswith("\tmakedepends = ")
            ],
            ["cmake>=3.20", "gtest", "ninja", "python"],
        )
        self.assertEqual(
            [
                line.removeprefix("\tdepends = ")
                for line in base.splitlines()
                if line.startswith("\tdepends = ")
            ],
            ["boost>=1.74", "nlohmann-json>=3.10.5", "openssl>=3.0"],
        )
        self.assertNotIn("boost-libs", srcinfo)
        self.assertEqual(srcinfo.count("boost>=1.74"), 1)
        self.assertIn(
            "pkgname = mcp-cpp-sdk-static\n\tdepends = mcp-cpp-sdk=0.2.0-1\n",
            srcinfo,
        )
        self.assertIn('depends=("mcp-cpp-sdk=${pkgver}-${pkgrel}")', template)


if __name__ == "__main__":
    unittest.main()
