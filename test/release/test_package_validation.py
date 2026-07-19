from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest

from release.model import ValidationError
from release.package_validation import (
    _consumer_sources,
    adapt_chocolatey_install,
    compare_files,
    compare_package_trees,
    copy_exact_file,
)
from release.model import SemVer


REPOSITORY = "https://github.com/yurirocha15/mcp-cpp-sdk"


class PackageAdaptationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def archive(self, name: str) -> tuple[Path, str]:
        path = self.root / name
        path.write_bytes(b"reviewed candidate bytes")
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def test_chocolatey_adaptation_changes_only_the_archive_url(self) -> None:
        archive, digest = self.archive("mcp-cpp-sdk-0.2.0-windows-x64-v143-md.zip")
        production_url = f"{REPOSITORY}/releases/download/v0.2.0/{archive.name}"
        script = self.root / "chocolateyinstall.ps1"
        production = (
            "$version = '0.2.0'\n"
            "Get-ChocolateyWebFile -PackageName 'mcp-cpp-sdk' `\n"
            f"    -Url64bit '{production_url}' -Checksum64 '{digest}' "
            "-ChecksumType64 'sha256'\n"
        )
        script.write_text(production, encoding="utf-8")
        output = self.root / "adapted.ps1"
        local_url = f"http://127.0.0.1:54321/{archive.name}"

        adapt_chocolatey_install(
            script=script,
            archive=archive,
            local_url=local_url,
            output=output,
        )

        adapted = output.read_text(encoding="utf-8")
        self.assertEqual(adapted.replace(local_url, production_url), production)

    def test_chocolatey_requires_exact_production_identity(self) -> None:
        archive, digest = self.archive("mcp-cpp-sdk-0.2.0-windows-x64-v143-md.zip")
        script = self.root / "chocolateyinstall.ps1"
        script.write_text(
            "$version = '0.2.0'\n"
            f"    -Url64bit 'https://example.invalid/{archive.name}' "
            f"-Checksum64 '{digest}' -ChecksumType64 'sha256'\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValidationError, "production archive identity"):
            adapt_chocolatey_install(
                script=script,
                archive=archive,
                local_url=f"http://127.0.0.1:54321/{archive.name}",
                output=self.root / "adapted.ps1",
            )

    def test_chocolatey_rejects_non_loopback_test_url(self) -> None:
        archive, digest = self.archive("mcp-cpp-sdk-0.2.0-windows-x64-v143-md.zip")
        production_url = f"{REPOSITORY}/releases/download/v0.2.0/{archive.name}"
        script = self.root / "chocolateyinstall.ps1"
        script.write_text(
            "$version = '0.2.0'\n"
            f"    -Url64bit '{production_url}' -Checksum64 '{digest}' "
            "-ChecksumType64 'sha256'\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValidationError, "loopback HTTP"):
            adapt_chocolatey_install(
                script=script,
                archive=archive,
                local_url="https://example.invalid/archive.zip",
                output=self.root / "adapted.ps1",
            )


class PackageTreeTests(unittest.TestCase):
    def test_exact_file_copy_must_be_new_and_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.tar.gz"
            destination = root / "cache/source.tar.gz"
            source.write_bytes(b"candidate")
            copy_exact_file(source, destination)
            compare_files(source, destination)
            destination.write_bytes(b"changed")
            with self.assertRaisesRegex(ValidationError, "differs"):
                compare_files(source, destination)
            with self.assertRaisesRegex(ValidationError, "must be new"):
                copy_exact_file(source, destination)

    def test_tree_comparison_is_content_and_case_sensitive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = root / "expected"
            actual = root / "actual"
            (expected / "include").mkdir(parents=True)
            (actual / "include").mkdir(parents=True)
            (expected / "include/sdk.hpp").write_text("same", encoding="utf-8")
            (actual / "include/sdk.hpp").write_text("same", encoding="utf-8")
            compare_package_trees(expected, actual)
            (actual / "include/sdk.hpp").write_text("different", encoding="utf-8")
            with self.assertRaisesRegex(ValidationError, "differs"):
                compare_package_trees(expected, actual)
            (actual / "include/sdk.hpp").unlink()
            (actual / "include/SDK.hpp").write_text("same", encoding="utf-8")
            with self.assertRaisesRegex(ValidationError, "differs"):
                compare_package_trees(expected, actual)

    def test_consumer_sources_require_both_linkages_and_exact_version(self) -> None:
        cmake, source = _consumer_sources(SemVer.parse("0.2.0"))
        self.assertIn("COMPONENTS shared static", cmake)
        self.assertIn("mcp::sdk_shared", cmake)
        self.assertIn("mcp::sdk_static", cmake)
        self.assertIn('expected{\"0.2.0\"}', source)


if __name__ == "__main__":
    unittest.main()
