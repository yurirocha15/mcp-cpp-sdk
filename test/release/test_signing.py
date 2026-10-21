from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from release.artifacts import build_sha256sums
from release.model import ValidationError
from release.signing import prepare_signing, verify_final_boundary, write_final_checksums


class SigningBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.directory = self.base / "signed"
        self.directory.mkdir()
        for name in (
            "mcp-cpp-sdk-0.2.0.tar.gz",
            "mcp-cpp-sdk-0.2.0.zip",
            "release-manifest.json",
            "release-signing-key.asc",
        ):
            (self.directory / name).write_text(f"{name}\n", encoding="utf-8")
        (self.directory / "RELEASE_NOTES.md").write_text("notes\n", encoding="utf-8")
        checksum_inputs = [
            path for path in self.directory.iterdir() if path.name != "RELEASE_NOTES.md"
        ]
        (self.directory / "UNSIGNED-SHA256SUMS").write_bytes(
            build_sha256sums(checksum_inputs)
        )
        self.plan = self.base / "plan"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_prepares_and_finalizes_exact_signing_boundaries(self) -> None:
        assets = prepare_signing(self.directory, self.plan)
        self.assertEqual(
            assets,
            (
                "mcp-cpp-sdk-0.2.0.tar.gz",
                "mcp-cpp-sdk-0.2.0.zip",
                "release-manifest.json",
            ),
        )
        self.assertFalse((self.directory / "UNSIGNED-SHA256SUMS").exists())
        for name in assets:
            (self.directory / f"{name}.asc").write_text("signature\n", encoding="ascii")
        write_final_checksums(self.directory, self.plan)
        (self.directory / "SHA256SUMS.asc").write_text("signature\n", encoding="ascii")
        verify_final_boundary(self.directory, self.plan)

        (self.directory / "release-manifest.json").write_text("tampered\n", encoding="utf-8")
        with self.assertRaises(ValidationError):
            verify_final_boundary(self.directory, self.plan)

    def test_unsigned_boundary_must_cover_every_input(self) -> None:
        (self.directory / "unbound-file").write_text("extra\n", encoding="utf-8")
        with self.assertRaises(ValidationError):
            prepare_signing(self.directory, self.plan)

    def test_rejects_missing_or_unplanned_detached_signatures(self) -> None:
        assets = prepare_signing(self.directory, self.plan)
        for name in assets[:-1]:
            (self.directory / f"{name}.asc").write_text("signature\n", encoding="ascii")
        with self.assertRaises(ValidationError):
            write_final_checksums(self.directory, self.plan)

        (self.directory / f"{assets[-1]}.asc").write_text("signature\n", encoding="ascii")
        (self.directory / "unplanned.zip.asc").write_text("signature\n", encoding="ascii")
        with self.assertRaises(ValidationError):
            write_final_checksums(self.directory, self.plan)
