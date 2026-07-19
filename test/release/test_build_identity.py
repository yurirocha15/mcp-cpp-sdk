from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from release.build_identity import (
    APT_TARGETS,
    AUR_TARGETS,
    RPM_TARGETS,
    BuildIdentityError,
    load_and_validate_target_projection,
    validate_apt_build_identity,
    validate_aur_build_identity,
    validate_rpm_build_identity,
    validate_windows_build_identity,
)


ROOT = Path(__file__).resolve().parents[2]


class AptBuildIdentityTests(unittest.TestCase):
    def test_target_table_covers_every_native_apt_route(self) -> None:
        self.assertEqual(
            set(APT_TARGETS),
            {
                f"{distribution}-{release}-{architecture}"
                for distribution, release in (
                    ("ubuntu", "jammy"),
                    ("ubuntu", "noble"),
                    ("ubuntu", "resolute"),
                    ("debian", "bookworm"),
                    ("debian", "trixie"),
                )
                for architecture in ("amd64", "arm64")
            },
        )

    def test_accepts_every_exact_apt_identity(self) -> None:
        version_ids = {
            ("ubuntu", "jammy"): "22.04",
            ("ubuntu", "noble"): "24.04",
            ("ubuntu", "resolute"): "26.04",
            ("debian", "bookworm"): "12",
            ("debian", "trixie"): "13",
        }
        for target_id, target in APT_TARGETS.items():
            with self.subTest(target_id=target_id):
                self.assertEqual(target.version_id, version_ids[(target.distribution, target.codename)])
                result = validate_apt_build_identity(target_id, target.expected_facts())
                self.assertEqual(result["target_id"], target_id)
                self.assertEqual(result["kind"], "apt")
                self.assertEqual(len(result["evidence_sha256"]), 64)

    def test_rejects_os_release_architecture_and_schema_mutations(self) -> None:
        valid = APT_TARGETS["ubuntu-jammy-amd64"].expected_facts()
        mutations = {
            "wrong ID": {**valid, "os_id": "debian"},
            "wrong VERSION_ID": {**valid, "os_version_id": "24.04"},
            "wrong codename": {**valid, "os_version_codename": "noble"},
            "wrong dpkg arch": {**valid, "dpkg_architecture": "arm64"},
            "wrong uname": {**valid, "uname_machine": "aarch64"},
            "missing": {key: value for key, value in valid.items() if key != "os_id"},
            "extra": {**valid, "runner_label": "release-ubuntu-jammy-amd64"},
        }
        for name, facts in mutations.items():
            with self.subTest(name=name), self.assertRaises(BuildIdentityError):
                validate_apt_build_identity("ubuntu-jammy-amd64", facts)

    def test_rejects_unknown_target_and_unstripped_command_output(self) -> None:
        with self.assertRaises(BuildIdentityError):
            validate_apt_build_identity("ubuntu-future-amd64", {})
        facts = APT_TARGETS["debian-bookworm-amd64"].expected_facts()
        facts["os_version_id"] = "12\n"
        with self.assertRaises(BuildIdentityError):
            validate_apt_build_identity("debian-bookworm-amd64", facts)


class RpmBuildIdentityTests(unittest.TestCase):
    def fedora_facts(self) -> dict[str, str]:
        return {
            "os_id": "fedora",
            "os_version_id": "43",
            "rpm_fedora": "43",
            "rpm_rhel": "",
            "rpm_dist": ".fc43",
            "rpm_architecture": "x86_64",
            "uname_machine": "x86_64",
        }

    def el_facts(self) -> dict[str, str]:
        return {
            "os_id": "almalinux",
            "os_version_id": "9.8",
            "rpm_fedora": "",
            "rpm_rhel": "9",
            "rpm_dist": ".el9",
            "rpm_architecture": "aarch64",
            "uname_machine": "aarch64",
        }

    def test_target_table_covers_every_native_rpm_route(self) -> None:
        self.assertEqual(
            set(RPM_TARGETS),
            {
                f"{distribution}-{release}-{architecture}"
                for distribution, releases in (("fedora", ("43", "44")), ("el", ("9", "10")))
                for release in releases
                for architecture in ("x86_64", "aarch64")
            },
        )
        for target in RPM_TARGETS.values():
            with self.subTest(target=target):
                expected_prefix = ".fc" if target.distribution == "fedora" else ".el"
                self.assertEqual(target.rpm_dist, expected_prefix + target.release)
                self.assertEqual(target.rpm_fedora, target.release if target.distribution == "fedora" else "")
                self.assertEqual(target.rpm_rhel, target.release if target.distribution == "el" else "")

    def test_accepts_exact_fedora_and_el_builder_facts(self) -> None:
        fedora = validate_rpm_build_identity("fedora-43-x86_64", self.fedora_facts())
        self.assertEqual(fedora["facts"]["rpm_dist"], ".fc43")
        enterprise = validate_rpm_build_identity(
            "el-9-aarch64",
            self.el_facts(),
        )
        self.assertEqual(enterprise["facts"]["os_version_id"], "9.8")

    def test_el_targets_pin_reviewed_exact_almalinux_versions(self) -> None:
        for release, version_id in (("9", "9.8"), ("10", "10.2")):
            for architecture in ("x86_64", "aarch64"):
                target = RPM_TARGETS[f"el-{release}-{architecture}"]
                with self.subTest(release=release, architecture=architecture):
                    self.assertEqual(target.builder_os_id, "almalinux")
                    self.assertEqual(target.builder_os_version_id, version_id)

    def test_rejects_rpm_os_macro_and_architecture_mutations(self) -> None:
        valid = self.fedora_facts()
        for field, value in (
            ("os_id", "rocky"),
            ("os_version_id", "44"),
            ("rpm_fedora", "44"),
            ("rpm_rhel", "9"),
            ("rpm_dist", ".fc44"),
            ("rpm_architecture", "aarch64"),
            ("uname_machine", "aarch64"),
        ):
            facts = {**valid, field: value}
            with self.subTest(field=field), self.assertRaises(BuildIdentityError):
                validate_rpm_build_identity("fedora-43-x86_64", facts)

    def test_rejects_observed_el_identity_different_from_configured_builder(self) -> None:
        for field, value in (("os_id", "rocky"), ("os_version_id", "9.7")):
            facts = {**self.el_facts(), field: value}
            with self.subTest(field=field), self.assertRaises(BuildIdentityError):
                validate_rpm_build_identity("el-9-aarch64", facts)


class AurBuildIdentityTests(unittest.TestCase):
    def test_accepts_the_official_x86_64_arch_linux_host(self) -> None:
        self.assertEqual(set(AUR_TARGETS), {"x86_64"})
        for architecture, target in AUR_TARGETS.items():
            with self.subTest(architecture=architecture):
                result = validate_aur_build_identity(architecture, target.expected_facts())
                self.assertEqual(result["target_id"], f"aur-{architecture}")

    def test_rejects_non_arch_missing_release_file_and_wrong_uname(self) -> None:
        valid = AUR_TARGETS["x86_64"].expected_facts()
        for facts in (
            {**valid, "os_id": "ubuntu"},
            {**valid, "arch_release_present": "false"},
            {**valid, "uname_machine": "aarch64"},
            {key: value for key, value in valid.items() if key != "arch_release_present"},
        ):
            with self.subTest(facts=facts), self.assertRaises(BuildIdentityError):
                validate_aur_build_identity("x86_64", facts)


class TargetCatalogTests(unittest.TestCase):
    def test_checked_in_catalog_is_the_only_native_target_source(self) -> None:
        path = ROOT / "packaging/targets.json"
        targets = load_and_validate_target_projection(path)
        self.assertEqual(sum(target["format"] == "apt" for target in targets), 10)
        self.assertEqual(sum(target["format"] == "rpm" for target in targets), 8)
        self.assertFalse((ROOT / "packaging/native-targets.json").exists())

    def test_strict_loader_rejects_duplicate_keys_and_incomplete_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            duplicate = Path(directory) / "duplicate.json"
            duplicate.write_text('{"same":1,"same":2}\n', encoding="utf-8")
            with self.assertRaises(BuildIdentityError):
                load_and_validate_target_projection(duplicate)

            catalog = json.loads((ROOT / "packaging/targets.json").read_text(encoding="utf-8"))
            catalog["apt"].pop()
            incomplete = Path(directory) / "incomplete.json"
            incomplete.write_text(json.dumps(catalog), encoding="utf-8")
            with self.assertRaises(BuildIdentityError):
                load_and_validate_target_projection(incomplete)


class WindowsBuildIdentityTests(unittest.TestCase):
    def facts(self, abi_version: str = "0.2.0") -> dict[str, object]:
        name = f"mcp-cpp-sdk-{abi_version}.dll"
        return {
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
                name: (
                    f"Dump of file C:\\stage\\bin\\{name}\nFile Type: DLL\n\n"
                    "  Image has the following dependencies:\n\n"
                    "    VCRUNTIME140.dll\n    MSVCP140.dll\n    KERNEL32.dll\n"
                )
            },
        }

    def validate(
        self,
        facts: dict[str, object] | None = None,
        *,
        abi_version: str = "0.2.0",
    ) -> dict[str, object]:
        return validate_windows_build_identity(
            self.facts(abi_version) if facts is None else facts,
            expected_abi_version=abi_version,
        )

    def test_accepts_exact_x64_v143_dynamic_release_evidence(self) -> None:
        for abi_version in ("0.2.0", "1"):
            with self.subTest(abi_version=abi_version):
                result = self.validate(abi_version=abi_version)
                self.assertEqual(result["target_id"], "windows-x64-v143-md")
                self.assertEqual(result["facts"]["msc_ver"], "1942")
                self.assertEqual(result["facts"]["runtime"], "MultiThreadedDLL")
                self.assertEqual(result["facts"]["abi_version"], abi_version)
                self.assertEqual(
                    result["facts"]["sdk_dll"],
                    f"mcp-cpp-sdk-{abi_version}.dll",
                )
                self.assertEqual(len(result["evidence_sha256"]), 64)

    def test_abi_version_must_be_canonical_and_match_the_observed_dll(self) -> None:
        for abi_version in ("", "0", "0.2", "0.02.0", "1.0", "01", "../1"):
            with self.subTest(abi_version=abi_version), self.assertRaises(BuildIdentityError):
                self.validate(abi_version=abi_version)

        for expected, observed in (
            ("0.2.0", "0.3.0"),
            ("1", "2"),
            ("0.2.0", "0"),
        ):
            with self.subTest(expected=expected, observed=observed), self.assertRaises(
                BuildIdentityError
            ):
                self.validate(self.facts(observed), abi_version=expected)

        with self.assertRaisesRegex(BuildIdentityError, "full 0.minor.patch"):
            self.validate(abi_version="0.2")

    def test_rejects_host_compiler_configuration_and_cache_mutations(self) -> None:
        mutations = (
            ("os_architecture", "32-bit"),
            ("process_architecture", "ARM64"),
            ("compiler_id", "Clang"),
            ("msc_ver", 1929),
            ("msc_ver", 1950),
            ("pointer_bits", 32),
            ("build_configuration", "Debug"),
        )
        for field, value in mutations:
            facts = self.facts()
            facts[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(BuildIdentityError):
                self.validate(facts)

        for field, value in (
            ("CMAKE_GENERATOR", "Ninja"),
            ("CMAKE_GENERATOR_PLATFORM", "Win32"),
            ("CMAKE_GENERATOR_TOOLSET", "v142"),
            ("CMAKE_MSVC_RUNTIME_LIBRARY", "MultiThreaded"),
        ):
            facts = self.facts()
            facts["cache"][field] = value
            with self.subTest(cache_field=field), self.assertRaises(BuildIdentityError):
                self.validate(facts)

    def test_rejects_static_debug_or_incomplete_compile_flags(self) -> None:
        for flags in (
            ["/O2", "/MT", "/DNDEBUG"],
            ["/O2", "/MDd", "/DNDEBUG"],
            ["/Od", "/MD", "/DNDEBUG"],
            ["/O2", "/MD", "/D_DEBUG"],
            ["/O2", "/MD", "/DNDEBUG", "/D\"_DEBUG=1\""],
            ["/O2", "/MD"],
        ):
            facts = self.facts()
            facts["shared_compile_flags"] = flags
            with self.subTest(flags=flags), self.assertRaises(BuildIdentityError):
                self.validate(facts)

    def test_rejects_unbound_static_or_debug_dumpbin_evidence(self) -> None:
        outputs = (
            "Dump of file C:\\stage\\bin\\other.dll\n"
            "Image has the following dependencies:\nVCRUNTIME140.dll\nMSVCP140.dll\n",
            "Dump of file C:\\stage\\bin\\mcp-cpp-sdk-0.2.0.dll\nImage has the following dependencies:\nKERNEL32.dll\n",
            "Dump of file C:\\stage\\bin\\mcp-cpp-sdk-0.2.0.dll\n"
            "Image has the following dependencies:\nVCRUNTIME140D.dll\nMSVCP140D.dll\n",
            "Dump of file C:\\stage\\bin\\mcp-cpp-sdk-0.2.0.dll\nVCRUNTIME140.dll\nMSVCP140.dll\n",
        )
        for output in outputs:
            facts = self.facts()
            facts["dumpbin_dependents"] = {"mcp-cpp-sdk-0.2.0.dll": output}
            with self.subTest(output=output), self.assertRaises(BuildIdentityError):
                self.validate(facts)

    def test_rejects_missing_extra_or_noncanonical_windows_facts(self) -> None:
        missing = self.facts()
        del missing["cache"]
        extra = {**self.facts(), "runner_label": "release-windows-x64-v143"}
        non_integer = {**self.facts(), "msc_ver": "1942"}
        for facts in (missing, extra, non_integer):
            with self.subTest(fields=sorted(facts)), self.assertRaises(BuildIdentityError):
                self.validate(facts)


if __name__ == "__main__":
    unittest.main()
