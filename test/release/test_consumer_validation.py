from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from release.consumer_validation import consumer_sources, validate_elf_dependencies
from release.model import SemVer, ValidationError


class ConsumerValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_consumer_sources_require_both_linkages_and_exact_version(self) -> None:
        cmake, source = consumer_sources(SemVer.parse("0.2.0"))
        self.assertIn("COMPONENTS shared static", cmake)
        self.assertIn("mcp::sdk_shared", cmake)
        self.assertIn("mcp::sdk_static", cmake)
        self.assertIn('expected{\"0.2.0\"}', source)

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
