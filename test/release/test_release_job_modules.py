from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from release.assemble_unsigned import assemble_unsigned
from release.artifacts import build_sha256sums, public_dependency_closure
from release.aur_validation import (
    validate_elf_dependencies,
    validate_package_list,
    write_consumer,
)
from release.model import ValidationError
from release.signing import prepare_signing, verify_final_boundary, write_final_checksums
from release.verify_candidate import validate_dependency_closure


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
            ledger_issue="1",
            repository="yurirocha15/mcp-cpp-sdk",
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


class NativeDependencyPolicyTests(unittest.TestCase):
    def test_native_metadata_matches_the_public_cmake_link_interface(self) -> None:
        self.assertEqual(public_dependency_closure(), PUBLIC_DEPENDENCY_CLOSURE)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        public_interface = cmake.split(
            "function(mcp_cpp_sdk_configure_library target)", 1
        )[1].split("endfunction()", 1)[0]
        self.assertIn(
            "PUBLIC Boost::headers nlohmann_json::nlohmann_json\n"
            "                     OpenSSL::Crypto",
            public_interface,
        )
        self.assertNotIn("zlib", public_interface.lower())

        debian = (ROOT / "packaging/debian/control.in").read_text(encoding="utf-8")
        self.assertNotIn("zlib", debian.lower())
        self.assertEqual(
            next(line for line in debian.splitlines() if line.startswith("Build-Depends:")),
            "Build-Depends: debhelper-compat (= 13), cmake (>= 3.20), "
            "ninja-build, g++ (>= 11), libboost-dev (>= 1.74), libgtest-dev, "
            "nlohmann-json3-dev (>= 3.10.5), libssl-dev (>= 3.0), pkg-config",
        )
        self.assertEqual(
            [line for line in debian.splitlines() if line.startswith("Depends:")],
            [
                "Depends: ${shlibs:Depends}, ${misc:Depends}",
                "Depends: libmcp-cpp-sdk@ABI_VERSION@ (= ${binary:Version}), "
                "libboost-dev (>= 1.74), nlohmann-json3-dev (>= 3.10.5), "
                "libssl-dev (>= 3.0), ${misc:Depends}",
                "Depends: libmcp-cpp-sdk-dev (= ${binary:Version}), ${misc:Depends}",
            ],
        )

        rpm = (ROOT / "packaging/rpm/mcp-cpp-sdk.spec.in").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("zlib", rpm.lower())
        self.assertEqual(
            [line for line in rpm.splitlines() if line.startswith("BuildRequires:")],
            [
                "BuildRequires:  cmake >= 3.20",
                "BuildRequires:  gcc-c++ >= 11",
                "BuildRequires:  ninja-build",
                "BuildRequires:  boost-devel >= 1.74",
                "BuildRequires:  gtest-devel",
                "BuildRequires:  json-devel >= 3.10.5",
                "BuildRequires:  openssl-devel >= 3.0",
                "BuildRequires:  pkgconfig",
            ],
        )
        self.assertEqual(
            [line for line in rpm.splitlines() if line.startswith("Requires:")],
            [
                "Requires: mcp-cpp-sdk@ABI_VERSION@-libs%{?_isa} = "
                "%{version}-%{release}",
                "Requires: boost-devel >= 1.74",
                "Requires: json-devel >= 3.10.5",
                "Requires: openssl-devel >= 3.0",
                "Requires: %{name}-devel%{?_isa} = %{version}-%{release}",
            ],
        )

    def test_candidate_dependency_closure_is_exact_and_rejects_zlib(self) -> None:
        validate_dependency_closure(PUBLIC_DEPENDENCY_CLOSURE)
        mutations = (
            PUBLIC_DEPENDENCY_CLOSURE[:-1],
            list(reversed(PUBLIC_DEPENDENCY_CLOSURE)),
            [
                *PUBLIC_DEPENDENCY_CLOSURE,
                {"name": "zlib", "minimum": "1.2.11"},
            ],
            [
                *PUBLIC_DEPENDENCY_CLOSURE[:-1],
                {"name": "openssl", "minimum": "3.1"},
            ],
        )
        for value in mutations:
            with self.subTest(value=value), self.assertRaises(ValidationError):
                validate_dependency_closure(value)


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
        self.assertIn('MCP_EXPECTED_VERSION=\\"0.2.0\\"', cmake)
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


class ReleaseShellContractTests(unittest.TestCase):
    def test_signing_script_keeps_secrets_out_of_arguments_and_cleans_them(self) -> None:
        script = (ROOT / "scripts/release/sign_release.sh").read_text(encoding="utf-8")
        for fragment in (
            "set -euo pipefail",
            "set +x",
            "umask 077",
            "mktemp -d",
            "trap cleanup EXIT",
            "release.signing prepare",
            "release.signing finalize",
            "release.signing verify",
            '--local-user "${ARTIFACT_SUBKEY_FINGERPRINT}!"',
            "--passphrase-fd 0",
            "unset RELEASE_GPG_PRIVATE_KEY_B64",
            "unset RELEASE_GPG_PASSPHRASE",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, script)
        self.assertNotIn("--passphrase ${RELEASE_GPG_PASSPHRASE}", script)
        self.assertNotIn("<<'PY'", script)

    def test_aur_script_uses_module_generated_exported_symbol_consumers(self) -> None:
        script = (ROOT / "scripts/release/validate_aur_packages.sh").read_text(
            encoding="utf-8"
        )
        for fragment in (
            "collect_build_identity.py",
            "makepkg --verifysource --noconfirm",
            "makepkg --cleanbuild --noconfirm",
            "release.aur_validation packages",
            "release.aur_validation consumer",
            "readelf --dynamic --wide",
            "release.aur_validation elf",
            "sudo /usr/local/libexec/mcp-install-aur-packages",
            '"${check}/consumer-build/shared-consumer"',
            '"${check}/consumer-build/static-consumer"',
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, script)
        self.assertNotIn("<<'PY'", script)


if __name__ == "__main__":
    unittest.main()
