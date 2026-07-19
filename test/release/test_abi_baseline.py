from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from release.abi_baseline import (
    AbiPolicyError,
    BaselineSelection,
    select_baseline,
    verify_abidiff_result,
    verify_baseline_artifacts,
)


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "release" / "abi_baseline.py"
ONE = "1" * 64
TWO = "2" * 64
TOOL_VERSION = "2.7"
BUILD_TUPLE = "ubuntu-noble-amd64-gcc14-libstdcxx"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def release(
    tag: str,
    database_id: int,
    *,
    draft: bool = False,
    immutable: bool = True,
    prerelease: bool | None = None,
) -> dict[str, object]:
    if prerelease is None:
        prerelease = "-rc." in tag
    return {
        "databaseId": database_id,
        "isDraft": draft,
        "isImmutable": immutable,
        "isPrerelease": prerelease,
        "tagName": tag,
    }


class BaselineSelectionTests(unittest.TestCase):
    def test_zero_major_selects_latest_stable_in_same_minor_series(self) -> None:
        catalog = [
            release("v0.1.99", 1),
            release("v0.2.0", 2),
            release("v0.2.1-rc.1", 3),
            release("v0.3.0", 4),
            release("v1.0.0", 5),
        ]
        selection = select_baseline("v0.2.1", catalog)
        self.assertEqual(selection.current.comparison_series, "0.2")
        self.assertEqual(selection.current.loader_identity, "0.2.1")
        self.assertEqual(selection.baseline.version.tag, "v0.2.0")

    def test_nonzero_major_uses_major_as_comparison_series(self) -> None:
        catalog = [
            release("v1.4.9", 1),
            release("v1.5.0-rc.1", 2),
            release("v1.4.10", 3),
            release("v0.9.99", 4),
            release("v2.0.0", 5),
        ]
        selection = select_baseline("v1.5.0", catalog)
        self.assertEqual(selection.current.comparison_series, "1")
        self.assertEqual(selection.current.loader_identity, "1")
        self.assertEqual(selection.baseline.version.tag, "v1.4.10")

    def test_new_zero_minor_comparison_series_is_explicit(self) -> None:
        selection = select_baseline(
            "v0.3.0",
            [[release("v0.2.9", 1)], [release("v1.0.0", 2)]],
        )
        self.assertFalse(selection.has_baseline)
        self.assertEqual(
            selection.to_mapping(),
            {
                "schema_version": 1,
                "status": "new-abi-line",
                "current_tag": "v0.3.0",
                "abi_line": "0.3",
                "baseline_tag": None,
                "baseline_release_id": None,
            },
        )

    def test_higher_version_does_not_replace_lower_backport_baseline(self) -> None:
        selection = select_baseline(
            "v1.2.4",
            [release("v1.2.3", 1), release("v1.3.0", 2)],
        )
        self.assertEqual(selection.baseline.version.tag, "v1.2.3")

    def test_draft_is_not_a_baseline(self) -> None:
        selection = select_baseline(
            "v1.2.2",
            [release("v1.2.1", 1, draft=True, immutable=False), release("v1.2.0", 2)],
        )
        self.assertEqual(selection.baseline.version.tag, "v1.2.0")

    def test_published_mutable_stable_on_line_fails_closed(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "mutable"):
            select_baseline("v1.2.2", [release("v1.2.1", 1, immutable=False)])

    def test_rc_candidate_is_rejected(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "stable releases"):
            select_baseline("v0.2.0-rc.1", [])

    def test_existing_candidate_release_is_rejected(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "already has"):
            select_baseline("v0.2.0", [release("v0.2.0", 1)])

    def test_duplicate_ids_or_tags_are_rejected_across_pages(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "duplicate"):
            select_baseline("v1.2.2", [[release("v1.2.0", 1)], [release("v1.2.1", 1)]])
        with self.assertRaisesRegex(AbiPolicyError, "duplicate"):
            select_baseline("v1.2.2", [release("v1.2.0", 1), release("v1.2.0", 2)])

    def test_catalog_requires_exact_typed_metadata(self) -> None:
        malformed = release("v1.2.0", 1)
        malformed["unexpected"] = True
        with self.assertRaisesRegex(AbiPolicyError, "fields"):
            select_baseline("v1.2.1", [malformed])
        malformed = release("v1.2.0", 1)
        malformed["databaseId"] = True
        with self.assertRaisesRegex(AbiPolicyError, "databaseId"):
            select_baseline("v1.2.1", [malformed])
        malformed = release("v1.2.0-rc.1", 1, prerelease=False)
        with self.assertRaisesRegex(AbiPolicyError, "disagrees"):
            select_baseline("v1.2.1", [malformed])

    def test_selection_round_trip_and_mutations(self) -> None:
        expected = select_baseline("v1.2.2", [release("v1.2.1", 7)])
        value = expected.to_mapping()
        self.assertEqual(BaselineSelection.from_mapping(value), expected)
        for field, replacement in (
            ("schema_version", 2),
            ("abi_line", "2"),
            ("status", "unknown"),
            ("baseline_tag", "v2.3.0"),
            ("baseline_release_id", True),
        ):
            mutated = dict(value)
            mutated[field] = replacement
            with self.subTest(field=field), self.assertRaises(AbiPolicyError):
                BaselineSelection.from_mapping(mutated)

    def test_cli_select_writes_machine_readable_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            catalog = directory / "catalog.json"
            output = directory / "selection.json"
            github_output = directory / "github-output"
            catalog.write_text(json.dumps([release("v1.2.1", 7)]), encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "-S",
                    str(SCRIPT),
                    "select",
                    "--current-tag",
                    "v1.2.2",
                    "--catalog",
                    str(catalog),
                    "--output",
                    str(output),
                    "--github-output",
                    str(github_output),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(output.read_text(encoding="ascii"))["baseline_tag"], "v1.2.1")
            self.assertEqual(
                github_output.read_text(encoding="ascii"),
                "abi_line=1\nhas_baseline=true\nbaseline_tag=v1.2.1\nbaseline_release_id=7\n",
            )


class BaselineArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        self.selection = select_baseline("v1.2.2", [release("v1.2.1", 7)])
        self.corpus = b"<abi-corpus version='1'/>\n"
        self.source = b"source archive\n"
        corpus_name = f"mcp-cpp-sdk-1.2.1-{BUILD_TUPLE}.abi.xml"
        self.records = sorted(
            [
                {
                    "name": corpus_name,
                    "size": len(self.corpus),
                    "sha256": digest(self.corpus),
                    "role": "abi-corpus",
                    "build_tuple": BUILD_TUPLE,
                },
                {
                    "name": "mcp-cpp-sdk-1.2.1.tar.gz",
                    "size": len(self.source),
                    "sha256": digest(self.source),
                    "role": "canonical-source",
                },
            ],
            key=lambda item: item["name"],
        )
        self.manifest = {
            "schema_version": 2,
            "package": "mcp-cpp-sdk",
            "version": "1.2.1",
            "tag": "v1.2.1",
            "commit": "a" * 40,
            "source_tree_sha256": "b" * 64,
            "release_ledger": {"issue_id": "1", "issue_url": "https://example.invalid/1"},
            "signers": {},
            "channel_capabilities": ["github"],
            "payloads": self.records,
            "dependency_closure": [],
            "conan_requirements": [
                "boost/1.86.0",
                "nlohmann_json/3.12.0",
                "openssl/3.6.3",
            ],
            "provenance_subjects": [],
        }
        self.manifest_bytes = (
            json.dumps(self.manifest, sort_keys=True, separators=(",", ":")) + "\n"
        ).encode("ascii")
        bytes_by_name = {
            record["name"]: self.corpus if record["role"] == "abi-corpus" else self.source
            for record in self.records
        }
        bytes_by_name.update(
            {
                "release-manifest.json": self.manifest_bytes,
                "release-manifest.json.asc": b"manifest signature",
                "SHA256SUMS": b"checksums",
                "SHA256SUMS.asc": b"checksum signature",
                "mcp-cpp-sdk-1.2.1.tar.gz.asc": b"source signature",
            }
        )
        self.assets = [
            {
                "id": index,
                "name": name,
                "size": len(content),
                "state": "uploaded",
                "digest": f"sha256:{digest(content)}",
                "browser_download_url": f"https://example.invalid/{name}",
            }
            for index, (name, content) in enumerate(sorted(bytes_by_name.items()), 10)
        ]

    def verify(self, **overrides):
        return verify_baseline_artifacts(
            overrides.get("selection", self.selection),
            overrides.get("assets", self.assets),
            overrides.get("manifest_bytes", self.manifest_bytes),
            overrides.get("corpus_bytes", self.corpus),
            expected_build_tuple=overrides.get("build_tuple", BUILD_TUPLE),
        )

    def test_binds_corpus_to_manifest_and_server_asset_digests(self) -> None:
        artifact = self.verify()
        self.assertEqual(artifact.release_id, 7)
        self.assertEqual(artifact.tag, "v1.2.1")
        self.assertEqual(artifact.comparison_series, "1")
        self.assertEqual(artifact.corpus_sha256, digest(self.corpus))
        self.assertEqual(artifact.build_tuple, BUILD_TUPLE)

    def test_downloaded_manifest_digest_mismatch_fails(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "manifest differs"):
            self.verify(manifest_bytes=self.manifest_bytes + b" ")

    def test_downloaded_corpus_digest_mismatch_fails(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "corpus differs"):
            self.verify(corpus_bytes=self.corpus + b" ")

    def test_manifest_identity_or_build_tuple_mismatch_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["tag"] = "v1.2.0"
        manifest_bytes = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
        assets = copy.deepcopy(self.assets)
        manifest_asset = next(item for item in assets if item["name"] == "release-manifest.json")
        manifest_asset["size"] = len(manifest_bytes)
        manifest_asset["digest"] = f"sha256:{digest(manifest_bytes)}"
        with self.assertRaisesRegex(AbiPolicyError, "identity"):
            self.verify(assets=assets, manifest_bytes=manifest_bytes)
        with self.assertRaisesRegex(AbiPolicyError, "exactly one ABI corpus"):
            self.verify(build_tuple="ubuntu-noble-amd64-clang18-libcxx")

    def test_extra_or_duplicate_remote_assets_fail(self) -> None:
        extra = copy.deepcopy(self.assets)
        extra.append(
            {
                "id": 999,
                "name": "unexpected.bin",
                "size": 1,
                "state": "uploaded",
                "digest": f"sha256:{digest(b'x')}",
            }
        )
        with self.assertRaisesRegex(AbiPolicyError, "not exactly covered"):
            self.verify(assets=extra)
        duplicate = copy.deepcopy(self.assets)
        duplicate.append(dict(duplicate[0]))
        with self.assertRaisesRegex(AbiPolicyError, "duplicates"):
            self.verify(assets=duplicate)

    def test_new_abi_line_cannot_verify_baseline_assets(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "no baseline artifacts"):
            self.verify(selection=select_baseline("v0.3.0", []))


class AbidiffResultTests(unittest.TestCase):
    def setUp(self) -> None:
        self.selection = select_baseline("v1.2.2", [release("v1.2.1", 7)])
        self.report = b""

    def result(self, exit_code: object = 0) -> dict[str, object]:
        return {
            "schema_version": 1,
            "tool": "abidiff",
            "tool_version": TOOL_VERSION,
            "baseline_tag": "v1.2.1",
            "candidate_tag": "v1.2.2",
            "abi_line": "1",
            "baseline_sha256": ONE,
            "candidate_sha256": TWO,
            "report_sha256": digest(self.report),
            "exit_code": exit_code,
        }

    def verify(self, value: object) -> None:
        verify_abidiff_result(
            value,
            self.selection,
            baseline_sha256=ONE,
            candidate_sha256=TWO,
            report_bytes=self.report,
            expected_tool_version=TOOL_VERSION,
        )

    def test_zero_status_is_accepted(self) -> None:
        self.assertIsNone(self.verify(self.result()))

    def test_every_nonzero_documented_status_fails_closed(self) -> None:
        expectations = {
            1: "failed to compare",
            2: "usage error",
            3: "usage error",
            4: "explicit review",
            8: "internally inconsistent",
            12: "incompatible",
        }
        for status, message in expectations.items():
            with self.subTest(status=status), self.assertRaisesRegex(AbiPolicyError, message):
                self.verify(self.result(status))

    def test_unknown_or_non_integer_status_fails(self) -> None:
        for status in (16, 256, -1, True, "0"):
            with self.subTest(status=status), self.assertRaises(AbiPolicyError):
                self.verify(self.result(status))

    def test_identity_digest_report_and_tool_mutations_fail(self) -> None:
        replacements = {
            "baseline_tag": "v1.2.0",
            "candidate_tag": "v1.2.3",
            "abi_line": "2",
            "baseline_sha256": TWO,
            "candidate_sha256": ONE,
            "report_sha256": ONE,
            "tool_version": "2.6",
            "tool": "not-abidiff",
        }
        for field, replacement in replacements.items():
            value = self.result()
            value[field] = replacement
            with self.subTest(field=field), self.assertRaisesRegex(AbiPolicyError, "not bound"):
                self.verify(value)

    def test_unknown_result_field_fails(self) -> None:
        value = self.result()
        value["ignored"] = "unsafe"
        with self.assertRaisesRegex(AbiPolicyError, "fields"):
            self.verify(value)

    def test_no_baseline_cannot_accept_result(self) -> None:
        with self.assertRaisesRegex(AbiPolicyError, "must not run"):
            verify_abidiff_result(
                self.result(),
                select_baseline("v0.3.0", []),
                baseline_sha256=ONE,
                candidate_sha256=TWO,
                report_bytes=self.report,
                expected_tool_version=TOOL_VERSION,
            )

    def test_cli_propagates_fail_closed_result_policy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            selection = directory / "selection.json"
            result_path = directory / "result.json"
            report = directory / "abidiff.txt"
            selection.write_text(json.dumps(self.selection.to_mapping()), encoding="utf-8")
            report.write_bytes(self.report)
            command = [
                sys.executable,
                "-I",
                "-S",
                str(SCRIPT),
                "verify-result",
                "--selection",
                str(selection),
                "--result",
                str(result_path),
                "--report",
                str(report),
                "--baseline-sha256",
                ONE,
                "--candidate-sha256",
                TWO,
                "--tool-version",
                TOOL_VERSION,
            ]
            for status, expected_code in ((0, 0), (4, 2)):
                result_path.write_text(json.dumps(self.result(status)), encoding="utf-8")
                completed = subprocess.run(
                    command,
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                with self.subTest(status=status):
                    self.assertEqual(completed.returncode, expected_code, completed.stderr)


if __name__ == "__main__":
    unittest.main()
