from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from release.assemble_unsigned import assemble_unsigned
from release.model import ValidationError


ROOT = Path(__file__).resolve().parents[2]
PRIMARY = "A" * 40
ARTIFACT = "B" * 40
TAG_SUBKEY = "C" * 40
PUBLIC_DEPENDENCY_CLOSURE = [
    {"name": "boost", "minimum": "1.74"},
    {"name": "nlohmann_json", "minimum": "3.10.5"},
    {"name": "openssl", "minimum": "3.0"},
]

class AssembleUnsignedTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.parts = self.base / "parts"
        self.output = self.base / "unsigned"
        self.parts.mkdir()
        self.version = "0.2.0-rc.1"
        self.commit = "a" * 40
        self.tarball = self.parts / f"mcp-cpp-sdk-{self.version}.tar.gz"
        assets = {
            self.tarball.name: b"source tarball\n",
            f"mcp-cpp-sdk-{self.version}.zip": b"source zip\n",
            f"mcp-cpp-sdk-{self.version}.spdx.json": b"{}\n",
            f"mcp-cpp-sdk-{self.version}.cdx.json": b"{}\n",
            "release-signing-key.asc": b"public key\n",
            "RELEASE_NOTES.md": b"release notes\n",
        }
        for name, content in assets.items():
            (self.parts / name).write_bytes(content)
        self.write_metadata(hashlib.sha256(self.tarball.read_bytes()).hexdigest())

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_metadata(self, source_sha256: str) -> None:
        (self.parts / "core-metadata.json").write_text(
            json.dumps(
                {
                    "commit": self.commit,
                    "source_date_epoch": 1,
                    "source_tree_sha256": "d" * 64,
                    "source_sha256": source_sha256,
                },
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )

    def assemble(self) -> None:
        assemble_unsigned(
            root=ROOT,
            parts=self.parts,
            output=self.output,
            version_text=self.version,
            tag=f"v{self.version}",
            commit=self.commit,
            primary_fingerprint=PRIMARY,
            tag_subkey_fingerprint=TAG_SUBKEY,
            artifact_subkey_fingerprint=ARTIFACT,
        )

    def test_assembles_complete_rc_boundary_without_internal_metadata(self) -> None:
        self.assemble()
        expected_payloads = {
            f"mcp-cpp-sdk-{self.version}.tar.gz",
            f"mcp-cpp-sdk-{self.version}.zip",
            f"mcp-cpp-sdk-{self.version}.spdx.json",
            f"mcp-cpp-sdk-{self.version}.cdx.json",
            "release-signing-key.asc",
        }
        manifest = json.loads((self.output / "release-manifest.json").read_text())
        self.assertEqual({item["name"] for item in manifest["payloads"]}, expected_payloads)
        self.assertEqual(manifest["channel_capabilities"], ["github"])
        self.assertEqual(manifest["dependency_closure"], PUBLIC_DEPENDENCY_CLOSURE)
        self.assertFalse((self.output / "core-metadata.json").exists())
        self.assertTrue((self.output / "RELEASE_NOTES.md").is_file())
        checksum_names = {
            line.split("  ", 1)[1]
            for line in (self.output / "UNSIGNED-SHA256SUMS").read_text().splitlines()
        }
        self.assertEqual(checksum_names, expected_payloads | {"release-manifest.json"})

    def test_rejects_tampered_metadata_extra_assets_and_non_regular_parts(self) -> None:
        self.write_metadata("0" * 64)
        with self.assertRaises(ValidationError):
            self.assemble()

        self.write_metadata(hashlib.sha256(self.tarball.read_bytes()).hexdigest())
        (self.parts / "unexpected.bin").write_bytes(b"unexpected")
        with self.assertRaises(ValidationError):
            self.assemble()
        (self.parts / "unexpected.bin").unlink()

        (self.parts / "linked").symlink_to(self.tarball)
        with self.assertRaises(ValidationError):
            self.assemble()

    def test_rejects_duplicate_json_fields(self) -> None:
        digest = hashlib.sha256(self.tarball.read_bytes()).hexdigest()
        (self.parts / "core-metadata.json").write_text(
            '{"commit":"' + self.commit + '","commit":"' + self.commit
            + '","source_date_epoch":1,"source_tree_sha256":"' + "d" * 64
            + '","source_sha256":"' + digest + '"}\n',
            encoding="utf-8",
        )
        with self.assertRaises(ValidationError):
            self.assemble()
