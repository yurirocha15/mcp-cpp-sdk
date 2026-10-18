from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from release.aur_validation import (
    validate_elf_dependencies,
    validate_package_list,
    write_consumer,
)
from release.model import ValidationError


class AurValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_consumer_calls_exported_version_from_both_linkage_variants(self) -> None:
        consumer = self.base / "consumer"
        write_consumer(consumer, "0.2.0")
        cmake = (consumer / "CMakeLists.txt").read_text(encoding="utf-8")
        source = (consumer / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("mcp::sdk_shared", cmake)
        self.assertIn("mcp::sdk_static", cmake)
        self.assertIn('expected{\"0.2.0\"}', source)
        self.assertIn("mcp::version()", source)
        self.assertIn("mcp::g_VERSION", source)

    def test_validates_exact_two_arch_package_outputs(self) -> None:
        package_directory = self.base / "packages"
        package_directory.mkdir()
        packages = [
            package_directory / "mcp-cpp-sdk-0.2.0-1-x86_64.pkg.tar.zst",
            package_directory / "mcp-cpp-sdk-static-0.2.0-1-x86_64.pkg.tar.zst",
        ]
        for package in packages:
            package.write_bytes(b"package")
        package_list = self.base / "package-list"
        package_list.write_text("".join(f"{path}\n" for path in reversed(packages)))
        output = self.base / "validated"
        result = validate_package_list(
            directory=package_directory,
            package_list=package_list,
            output=output,
            version_text="0.2.0",
            architecture="x86_64",
        )
        self.assertEqual(result, tuple(sorted(packages, key=lambda path: path.name)))
        self.assertEqual(output.read_text().splitlines(), [str(path) for path in result])

    def test_rejects_wrong_version_extra_package_and_path_escape(self) -> None:
        package_directory = self.base / "packages"
        package_directory.mkdir()
        package = package_directory / "mcp-cpp-sdk-0.3.0-1-aarch64.pkg.tar.zst"
        static = package_directory / "mcp-cpp-sdk-static-0.3.0-1-aarch64.pkg.tar.zst"
        package.write_bytes(b"package")
        static.write_bytes(b"package")
        package_list = self.base / "package-list"
        output = self.base / "validated"
        package_list.write_text(f"{package}\n{static}\n", encoding="utf-8")
        with self.assertRaises(ValidationError):
            validate_package_list(
                directory=package_directory,
                package_list=package_list,
                output=output,
                version_text="0.2.0",
                architecture="aarch64",
            )

        outside = self.base / "mcp-cpp-sdk-0.2.0-1-aarch64.pkg.tar.zst"
        outside.write_bytes(b"package")
        package_list.write_text(f"{outside}\n{static}\n", encoding="utf-8")
        with self.assertRaises(ValidationError):
            validate_package_list(
                directory=package_directory,
                package_list=package_list,
                output=output,
                version_text="0.2.0",
                architecture="aarch64",
            )

    def test_elf_contract_distinguishes_shared_and_static_sdk_linkage(self) -> None:
        shared = self.base / "shared.readelf"
        static = self.base / "static.readelf"
        shared.write_text(
            " 0x0000000000000001 (NEEDED) Shared library: [libmcp-cpp-sdk.so.0.2.0]\n"
            " 0x0000000000000001 (NEEDED) Shared library: [libstdc++.so.6]\n",
            encoding="utf-8",
        )
        static.write_text(
            " 0x0000000000000001 (NEEDED) Shared library: [libcrypto.so.3]\n"
            " 0x0000000000000001 (NEEDED) Shared library: [libstdc++.so.6]\n",
            encoding="utf-8",
        )
        validate_elf_dependencies(
            shared_readelf=shared,
            static_readelf=static,
            version_text="0.2.0",
        )

        for shared_soname, static_soname in (
            ("libmcp-cpp-sdk.so.0", None),
            ("libmcp-cpp-sdk.so.0.2.0", "libmcp-cpp-sdk.so.0.2.0"),
        ):
            shared.write_text(
                f" (NEEDED) Shared library: [{shared_soname}]\n"
                " (NEEDED) Shared library: [libstdc++.so.6]\n",
                encoding="utf-8",
            )
            static.write_text(
                (
                    f" (NEEDED) Shared library: [{static_soname}]\n"
                    if static_soname is not None
                    else ""
                )
                + " (NEEDED) Shared library: [libstdc++.so.6]\n",
                encoding="utf-8",
            )
            with self.subTest(shared=shared_soname, static=static_soname), self.assertRaises(
                ValidationError
            ):
                validate_elf_dependencies(
                    shared_readelf=shared,
                    static_readelf=static,
                    version_text="0.2.0",
                )
