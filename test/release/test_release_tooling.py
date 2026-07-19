from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
from uuid import UUID
import xml.etree.ElementTree as ET
import zipfile

from release.artifacts import (
    ABI_BUILD_IDENTITY_NAME,
    ABI_BUILD_TUPLE,
    ArtifactRecord,
    NativeRoute,
    SourceInventory,
    build_release_manifest,
    build_sboms,
    build_sha256sums,
    build_source_archives,
    canonical_json_bytes,
    load_conan_requirements,
    load_native_targets,
    sha256_file,
    validate_candidate_inventory,
)
from release.build_identity import (
    APT_TARGETS,
    RPM_TARGETS,
    validate_apt_build_identity,
    validate_rpm_build_identity,
    validate_windows_build_identity,
)
from release.model import (
    RELEASE_DESTINATIONS,
    DispatchRequest,
    ReleaseLedger,
    SemVer,
    ValidationError,
)
from release.native_builder import bind_container_identity, load_builder_lock
from release.publication_contract import build_publication_contract
from release import historical_anchor
from release import verify_candidate as candidate_verifier
from release.policy import ArtifactIdentity, IdempotencyDecision, decide_idempotency
from release.templates import render_file, render_template
from release.verify_gnupg_status import validate_status


ZERO_SHA = "0" * 40
ONE_DIGEST = "1" * 64
TWO_DIGEST = "2" * 64
PRIMARY = "A" * 40
SUBKEY = "B" * 40
CONAN_REQUIREMENTS = load_conan_requirements(
    Path("packaging/conan-center/requirements.json")
)
PROVIDER_DESTINATIONS = {
    "cloudsmith": {
        "namespace": "mcp-cpp-sdk",
        "repository": "mcp-cpp-sdk",
        "preflight_username": "mcp-cpp-sdk-preflight",
        "publish_username": "mcp-cpp-sdk-publisher",
    },
    "homebrew": {
        "repository": "yurirocha15/homebrew-mcp-cpp-sdk",
        "repository_id": "11",
        "bot_login": "mcp-cpp-sdk-homebrew[bot]",
    },
    "chocolatey": {
        "repository": "yurirocha15/mcp-cpp-sdk-chocolatey-publisher",
        "repository_id": "12",
    },
    "conan_fork": {
        "repository": "yurirocha15/conan-center-index",
        "repository_id": "13",
        "upstream": "conan-io/conan-center-index",
    },
    "conan_control": {
        "repository": "yurirocha15/mcp-cpp-sdk-release-control",
        "repository_id": "14",
    },
}


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
        self.assertEqual(stable.abi_version, "0.2.0")
        self.assertEqual(SemVer.parse("2.3.4").abi_version, "2")
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
                "conan2": "DISPATCHED_PENDING_REVIEW",
                "deb_apt": "PUBLISHED",
                "rpm": "PUBLISHED",
                "arch_aur": "PUBLISHED",
                "homebrew": "DISPATCHED_PENDING_REVIEW",
                "chocolatey": "DISPATCHED_PENDING_MODERATION",
            },
        }

    def test_round_trips_complete_ledger(self) -> None:
        ledger = ReleaseLedger.from_mapping(self.ledger())
        self.assertEqual(ledger.to_mapping(), self.ledger())

    def test_tag_must_equal_v_prefixed_version(self) -> None:
        value = self.ledger()
        value["tag"] = "v0.2.1"
        with self.assertRaises(ValidationError):
            ReleaseLedger.from_mapping(value)

    def test_public_schema_encodes_anchor_and_prerelease_safety_constraints(self) -> None:
        schema = json.loads(
            Path("release/release-ledger.schema.json").read_text(encoding="utf-8")
        )
        self.assertIn("tag is exactly 'v' plus version", schema["description"])
        self.assertEqual(
            set(schema["$defs"]["public_result"]["enum"]),
            {"PUBLISHED", "SKIPPED_ALREADY_IDENTICAL"},
        )
        self.assertEqual(
            set(schema["$defs"]["non_public_result"]["enum"]),
            {"NOT_SELECTED", "BLOCKED_MANUAL_ACTION", "FAILED", "FIRST_USE_UNPROVEN"},
        )
        anchor_rule = schema["allOf"][1]
        self.assertEqual(
            anchor_rule["then"]["properties"]["release_manifest_sha256"],
            {"type": "string", "pattern": "^[0-9a-f]{64}$"},
        )
        downstream = {
            "conan2", "deb_apt", "rpm", "arch_aur", "homebrew", "chocolatey"
        }
        self.assertEqual(
            anchor_rule["else"]["properties"]["results"]["properties"],
            {
                name: {"$ref": "#/$defs/non_public_result"}
                for name in downstream
            },
        )
        rc_results = schema["allOf"][0]["then"]["properties"]["results"]["properties"]
        self.assertEqual(
            rc_results,
            {name: {"const": "NOT_SELECTED"} for name in downstream},
        )

        for result_definition in (
            "public_result",
            "non_public_result",
            "synchronous_result",
            "review_result",
            "moderation_result",
        ):
            with self.subTest(result_definition=result_definition):
                self.assertNotIn("LIVE", schema["$defs"][result_definition]["enum"])

        published_without_digest = self.ledger()
        published_without_digest["release_manifest_sha256"] = None
        downstream_without_anchor = self.ledger()
        downstream_without_anchor["results"]["github"] = "FAILED"  # type: ignore[index]
        rc_with_downstream = self.ledger("0.2.0-rc.1")
        for value in (published_without_digest, downstream_without_anchor, rc_with_downstream):
            with self.subTest(tag=value["tag"]), self.assertRaises(ValidationError):
                ReleaseLedger.from_mapping(value)

    def test_rc_cannot_claim_downstream_publication(self) -> None:
        with self.assertRaises(ValidationError):
            ReleaseLedger.from_mapping(self.ledger("0.2.0-rc.1"))

    def test_rejects_removed_live_result_for_every_destination(self) -> None:
        for destination in RELEASE_DESTINATIONS:
            value = self.ledger()
            value["results"][destination] = "LIVE"  # type: ignore[index]
            with self.subTest(destination=destination), self.assertRaises(ValidationError):
                ReleaseLedger.from_mapping(value)

    def test_downstream_publication_requires_github_anchor(self) -> None:
        value = self.ledger()
        value["results"]["github"] = "FAILED"  # type: ignore[index]
        with self.assertRaises(ValidationError):
            ReleaseLedger.from_mapping(value)

    def test_failed_pre_anchor_ledger_allows_null_manifest(self) -> None:
        value = self.ledger()
        value["release_manifest_sha256"] = None
        value["results"] = {
            "github": "FAILED",
            "conan2": "NOT_SELECTED",
            "deb_apt": "NOT_SELECTED",
            "rpm": "NOT_SELECTED",
            "arch_aur": "NOT_SELECTED",
            "homebrew": "NOT_SELECTED",
            "chocolatey": "NOT_SELECTED",
        }
        self.assertEqual(ReleaseLedger.from_mapping(value).to_mapping(), value)

    def test_published_anchor_requires_manifest_digest(self) -> None:
        value = self.ledger()
        value["release_manifest_sha256"] = None
        with self.assertRaises(ValidationError):
            ReleaseLedger.from_mapping(value)

    def test_rc_accepts_not_selected_downstream_channels(self) -> None:
        value = self.ledger("0.2.0-rc.1")
        value["results"] = {
            name: "PUBLISHED" if name == "github" else "NOT_SELECTED"
            for name in value["results"]  # type: ignore[union-attr]
        }
        self.assertEqual(ReleaseLedger.from_mapping(value).to_mapping(), value)

    def test_destination_specific_pending_states_are_enforced(self) -> None:
        for destination, result in (
            ("github", "DISPATCHED_PENDING_REVIEW"),
            ("deb_apt", "DISPATCHED_PENDING_REVIEW"),
            ("rpm", "SUBMITTED_PENDING_MODERATION"),
            ("arch_aur", "DISPATCHED_PENDING_MODERATION"),
            ("homebrew", "DISPATCHED_PENDING_MODERATION"),
            ("chocolatey", "DISPATCHED_PENDING_REVIEW"),
        ):
            value = self.ledger()
            value["results"][destination] = result  # type: ignore[index]
            with self.subTest(destination=destination, result=result), self.assertRaises(ValidationError):
                ReleaseLedger.from_mapping(value)

    def test_rc_rejects_even_failed_downstream_state(self) -> None:
        value = self.ledger("0.2.0-rc.1")
        value["results"] = {
            name: "PUBLISHED" if name == "github" else "NOT_SELECTED"
            for name in value["results"]  # type: ignore[union-attr]
        }
        value["results"]["deb_apt"] = "FAILED"  # type: ignore[index]
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
        lock = json.loads(Path("release/native-builders/lock.json").read_text(encoding="utf-8"))
        for target in lock["targets"]:
            target["base_digest"] = f"sha256:{ONE_DIGEST}"
            target["image_digest"] = f"sha256:{TWO_DIGEST}"
        self.builder_lock = Path(self.temporary.name) / "native-builder-lock.json"
        self.builder_lock.write_text(json.dumps(lock), encoding="utf-8")

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
            channel_capabilities=["github", "conan2", "apt", "rpm", "aur", "homebrew", "chocolatey"],
            payloads=[record],
            dependency_closure=[],
            conan_requirements=list(CONAN_REQUIREMENTS),
            provenance_subjects=[],
        )
        manifest_path = Path(self.temporary.name) / "release-manifest.json"
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        checksums = build_sha256sums([payload, manifest_path]).decode()
        self.assertIn("VERSION", checksums)
        self.assertIn("release-manifest.json", checksums)
        with self.assertRaises(ValidationError):
            build_sha256sums([Path(self.temporary.name) / "SHA256SUMS"])

    def test_stable_manifest_requires_complete_channel_capabilities(self) -> None:
        record = ArtifactRecord.from_path(self.root / "VERSION", role="canonical-source")
        arguments = {
            "version": SemVer.parse("0.2.0"),
            "tag": "v0.2.0",
            "commit": ZERO_SHA,
            "source_tree_sha256": ONE_DIGEST,
            "ledger_issue_id": "1",
            "ledger_issue_url": "https://github.com/yurirocha15/mcp-cpp-sdk/issues/1",
            "primary_fingerprint": PRIMARY,
            "tag_subkey_fingerprint": "C" * 40,
            "artifact_subkey_fingerprint": SUBKEY,
            "payloads": [record],
            "dependency_closure": [],
            "conan_requirements": list(CONAN_REQUIREMENTS),
            "provenance_subjects": [],
        }
        with self.assertRaises(ValidationError):
            build_release_manifest(channel_capabilities=["github"], **arguments)

    @staticmethod
    def candidate_inventory(version: SemVer):
        root = Path(__file__).resolve().parents[2]
        targets = load_native_targets(root / "packaging/native-targets.json")
        prefix = f"mcp-cpp-sdk-{version}"
        roles = {
            f"{prefix}.tar.gz": "source-or-binary-archive",
            f"{prefix}.zip": "source-or-binary-archive",
            f"{prefix}.spdx.json": "sbom",
            f"{prefix}.cdx.json": "sbom",
            "release-signing-key.asc": "publisher-input",
        }
        routes = []
        build_tuples = {}
        if not version.is_prerelease:
            roles.update(
                {
                    name: "publisher-input"
                    for name in (
                        "aur-PKGBUILD",
                        "aur-SRCINFO",
                        "homebrew-mcp-cpp-sdk.rb",
                        "conan-recipe-config-entry.json",
                        "conan-recipe-conandata-entry.json",
                        "conan-recipe-conanfile.py",
                        "conan-recipe-test-CMakeLists.txt",
                        "conan-recipe-test-conanfile.py",
                        "conan-recipe-test-test_package.cpp",
                        "conan-source.json",
                        "cloudsmith-routes.json",
                        "release-publication-contract.json",
                    )
                }
            )
            roles[f"{prefix}-windows-x64-v143-md.zip"] = "source-or-binary-archive"
            roles[f"mcp-cpp-sdk.{version}.nupkg"] = "native-package"
            roles[f"{prefix}-ubuntu-noble-amd64-gcc13-libstdcxx-abigail2.4.abi.xml"] = "abi-corpus"
            roles[ABI_BUILD_IDENTITY_NAME] = "abi-build-identity"
            roles["build-identity-windows-x64-v143-md.json"] = "build-identity"
            build_tuples[f"{prefix}-windows-x64-v143-md.zip"] = "windows-x64-v143-md"
            build_tuples[f"mcp-cpp-sdk.{version}.nupkg"] = "windows-x64-v143-md"
            build_tuples["build-identity-windows-x64-v143-md.json"] = "windows-x64-v143-md"
            build_tuples[f"{prefix}-ubuntu-noble-amd64-gcc13-libstdcxx-abigail2.4.abi.xml"] = (
                "ubuntu-noble-amd64-gcc13-libstdcxx-abigail2.4"
            )
            build_tuples[ABI_BUILD_IDENTITY_NAME] = ABI_BUILD_TUPLE
            for target in targets:
                identity = f"build-identity-{target.id}.json"
                roles[identity] = "build-identity"
                build_tuples[identity] = f"{target.format}-{target.id}"
                if target.format == "apt":
                    packages = (
                        (f"libmcp-cpp-sdk{version.abi_version}", version.debian_version, target.architecture),
                        ("libmcp-cpp-sdk-dev", version.debian_version, target.architecture),
                        ("libmcp-cpp-sdk-static-dev", version.debian_version, target.architecture),
                    )
                    extension = "deb"
                else:
                    rpm_version, rpm_release = version.rpm_version_release
                    suffix = f".fc{target.release}" if target.distribution == "fedora" else f".el{target.release}"
                    package_version = f"{rpm_version}-{rpm_release}{suffix}"
                    packages = [
                        (f"mcp-cpp-sdk{version.abi_version}-libs", package_version, target.architecture),
                        ("mcp-cpp-sdk-devel", package_version, target.architecture),
                        ("mcp-cpp-sdk-static", package_version, target.architecture),
                    ]
                    if target.architecture == "x86_64":
                        packages.append(("mcp-cpp-sdk", package_version, "src"))
                    extension = "rpm"
                for index, (package_name, package_version, package_architecture) in enumerate(packages):
                    asset = f"{target.id}--{package_name}-{index}.{extension}"
                    routes.append(
                        NativeRoute(
                            asset,
                            target.format,
                            target.id,
                            target.distribution,
                            target.release,
                            target.architecture,
                            package_name,
                            package_version,
                            package_architecture,
                            f"{target.format}-{target.id}",
                            identity,
                        )
                    )
                    roles[asset] = "native-package"
                    build_tuples[asset] = f"{target.format}-{target.id}"
        records = [
            ArtifactRecord(name, 1, ONE_DIGEST, role, build_tuples.get(name))
            for name, role in roles.items()
        ]
        provenance = [
            {"name": record.name, "digest": {"sha256": record.sha256}}
            for record in sorted(records, key=lambda record: record.name)
        ]
        return records, routes, targets, provenance

    def test_complete_stable_and_rc_candidate_inventories(self) -> None:
        for version_text, expected_payloads in (("0.2.0", 98), ("0.2.0-rc.1", 5)):
            version = SemVer.parse(version_text)
            records, routes, targets, provenance = self.candidate_inventory(version)
            self.assertEqual(len(records), expected_payloads)
            validate_candidate_inventory(
                version=version,
                payloads=records,
                routes=routes,
                targets=targets,
                provenance_subjects=provenance,
            )

    def test_candidate_inventory_rejects_every_missing_fixed_asset(self) -> None:
        version = SemVer.parse("0.2.0")
        records, routes, targets, provenance = self.candidate_inventory(version)
        fixed = [record.name for record in records if not record.name.endswith((".deb", ".rpm"))]
        for missing in fixed:
            with self.subTest(missing=missing), self.assertRaises(ValidationError):
                validate_candidate_inventory(
                    version=version,
                    payloads=[record for record in records if record.name != missing],
                    routes=routes,
                    targets=targets,
                    provenance_subjects=provenance,
                )

    def test_candidate_inventory_rejects_missing_target_or_package(self) -> None:
        version = SemVer.parse("0.2.0")
        records, routes, targets, provenance = self.candidate_inventory(version)
        for removed in (routes[0:3], routes[0:1]):
            removed_names = {route.asset for route in removed}
            with self.subTest(count=len(removed)), self.assertRaises(ValidationError):
                validate_candidate_inventory(
                    version=version,
                    payloads=[record for record in records if record.name not in removed_names],
                    routes=[route for route in routes if route.asset not in removed_names],
                    targets=targets,
                    provenance_subjects=[item for item in provenance if item["name"] not in removed_names],
                )

    def test_candidate_inventory_rejects_wrong_route_metadata_and_provenance(self) -> None:
        version = SemVer.parse("0.2.0")
        records, routes, targets, provenance = self.candidate_inventory(version)
        wrong = list(routes)
        route = wrong[0]
        wrong[0] = NativeRoute(**{**route.to_mapping(), "package_version": "9.9.9-1"})
        with self.assertRaises(ValidationError):
            validate_candidate_inventory(
                version=version,
                payloads=records,
                routes=wrong,
                targets=targets,
                provenance_subjects=provenance,
            )
        with self.assertRaises(ValidationError):
            validate_candidate_inventory(
                version=version,
                payloads=records,
                routes=routes,
                targets=targets,
                provenance_subjects=provenance[:-1],
            )

    def signed_candidate(self, *, include_notes: bool = True):
        version = SemVer.parse("0.2.0")
        prototype_records, routes, _, _ = self.candidate_inventory(version)
        prefix = "candidate-notes-" if include_notes else "candidate-anchor-"
        directory = Path(tempfile.mkdtemp(prefix=prefix, dir=self.temporary.name))
        route_bytes = (json.dumps([route.to_mapping() for route in routes], sort_keys=True) + "\n").encode()
        roles = {record.name: record.role for record in prototype_records}
        build_tuples = {record.name: record.build_tuple for record in prototype_records}
        with mock.patch(
            "release.publication_contract.load_builder_lock",
            return_value=load_builder_lock(self.builder_lock, require_resolved=True),
        ):
            contract_bytes = canonical_json_bytes(
                build_publication_contract(
                    root=Path(__file__).resolve().parents[2],
                    control_commits={
                        "homebrew": "3" * 40,
                        "chocolatey": "4" * 40,
                        "conan": "5" * 40,
                    },
                    destinations=PROVIDER_DESTINATIONS,
                )
            )
        for name in roles:
            data = route_bytes if name == "cloudsmith-routes.json" else f"payload:{name}\n".encode()
            if name == "release-publication-contract.json":
                data = contract_bytes
            if name == ABI_BUILD_IDENTITY_NAME:
                data = b"{}\n"
            elif name.startswith("build-identity-") and name != "build-identity-windows-x64-v143-md.json":
                target_id = name.removeprefix("build-identity-").removesuffix(".json")
                if target_id in APT_TARGETS:
                    identity = validate_apt_build_identity(
                        target_id, APT_TARGETS[target_id].expected_facts()
                    )
                else:
                    target = RPM_TARGETS[target_id]
                    identity = validate_rpm_build_identity(
                        target_id,
                        {
                            "os_id": target.builder_os_id,
                            "os_version_id": target.builder_os_version_id,
                            "rpm_fedora": target.rpm_fedora,
                            "rpm_rhel": target.rpm_rhel,
                            "rpm_dist": target.rpm_dist,
                            "rpm_architecture": target.architecture,
                            "uname_machine": target.uname_machine,
                        },
                    )
                identity = bind_container_identity(
                    identity,
                    image=(
                        "ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders"
                        f"@sha256:{TWO_DIGEST}"
                    ),
                    image_id=f"sha256:{ONE_DIGEST}",
                )
                data = canonical_json_bytes(identity)
            elif name == "build-identity-windows-x64-v143-md.json":
                dll = "mcp-cpp-sdk-0.2.0.dll"
                data = canonical_json_bytes(
                    validate_windows_build_identity(
                        {
                            "os_architecture": "64-bit",
                            "process_architecture": "AMD64",
                            "compiler_id": "MSVC",
                            "msc_ver": 1942,
                            "pointer_bits": 64,
                            "build_configuration": "Release",
                            "cache": {
                                "CMAKE_GENERATOR": "Visual Studio 17 2022",
                                "CMAKE_GENERATOR_PLATFORM": "x64",
                                "CMAKE_GENERATOR_TOOLSET": "v143",
                                "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreadedDLL",
                            },
                            "shared_compile_flags": ["/nologo", "/O2", "/MD", "/DNDEBUG", "/EHsc"],
                            "dumpbin_dependents": {
                                dll: (
                                    f"Dump of file C:\\stage\\bin\\{dll}\nFile Type: DLL\n\n"
                                    "Image has the following dependencies:\n\n"
                                    "    VCRUNTIME140.dll\n    MSVCP140.dll\n    KERNEL32.dll\n"
                                )
                            },
                        },
                        expected_abi_version="0.2.0",
                    )
                )
            (directory / name).write_bytes(data)
        trusted_key = Path(self.temporary.name) / "trusted-release-key.asc"
        trusted_key.write_bytes((directory / "release-signing-key.asc").read_bytes())
        records = [
            ArtifactRecord.from_path(
                directory / name, role=role, build_tuple=build_tuples[name]
            )
            for name, role in roles.items()
        ]
        provenance = [
            {"name": record.name, "digest": {"sha256": record.sha256}}
            for record in sorted(records, key=lambda record: record.name)
        ]
        manifest = build_release_manifest(
            version=version,
            tag=version.tag,
            commit=ZERO_SHA,
            source_tree_sha256=ONE_DIGEST,
            ledger_issue_id="1",
            ledger_issue_url="https://github.com/yurirocha15/mcp-cpp-sdk/issues/1",
            primary_fingerprint=PRIMARY,
            tag_subkey_fingerprint="C" * 40,
            artifact_subkey_fingerprint=SUBKEY,
            channel_capabilities=["github", "conan2", "apt", "rpm", "aur", "homebrew", "chocolatey"],
            payloads=records,
            dependency_closure=candidate_verifier.DEPENDENCY_CLOSURE,
            conan_requirements=list(CONAN_REQUIREMENTS),
            provenance_subjects=provenance,
        )
        (directory / "release-manifest.json").write_bytes(canonical_json_bytes(manifest))
        (directory / "release-manifest.json.asc").write_text("signature\n", encoding="ascii")
        for name in roles:
            if name.endswith((".tar.gz", ".zip")):
                (directory / f"{name}.asc").write_text("signature\n", encoding="ascii")
        checksum_paths = sorted(directory.iterdir(), key=lambda path: path.name)
        (directory / "SHA256SUMS").write_text(
            "".join(f"{sha256_file(path)}  {path.name}\n" for path in checksum_paths),
            encoding="ascii",
        )
        (directory / "SHA256SUMS.asc").write_text("signature\n", encoding="ascii")
        if include_notes:
            (directory / "RELEASE_NOTES.md").write_text("release notes\n", encoding="utf-8")
        return directory, trusted_key

    def verify_signed_candidate(self, directory: Path, trusted_key: Path, *, include_notes: bool):
        imported = subprocess.CompletedProcess(["gpg"], 0, "", "")
        with mock.patch.object(candidate_verifier.subprocess, "run", return_value=imported), mock.patch.object(
            candidate_verifier, "_verify_signature"
        ), mock.patch.object(candidate_verifier, "validate_candidate_pair"):
            return candidate_verifier.verify_candidate(
                directory,
                tag="v0.2.0",
                commit=ZERO_SHA,
                ledger_issue="1",
                repository="yurirocha15/mcp-cpp-sdk",
                primary_fingerprint=PRIMARY,
                tag_fingerprint="C" * 40,
                artifact_fingerprint=SUBKEY,
                public_key=trusted_key,
                targets_path=Path(__file__).resolve().parents[2] / "packaging/native-targets.json",
                target_catalog_path=Path(__file__).resolve().parents[2] / "packaging/targets.json",
                native_builder_lock_path=self.builder_lock,
                conan_requirements_path=(
                    Path(__file__).resolve().parents[2]
                    / "packaging/conan-center/requirements.json"
                ),
                include_release_notes=include_notes,
            )

    def test_signed_candidate_verifier_accepts_fresh_and_downloaded_anchor_modes(self) -> None:
        fresh, fresh_key = self.signed_candidate(include_notes=True)
        self.assertEqual(self.verify_signed_candidate(fresh, fresh_key, include_notes=True), sha256_file(fresh / "release-manifest.json"))
        anchor, anchor_key = self.signed_candidate(include_notes=False)
        self.assertEqual(self.verify_signed_candidate(anchor, anchor_key, include_notes=False), sha256_file(anchor / "release-manifest.json"))

    def test_deferred_v020_survives_current_policy_catalog_and_lock_evolution(self) -> None:
        anchor, anchor_key = self.signed_candidate(include_notes=False)
        imported = subprocess.CompletedProcess(["gpg"], 0, "", "")
        with (
            mock.patch.object(historical_anchor.subprocess, "run", return_value=imported),
            mock.patch.object(historical_anchor, "_verify_signature"),
            mock.patch.object(candidate_verifier, "MANIFEST_FIELDS", {"future"}),
            mock.patch("release.artifacts.EXPECTED_NATIVE_TARGET_IDS", ("future",)),
        ):
            digest = historical_anchor.verify_historical_candidate_v2(
                anchor,
                tag="v0.2.0",
                commit=ZERO_SHA,
                ledger_issue="1",
                repository="yurirocha15/mcp-cpp-sdk",
                signers={
                    "primary_fingerprint": PRIMARY,
                    "tag_subkey_fingerprint": "C" * 40,
                    "artifact_subkey_fingerprint": SUBKEY,
                },
                public_key=anchor_key,
            )
        self.assertEqual(digest, sha256_file(anchor / "release-manifest.json"))

    def test_signed_candidate_verifier_rejects_missing_and_extra_files(self) -> None:
        for mutation in ("missing", "extra"):
            directory, trusted_key = self.signed_candidate(include_notes=True)
            if mutation == "missing":
                (directory / "mcp-cpp-sdk.0.2.0.nupkg").unlink()
            else:
                (directory / "unexpected.txt").write_text("unexpected\n", encoding="utf-8")
            with self.subTest(mutation=mutation), self.assertRaises(ValidationError):
                self.verify_signed_candidate(directory, trusted_key, include_notes=True)

    def test_signed_candidate_verifier_rejects_obsolete_zlib_closure(self) -> None:
        directory, trusted_key = self.signed_candidate(include_notes=True)
        manifest_path = directory / "release-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["dependency_closure"].append(
            {"name": "zlib", "minimum": "1.2.11"}
        )
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        with self.assertRaisesRegex(ValidationError, "dependency closure"):
            self.verify_signed_candidate(directory, trusted_key, include_notes=True)

    def test_candidate_cli_accepts_release_notes_mode(self) -> None:
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            candidate_verifier, "verify_candidate", return_value=ONE_DIGEST
        ) as verify:
            output = Path(directory) / "output"
            arguments = [
                "verify_candidate.py", "--directory", directory, "--tag", "v0.2.0",
                "--commit", ZERO_SHA, "--ledger-issue", "1", "--repository", "yurirocha15/mcp-cpp-sdk",
                "--primary-fingerprint", PRIMARY, "--tag-fingerprint", "C" * 40,
                "--artifact-fingerprint", SUBKEY, "--public-key", "key.asc",
                "--targets", "targets.json", "--conan-requirements", "requirements.json",
                "--target-catalog", "target-catalog.json",
                "--native-builder-lock", "native-builder-lock.json",
                "--release-notes", "excluded",
                "--github-output", str(output),
            ]
            with mock.patch.object(candidate_verifier.sys, "argv", arguments):
                self.assertEqual(candidate_verifier.main(), 0)
            self.assertFalse(verify.call_args.kwargs["include_release_notes"])
            self.assertEqual(output.read_text(encoding="utf-8"), f"manifest_sha256={ONE_DIGEST}\n")

    def test_server_asset_digests_flattens_more_than_default_page_size(self) -> None:
        assets = [
            {"name": f"asset-{index}.bin", "digest": f"sha256:{index:064x}"}
            for index in range(87)
        ]
        pages = [assets[:30], assets[30:60], assets[60:]]
        digests = candidate_verifier.server_asset_digests(pages)
        self.assertEqual(len(digests), 87)
        self.assertEqual(digests["asset-86.bin"], f"{86:064x}")
        with self.assertRaises(ValidationError):
            candidate_verifier.server_asset_digests([assets[:1], assets[:1]])

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
            "ABI_VERSION": "0.2.0",
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
            "BOOST_REFERENCE": CONAN_REQUIREMENTS[0],
            "NLOHMANN_JSON_REFERENCE": CONAN_REQUIREMENTS[1],
            "OPENSSL_REFERENCE": CONAN_REQUIREMENTS[2],
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

            nuspec = ET.parse(output_root / "chocolatey/mcp-cpp-sdk.nuspec")
            namespace = {"n": "http://schemas.microsoft.com/packaging/2015/06/nuspec.xsd"}
            dependencies = nuspec.findall(".//n:dependency", namespace)
            self.assertEqual(
                [(item.attrib.get("id"), item.attrib.get("version")) for item in dependencies],
                [("vcredist140", None)],
            )
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
