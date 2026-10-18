from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest import mock

from release.aur_package_installer import validate_arguments


class AurPackageInstallerTests(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)
        patcher = mock.patch("release.aur_package_installer._DIRECTORY", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self._directory.cleanup)
        self.shared = self.package("mcp-cpp-sdk-0.2.0-1-x86_64.pkg.tar.zst")
        self.static = self.package("mcp-cpp-sdk-static-0.2.0-1-x86_64.pkg.tar.zst")

    def package(self, name: str) -> Path:
        path = self.root / name
        path.write_bytes(b"package")
        return path

    def test_accepts_only_exact_split_packages(self) -> None:
        self.assertEqual(
            validate_arguments((str(self.shared), str(self.static))),
            (str(self.shared), str(self.static)),
        )

    def test_rejects_a_noncanonical_package_name(self) -> None:
        attacker = self.package("attacker.pkg.tar.zst")
        with self.assertRaisesRegex(ValueError, "not canonical"):
            validate_arguments((str(self.shared), str(attacker)))

    def test_rejects_mismatched_versions(self) -> None:
        other = self.package("mcp-cpp-sdk-static-0.2.1-1-x86_64.pkg.tar.zst")
        with self.assertRaisesRegex(ValueError, "versions"):
            validate_arguments((str(self.shared), str(other)))

    def test_requires_both_shared_and_static_packages(self) -> None:
        with self.assertRaisesRegex(ValueError, "both required"):
            validate_arguments((str(self.shared), str(self.shared)))

    def test_rejects_a_path_outside_the_build_directory(self) -> None:
        with tempfile.TemporaryDirectory() as outside:
            elsewhere = Path(outside) / self.static.name
            elsewhere.write_bytes(b"package")
            with self.assertRaisesRegex(ValueError, "outside"):
                validate_arguments((str(self.shared), str(elsewhere)))

    def test_rejects_a_symlinked_package(self) -> None:
        self.static.unlink()
        self.static.symlink_to(self.shared)
        with self.assertRaisesRegex(ValueError, "symlink"):
            validate_arguments((str(self.shared), str(self.static)))


if __name__ == "__main__":
    unittest.main()
