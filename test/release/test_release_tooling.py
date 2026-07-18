from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from uuid import UUID
import xml.etree.ElementTree as ET
import zipfile

from release.artifacts import (
    ArtifactRecord,
    SourceInventory,
    build_release_manifest,
    build_sboms,
    build_sha256sums,
    build_source_archives,
    canonical_json_bytes,
    sha256_file,
)
from release.model import DispatchRequest, ReleaseLedger, SemVer, ValidationError
from release.policy import ArtifactIdentity, IdempotencyDecision, decide_idempotency
from release.templates import render_file, render_template
from release.verify_gnupg_status import validate_status


ZERO_SHA = "0" * 40
ONE_DIGEST = "1" * 64
TWO_DIGEST = "2" * 64
PRIMARY = "A" * 40
SUBKEY = "B" * 40


class SemVerTests(unittest.TestCase):
    def test_accepts_stable_and_rc(self) -> None:
        self.assertEqual(str(SemVer.parse("0.2.0")), "0.2.0")
        self.assertEqual(SemVer.from_tag("v0.2.0-rc.7").rc, 7)

    def test_native_version_mappings_keep_rc_distinct(self) -> None:
        stable = SemVer.parse("0.2.0")
        rc = SemVer.parse("0.2.0-rc.1")
        self.assertEqual(stable.debian_version, "0.2.0-1")
        self.assertEqual(rc.debian_version, "0.2.0~rc.1-1")
        self.assertEqual(stable.rpm_version_release, ("0.2.0", "1"))
        self.assertEqual(rc.rpm_version_release, ("0.2.0", "0.1.rc.1"))
        self.assertEqual(stable.arch_pkgver, "0.2.0")
        self.assertEqual(rc.arch_pkgver, "0.2.0rc1")

    def test_rejects_noncanonical_versions(self) -> None:
        for value in ("v0.2.0", "01.2.3", "1.2", "1.2.3-rc.0", "1.2.3+build", "1.2.3\n"):
            with self.subTest(value=value), self.assertRaises(ValidationError):
                SemVer.parse(value)

    def test_stable_only_rejects_rc(self) -> None:
        with self.assertRaises(ValidationError):
            SemVer.parse("0.2.0-rc.1", stable_only=True)


class DispatchTests(unittest.TestCase):
    def request(self) -> dict[str, str]:
        return {
            "source_tag": "v0.2.0",
            "source_commit_sha": ZERO_SHA,
            "github_release_id": "123",
            "source_workflow_run_id": "456",
            "release_manifest_sha256": ONE_DIGEST,
            "fork_branch": "package/mcp-cpp-sdk-0.2.0",
            "fork_head_sha": ZERO_SHA,
            "recipe_tree_sha256": TWO_DIGEST,
            "request_uuid": str(UUID(int=1)),
        }

    def test_accepts_exact_request_and_recovery_branch(self) -> None:
        self.assertEqual(DispatchRequest.from_mapping(self.request()).github_release_id, "123")
        recovery = self.request()
        recovery["fork_branch"] += "-r2"
        DispatchRequest.from_mapping(recovery)

    def test_rejects_extra_field_and_branch_mismatch(self) -> None:
        extra = self.request()
        extra["url"] = "https://example.invalid"
        with self.assertRaises(ValidationError):
            DispatchRequest.from_mapping(extra)
        mismatch = self.request()
        mismatch["fork_branch"] = "package/mcp-cpp-sdk-0.3.0"
        with self.assertRaises(ValidationError):
            DispatchRequest.from_mapping(mismatch)

    def test_rejects_rc_noncanonical_hash_and_whitespace(self) -> None:
        for field, value in (
            ("source_tag", "v0.2.0-rc.1"),
            ("source_commit_sha", "A" * 40),
            ("github_release_id", "01"),
            ("fork_branch", "package/mcp-cpp-sdk-0.2.0\n"),
        ):
            request = self.request()
            request[field] = value
            with self.subTest(field=field), self.assertRaises(ValidationError):
                DispatchRequest.from_mapping(request)


class LedgerTests(unittest.TestCase):
    def ledger(self, version: str = "0.2.0") -> dict[str, object]:
        return {
            "schema_version": 1,
            "version": version,
            "tag": f"v{version}",
            "source_commit_sha": ZERO_SHA,
            "release_manifest_sha256": ONE_DIGEST,
            "results": {
                "github": "PUBLISHED",
                "conan2": "SUBMITTED_PENDING_REVIEW",
                "deb_apt": "LIVE",
                "rpm": "LIVE",
                "arch_aur": "LIVE",
                "homebrew": "LIVE",
                "chocolatey": "SUBMITTED_PENDING_MODERATION",
            },
        }

    def test_round_trips_complete_ledger(self) -> None:
        ledger = ReleaseLedger.from_mapping(self.ledger())
        self.assertEqual(ledger.to_mapping(), self.ledger())

    def test_rc_cannot_claim_downstream_publication(self) -> None:
        with self.assertRaises(ValidationError):
            ReleaseLedger.from_mapping(self.ledger("0.2.0-rc.1"))

    def test_downstream_publication_requires_github_anchor(self) -> None:
        value = self.ledger()
        value["results"]["github"] = "FAILED"  # type: ignore[index]
        with self.assertRaises(ValidationError):
            ReleaseLedger.from_mapping(value)


class ArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        (self.root / "VERSION").write_text("0.2.0\n", encoding="utf-8")
        (self.root / "include").mkdir()
        (self.root / "include" / "sdk.hpp").write_text("#pragma once\n", encoding="utf-8")
        self.inventory = SourceInventory(["VERSION", "include/**"])

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_inventory_rejects_traversal_and_symlink(self) -> None:
        with self.assertRaises(ValidationError):
            SourceInventory(["../secret"])
        (self.root / "include" / "link.hpp").symlink_to(self.root / "VERSION")
        with self.assertRaises(ValidationError):
            self.inventory.expand(self.root)

    def test_archives_are_byte_reproducible_and_normalized(self) -> None:
        first = Path(self.temporary.name) / "first"
        second = Path(self.temporary.name) / "second"
        first_paths = build_source_archives(
            root=self.root, inventory=self.inventory, version=SemVer.parse("0.2.0"), output_dir=first, source_date_epoch=1_700_000_000
        )
        second_paths = build_source_archives(
            root=self.root, inventory=self.inventory, version=SemVer.parse("0.2.0"), output_dir=second, source_date_epoch=1_700_000_000
        )
        self.assertEqual([sha256_file(path) for path in first_paths], [sha256_file(path) for path in second_paths])
        with tarfile.open(first_paths[0], "r:gz") as archive:
            members = archive.getmembers()
            self.assertEqual([item.name for item in members], sorted(item.name for item in members))
            self.assertTrue(all(item.uid == 0 and item.gid == 0 and item.mtime == 1_700_000_000 for item in members))
        with zipfile.ZipFile(first_paths[1]) as archive:
            self.assertEqual(archive.namelist(), sorted(archive.namelist()))

    def test_archive_version_must_match_embedded_version(self) -> None:
        with self.assertRaises(ValidationError):
            build_source_archives(
                root=self.root,
                inventory=self.inventory,
                version=SemVer.parse("0.3.0"),
                output_dir=Path(self.temporary.name) / "mismatch",
                source_date_epoch=1_700_000_000,
            )

    def test_manifest_and_checksum_chain_excludes_self_reference(self) -> None:
        payload = self.root / "VERSION"
        record = ArtifactRecord.from_path(payload, role="canonical-source")
        manifest = build_release_manifest(
            version=SemVer.parse("0.2.0"),
            tag="v0.2.0",
            commit=ZERO_SHA,
            source_tree_sha256=ONE_DIGEST,
            ledger_issue_id="1",
            ledger_issue_url="https://github.com/yurirocha15/mcp-cpp-sdk/issues/1",
            primary_fingerprint=PRIMARY,
            tag_subkey_fingerprint="C" * 40,
            artifact_subkey_fingerprint=SUBKEY,
            payloads=[record],
            dependency_closure=[],
            provenance_subjects=[],
        )
        manifest_path = Path(self.temporary.name) / "release-manifest.json"
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        checksums = build_sha256sums([payload, manifest_path]).decode()
        self.assertIn("VERSION", checksums)
        self.assertIn("release-manifest.json", checksums)
        with self.assertRaises(ValidationError):
            build_sha256sums([Path(self.temporary.name) / "SHA256SUMS"])

    def test_sboms_are_deterministic_and_bind_each_file(self) -> None:
        files = self.inventory.expand(self.root)
        first = build_sboms(
            version=SemVer.parse("0.2.0"), files=files, root=self.root, source_date_epoch=1_700_000_000, namespace_base="https://example.invalid/sbom"
        )
        second = build_sboms(
            version=SemVer.parse("0.2.0"), files=files, root=self.root, source_date_epoch=1_700_000_000, namespace_base="https://example.invalid/sbom"
        )
        self.assertEqual(first, second)
        self.assertEqual(len(first[0]["files"]), 2)
        self.assertEqual(len(first[1]["components"]), 2)
        self.assertEqual(first[0]["packages"][0]["licenseDeclared"], "Apache-2.0")
        self.assertEqual(
            first[1]["metadata"]["component"]["licenses"][0]["license"]["id"],
            "Apache-2.0",
        )


class PolicyTests(unittest.TestCase):
    def identity(self, digest: str = ONE_DIGEST) -> ArtifactIdentity:
        return ArtifactIdentity.create(
            destination="chocolatey",
            coordinates={"feed": "community", "package_id": "mcp-cpp-sdk", "version": "0.2.0"},
            sha256=digest,
            manifest_sha256=TWO_DIGEST,
        )

    def test_idempotency_is_fail_closed(self) -> None:
        local = self.identity()
        self.assertEqual(decide_idempotency(local, []), IdempotencyDecision.PUBLISH)
        self.assertEqual(decide_idempotency(local, [local]), IdempotencyDecision.SKIP_IDENTICAL)
        self.assertEqual(decide_idempotency(local, [self.identity("3" * 64)]), IdempotencyDecision.BLOCK_CONFLICT)
        self.assertEqual(decide_idempotency(local, [local, local]), IdempotencyDecision.BLOCK_AMBIGUOUS)

    def test_identity_requires_exact_target_coordinates(self) -> None:
        with self.assertRaises(ValidationError):
            ArtifactIdentity.create(
                destination="chocolatey", coordinates={"package_id": "mcp-cpp-sdk", "version": "0.2.0"}, sha256=ONE_DIGEST, manifest_sha256=TWO_DIGEST
            )


class SigningStatusTests(unittest.TestCase):
    def valid_status(self) -> str:
        return (
            "[GNUPG:] NEWSIG\n"
            f"[GNUPG:] GOODSIG 0123 User\n"
            f"[GNUPG:] VALIDSIG {SUBKEY} 2026-07-11 0 0 4 0 1 10 00 {PRIMARY}\n"
            "[GNUPG:] TRUST_UNDEFINED 0 pgp\n"
        )

    def test_accepts_exact_subkey_and_primary(self) -> None:
        validate_status(self.valid_status(), signing_fingerprint=SUBKEY, primary_fingerprint=PRIMARY)

    def test_rejects_wrong_role_extra_signature_and_fatal_status(self) -> None:
        for status in (
            self.valid_status().replace(SUBKEY, "C" * 40),
            self.valid_status() + self.valid_status(),
            self.valid_status() + "[GNUPG:] REVKEYSIG bad\n",
            "human readable output\n" + self.valid_status(),
        ):
            with self.subTest(status=status[:30]), self.assertRaises(ValueError):
                validate_status(status, signing_fingerprint=SUBKEY, primary_fingerprint=PRIMARY)


class TemplateTests(unittest.TestCase):
    def test_renderer_requires_exact_values(self) -> None:
        self.assertEqual(render_template("version=@VERSION@\n", {"VERSION": "0.2.0"}), "version=0.2.0\n")
        with self.assertRaises(ValidationError):
            render_template("version=@VERSION@\n", {})
        with self.assertRaises(ValidationError):
            render_template("static\n", {"VERSION": "0.2.0"})
        with self.assertRaises(ValidationError):
            render_template("version=@VERSION@\n", {"VERSION": "0.2.0\nInjected: true"})

    def test_renderer_rejects_package_syntax_injection(self) -> None:
        invalid_values = (
            ("SOURCE_URL", "https://example.invalid/file' && curl bad"),
            ("SOURCE_SHA256", "A" * 64),
            ("PRIMARY_FINGERPRINT", "A" * 39),
            ("MAINTAINER", "Name <mail@example.invalid>\nUploaders: attacker"),
            ("DEBIAN_DISTRIBUTION", "stable; touch bad"),
        )
        for name, value in invalid_values:
            with self.subTest(name=name), self.assertRaises(ValidationError):
                render_template(f"value=@{name}@\n", {name: value})

    def test_template_inventory_matches_tokens(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        packaging = repository / "packaging"
        inventory = json.loads((packaging / "templates.json").read_text(encoding="utf-8"))
        for relative, tokens in inventory.items():
            text = (packaging / relative).read_text(encoding="utf-8")
            actual = sorted(set(__import__("re").findall(r"@([A-Z][A-Z0-9_]*)@", text)))
            self.assertEqual(actual, sorted(tokens), relative)

    def test_package_licenses_and_split_exports_are_consistent(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        packaging = repository / "packaging"
        for relative in (
            "aur/PKGBUILD.in",
            "homebrew/mcp-cpp-sdk.rb.in",
            "rpm/mcp-cpp-sdk.spec.in",
            "chocolatey/mcp-cpp-sdk.nuspec.in",
        ):
            text = (packaging / relative).read_text(encoding="utf-8")
            self.assertIn("Apache-2.0", text, relative)
            self.assertNotIn("MIT", text, relative)

        aur = (packaging / "aur/PKGBUILD.in").read_text(encoding="utf-8")
        self.assertIn("options=('!debug' 'staticlibs')", aur)

        debian_rules = (packaging / "debian/rules.in").read_text(encoding="utf-8")
        self.assertIn("CTEST_OUTPUT_ON_FAILURE=1 dh_auto_test", debian_rules)
        self.assertNotIn("ARGS=", debian_rules)
        self.assertEqual(
            (packaging / "debian/not-installed.in").read_text(encoding="utf-8"),
            "usr/share/licenses/mcp-cpp-sdk/LICENSE\n",
        )

        debian_devel = (packaging / "debian/libmcp-cpp-sdk-dev.install.in").read_text(
            encoding="utf-8"
        )
        debian_static = (
            packaging / "debian/libmcp-cpp-sdk-static-dev.install.in"
        ).read_text(encoding="utf-8")
        self.assertIn("shared-targets", debian_devel)
        self.assertNotIn("static-targets", debian_devel)
        self.assertIn("static-targets", debian_static)

    def test_package_templates_render_and_parse_offline(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        packaging = repository / "packaging"
        inventory = json.loads((packaging / "templates.json").read_text(encoding="utf-8"))
        values = {
            "VERSION": "0.2.0",
            "DEBIAN_VERSION": "0.2.0-1",
            "DEBIAN_DISTRIBUTION": "noble",
            "MAINTAINER": "MCP C++ SDK Maintainers <maintainer@example.invalid>",
            "RFC2822_DATE": "Sat, 11 Jul 2026 00:00:00 +0000",
            "COPYRIGHT": "2026 MCP C++ SDK Contributors",
            "RPM_VERSION": "0.2.0",
            "RPM_RELEASE": "1",
            "RPM_CHANGELOG_DATE": "Sat Jul 11 2026",
            "SOURCE_URL": "https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/v0.2.0/mcp-cpp-sdk-0.2.0.tar.gz",
            "SOURCE_SHA256": "1" * 64,
            "PKGREL": "1",
            "SOURCE_SIGNATURE_URL": "https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/v0.2.0/mcp-cpp-sdk-0.2.0.tar.gz.asc",
            "PRIMARY_FINGERPRINT": "A" * 40,
            "ARTIFACT_SUBKEY_FINGERPRINT": "B" * 40,
            "AUTHORS": "MCP C++ SDK Contributors",
            "OWNERS": "MCP C++ SDK Maintainers",
            "SUMMARY": "C++20 Model Context Protocol SDK",
            "DESCRIPTION": "C++20 MCP SDK & tools",
            "PROJECT_URL": "https://github.com/yurirocha15/mcp-cpp-sdk",
            "PACKAGE_SOURCE_URL": "https://github.com/yurirocha15/mcp-cpp-sdk",
            "TAGS": "mcp cpp20 sdk",
            "ARCHIVE_URL": "https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/v0.2.0/mcp-cpp-sdk-0.2.0-windows-x64-v143-md.zip",
            "ARCHIVE_SHA256": "2" * 64,
        }
        with tempfile.TemporaryDirectory() as directory:
            output_root = Path(directory)
            for relative, required in inventory.items():
                destination = output_root / relative.removesuffix(".in")
                render_file(
                    packaging / relative,
                    destination,
                    {name: values[name] for name in required},
                )

            ET.parse(output_root / "chocolatey/mcp-cpp-sdk.nuspec")
            compile(
                (output_root / "conan-center/all/conanfile.py").read_text(encoding="utf-8"),
                "conanfile.py",
                "exec",
            )
            compile(
                (output_root / "conan-center/all/test_package/conanfile.py").read_text(
                    encoding="utf-8"
                ),
                "test_package/conanfile.py",
                "exec",
            )
            if shutil.which("ruby"):
                subprocess.run(
                    ["ruby", "-c", str(output_root / "homebrew/mcp-cpp-sdk.rb")],
                    check=True,
                    capture_output=True,
                    text=True,
                )
            if shutil.which("dpkg-parsechangelog"):
                subprocess.run(
                    ["dpkg-parsechangelog", "-l", str(output_root / "debian/changelog")],
                    check=True,
                    capture_output=True,
                    text=True,
                )


if __name__ == "__main__":
    unittest.main()
