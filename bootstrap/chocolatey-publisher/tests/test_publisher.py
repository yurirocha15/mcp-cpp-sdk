from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from uuid import UUID
import zipfile

from publisher.scan_repository_secrets import (
    SENSITIVE_PATTERN,
    find_sensitive_paths,
    tracked_files,
)
from publisher.source_run import poll, verify_release as verify_github_release, verify_repository
from publisher.validate_dispatch import validate
from publisher import verify_release
from publisher.verify_release import _verify_nupkg


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github/workflows"
ACTION = re.compile(r"uses:\s*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)@([0-9a-f]{40})(?:\s|$)")


class DispatchTests(unittest.TestCase):
    def request(self) -> dict[str, str]:
        return {
            "source_tag": "v0.2.0",
            "source_commit_sha": "0" * 40,
            "source_workflow_head_sha": "2" * 40,
            "provider_control_sha": "3" * 40,
            "github_release_id": "1",
            "source_workflow_run_id": "2",
            "release_manifest_sha256": "1" * 64,
            "request_uuid": str(UUID(int=1)),
        }

    def test_exact_stable_dispatch(self) -> None:
        self.assertEqual(validate(self.request()), self.request())

    def test_rejects_rc_extra_and_noncanonical_values(self) -> None:
        for field, value in (
            ("source_tag", "v0.2.0-rc.1"),
            ("source_commit_sha", "A" * 40),
            ("github_release_id", "01"),
            ("extra", "value"),
        ):
            request = self.request()
            request[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate(request)


class SourceRunTests(unittest.TestCase):
    def test_binds_source_identity_and_waits_for_completion(self) -> None:
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
        verify_github_release(
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
        clock = [0.0]
        responses = iter(
            [
                {"status": "queued"},
                {
                    "id": 6,
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
        result = poll(
            lambda: next(responses),
            run_id="6",
            workflow_head_sha="a" * 40,
            repository_id="9",
            timeout_seconds=3600,
            monotonic=lambda: clock[0],
            sleep=lambda seconds: clock.__setitem__(0, clock[0] + seconds),
        )
        self.assertEqual(result["conclusion"], "success")
        self.assertEqual(clock[0], 20)


class PackageTests(unittest.TestCase):
    def test_nupkg_binds_archive_checksum_and_sdk_root(self) -> None:
        version = "0.2.0"
        tag = f"v{version}"
        archive = f"mcp-cpp-sdk-{version}-windows-x64-v143-md.zip"
        digest = "a" * 64
        nuspec = """<?xml version="1.0"?>
<package xmlns="http://schemas.microsoft.com/packaging/2015/06/nuspec.xsd">
  <metadata>
    <id>mcp-cpp-sdk</id><version>0.2.0</version>
    <license type="expression">Apache-2.0</license>
  </metadata>
</package>
"""
        install = (
            f"Get-ChocolateyWebFile -Url64bit 'https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/{tag}/{archive}' "
            f"-Checksum64 '{digest}'\n"
            '$stagingDir = "$installDir.installing"\n'
            "Get-ChocolateyUnzip -FileFullPath $archive -Destination $stagingDir\n"
            "Remove-Item -LiteralPath $installDir -Recurse -Force\n"
            "Move-Item -LiteralPath $stagingDir -Destination $installDir\n"
            "MCP_CPP_SDK_ROOT\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / f"mcp-cpp-sdk.{version}.nupkg"
            with zipfile.ZipFile(package, "w") as output:
                output.writestr("mcp-cpp-sdk.nuspec", nuspec)
                output.writestr("tools/chocolateyinstall.ps1", install)
                output.writestr("tools/chocolateyuninstall.ps1", "uninstall")
                output.writestr("LICENSE.txt", "license")
                output.writestr("VERIFICATION.txt", "verification")
            _verify_nupkg(
                package,
                version=version,
                tag=tag,
                records={archive: {"sha256": digest}},
            )
            with zipfile.ZipFile(package, "a") as output:
                output.writestr("../escape", "bad")
            with self.assertRaises(ValueError):
                _verify_nupkg(
                    package,
                    version=version,
                    tag=tag,
                    records={archive: {"sha256": digest}},
                )

    def test_nupkg_rejects_install_that_unzips_over_a_stale_sdk(self) -> None:
        version = "0.2.0"
        tag = f"v{version}"
        archive = f"mcp-cpp-sdk-{version}-windows-x64-v143-md.zip"
        digest = "a" * 64
        nuspec = """<?xml version="1.0"?>
<package xmlns="http://schemas.microsoft.com/packaging/2015/06/nuspec.xsd">
  <metadata><id>mcp-cpp-sdk</id><version>0.2.0</version>
  <license type="expression">Apache-2.0</license></metadata>
</package>
"""
        unsafe = (
            f"https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/{tag}/{archive}\n"
            f"-Checksum64 '{digest}'\nMCP_CPP_SDK_ROOT\n"
            "Get-ChocolateyUnzip -FileFullPath $archive -Destination $installDir\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory) / f"mcp-cpp-sdk.{version}.nupkg"
            with zipfile.ZipFile(package, "w") as output:
                output.writestr("mcp-cpp-sdk.nuspec", nuspec)
                output.writestr("tools/chocolateyinstall.ps1", unsafe)
                output.writestr("tools/chocolateyuninstall.ps1", "uninstall")
                output.writestr("LICENSE.txt", "license")
                output.writestr("VERIFICATION.txt", "verification")
            with self.assertRaisesRegex(ValueError, "does not bind"):
                _verify_nupkg(
                    package,
                    version=version,
                    tag=tag,
                    records={archive: {"sha256": digest}},
                )

    def test_rejects_legacy_incomplete_manifest_before_payload_use(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            key = root / "trusted.asc"
            key.write_text("public key\n", encoding="utf-8")
            (root / "release-signing-key.asc").write_bytes(key.read_bytes())
            manifest = root / "release-manifest.json"
            manifest.write_text(
                json.dumps({"schema_version": 1, "tag": "v0.2.0", "version": "0.2.0", "commit": "0" * 40}),
                encoding="utf-8",
            )
            with patch.object(verify_release, "_verify_key"), self.assertRaisesRegex(
                ValueError, "complete stable channel set"
            ):
                verify_release.verify(
                    root,
                    tag="v0.2.0",
                    commit="0" * 40,
                    manifest_sha256=verify_release.sha256(manifest),
                    primary_fingerprint="A" * 40,
                    artifact_fingerprint="B" * 40,
                    public_key=key,
                )


class WorkflowPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = (WORKFLOWS / "publish.yml").read_text(encoding="utf-8")
        self.manifest = json.loads((ROOT / "repository-settings.json").read_text(encoding="utf-8"))

    def test_actions_are_full_sha_allowlisted(self) -> None:
        allowed = self.manifest["actions"]["allowed_actions"]
        for path in WORKFLOWS.glob("*.yml"):
            for line in path.read_text(encoding="utf-8").splitlines():
                if "uses:" not in line:
                    continue
                match = ACTION.search(line)
                self.assertIsNotNone(match, line)
                self.assertEqual(allowed.get(match.group(1)), match.group(2))

    def test_api_key_is_one_final_no_checkout_step(self) -> None:
        publish = self.workflow.split("\n  publish:\n", 1)[1].split("\n  record:\n", 1)[0]
        self.assertNotIn("actions/checkout", publish)
        self.assertNotIn("publisher/", publish)
        self.assertEqual(publish.count("secrets.CHOCOLATEY_API_KEY"), 1)
        self.assertIn("EXPECTED_CHOCOLATEY_VERSION: '2.7.3'", publish)
        self.assertNotIn("choco upgrade", publish.lower())
        self.assertNotIn("community.chocolatey.org", publish)
        self.assertIn("& submission/push_package.ps1 -Mode Preflight", publish)
        self.assertTrue(
            publish.rstrip().endswith("run: '& submission/push_package.ps1 -Mode Publish'")
        )
        self.assertLess(
            publish.index("-Mode Preflight"), publish.index("secrets.CHOCOLATEY_API_KEY")
        )
        self.assertEqual(publish.count("EXPECTED_PUBLISHER_CLIENT_SHA256"), 2)
        self.assertEqual(publish.count("EXPECTED_IDENTITY_SHA256"), 1)

    def test_protected_push_client_never_installs_code_before_using_the_key(self) -> None:
        client = (ROOT / "publisher/push_package.ps1").read_text(encoding="utf-8")
        lowered = client.lower()
        for forbidden in (
            "choco upgrade",
            "choco install",
            "community.chocolatey.org",
            "invoke-webrequest",
            "invoke-restmethod",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, lowered)
        for required in (
            "Get-TrustedChocolatey",
            "Get-AuthenticodeSignature",
            "$signature.Status -cne 'Valid'",
            "CN=\"Chocolatey Software, Inc\"",
            "B009C875F4E10FFBC62B785BAF4FC4D6BC2D5711",
            "EXPECTED_CHOCOLATEY_VERSION",
            "EXPECTED_PUBLISHER_CLIENT_SHA256",
            "EXPECTED_IDENTITY_SHA256",
            "Get-FileHash",
            "$Mode -eq 'Preflight'",
            "& $choco push",
            "https://push.chocolatey.org/",
        ):
            with self.subTest(required=required):
                self.assertIn(required, client)
        self.assertEqual(
            self.manifest["workflow_constants"]["chocolatey_cli_signer_thumbprint"],
            "B009C875F4E10FFBC62B785BAF4FC4D6BC2D5711",
        )

    def test_claim_and_record_are_credential_free(self) -> None:
        claim = self.workflow.split("\n  claim:\n", 1)[1].split("\n  publish:\n", 1)[0]
        record = self.workflow.split("\n  record:\n", 1)[1]
        self.assertNotIn("secrets.", claim)
        self.assertNotIn("secrets.", record)
        self.assertIn("publisher/claim_controller.py manage", claim)
        self.assertIn("publisher/claim_controller.py record", record)
        self.assertIn("CHOCOLATEY_SUBMISSION_AUTOMATION_USER_ID", claim)

    def test_python_is_external_and_handoff_is_attempt_scoped(self) -> None:
        self.assertNotIn("python3 -I -S <<", self.workflow)
        self.assertNotIn("publisher/manage_claim.sh", self.workflow)
        self.assertNotIn("publisher/record_claim.sh", self.workflow)
        self.assertEqual(list((ROOT / "publisher").glob("*.sh")), [])
        self.assertIn("publisher/handoff.py", self.workflow)
        self.assertIn("chocolatey-submission-{run_id}-{run_attempt}",
                      (ROOT / "publisher/handoff.py").read_text())
        self.assertIn("name: ${{ steps.handoff.outputs.artifact_name }}", self.workflow)
        self.assertIn("name: ${{ needs.verify.outputs.artifact_name }}", self.workflow)
        self.assertIn("retention-days: 30", self.workflow)

    def test_source_run_has_hour_scale_bounded_polling(self) -> None:
        self.assertIn("timeout-minutes: 75", self.workflow)
        self.assertIn("publisher/source_run.py", self.workflow)
        self.assertIn("--timeout-seconds 3600", self.workflow)
        self.assertNotIn("for _ in $(seq", self.workflow)


class SecretScanTests(unittest.TestCase):
    def test_detector_recognizes_each_forbidden_shape(self) -> None:
        samples = (
            b"-----BEGIN PRIVATE" b" KEY-----",
            b"ghp_" + b"a" * 20,
            b"CHOCOLATEY_" b"API_KEY=secret",
        )
        for sample in samples:
            with self.subTest(sample=sample[:16]):
                self.assertIsNotNone(SENSITIVE_PATTERN.search(sample))

    def test_tracked_repository_does_not_match_its_own_scanner(self) -> None:
        self.assertEqual(find_sensitive_paths(ROOT), [])

    def test_tracked_sensitive_file_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            (root / "leak.txt").write_bytes(b"CHOCOLATEY_" b"API_KEY=secret")
            subprocess.run(["git", "-C", str(root), "add", "leak.txt"], check=True)
            self.assertEqual(find_sensitive_paths(root), [Path("leak.txt")])

    def test_unsafe_path_error_escapes_control_characters(self) -> None:
        result = subprocess.CompletedProcess([], 0, stdout=b"../bad\n\x1b[31m\0")
        with (
            patch("publisher.scan_repository_secrets.subprocess.run", return_value=result),
            self.assertRaises(ValueError) as context,
        ):
            tracked_files(ROOT)
        message = str(context.exception)
        self.assertNotIn("\n", message)
        self.assertNotIn("\x1b", message)
        self.assertIn(r"\n\x1b", message)


if __name__ == "__main__":
    unittest.main()
