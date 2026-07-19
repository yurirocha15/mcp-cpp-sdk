from __future__ import annotations

from pathlib import Path
import re
import unittest

from release.model import SemVer


ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = ROOT / "scripts" / "release" / "build_windows.ps1"
PROFILE_PATH = ROOT / "release" / "windows" / "conan-msvc-release.profile"


REQUIRED_FRAGMENTS = (
    "Set-StrictMode -Version Latest",
    "$ErrorActionPreference = 'Stop'",
    "function Invoke-CheckedCommand",
    "function Invoke-CapturedCommand",
    "function Assert-ExactToolVersion",
    "function Assert-ReleaseProjectSettings",
    "function Assert-InstalledConsumer",
    "function Assert-ExactOutputInventory",
    "function Start-LoopbackArchiveServer",
    "function Stop-LoopbackArchiveServer",
    "function Invoke-ChocolateyCandidateValidation",
    "$sourceVersion -cne $Version",
    "$versionMatch.Groups['major'].Value -eq '0'",
    "$abiVersion = if",
    "$versionCore",
    "-version', '[17.0,18.0)'",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "$env:VSCMD_ARG_TGT_ARCH -cne 'x64'",
    "$env:VSCMD_ARG_HOST_ARCH -cne 'x64'",
    "$env:PROCESSOR_ARCHITECTURE -cne 'AMD64'",
    "$actualVCToolsVersion -cne $ExpectedVCToolsVersion",
    "$mscVer -ne $ExpectedMscVer",
    r"\\bin\\Hostx64\\x64\\cl\.exe$",
    r"\\bin\\Hostx64\\x64\\dumpbin\.exe$",
    "-ToolName 'CMake'",
    "-ToolName 'Conan'",
    "-ToolName 'Chocolatey'",
    "-ToolName 'Python'",
    "'/nologo', '/EHsc', '/O2', '/MD', '/DNDEBUG'",
    "release/windows/conan-msvc-release.profile",
    "--lockfile=release/windows/conan.lock",
    "--profile:host=$profilePath",
    "--profile:build=$profilePath",
    "'-G', 'Visual Studio 17 2022'",
    "'-A', 'x64'",
    "'-T', 'v143'",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
    "-DMCP_CPP_SDK_BUILD_SHARED=ON",
    "-DMCP_CPP_SDK_BUILD_STATIC=ON",
    "-DMCP_CPP_SDK_DEFAULT_LINKAGE=shared",
    "-DBUILD_TESTING=ON",
    "-DBUILD_EXAMPLES=OFF",
    "-DBUILD_DOCS=OFF",
    "-TargetName 'mcp-cpp-sdk-shared'",
    "-TargetName 'mcp-cpp-sdk-static'",
    "find_package(mcp-cpp-sdk CONFIG REQUIRED)",
    'mcp::sdk_$Linkage',
    "-Linkage 'shared'",
    "-Linkage 'static'",
    "Installed $Linkage consumer execution failed",
    "'/HEADERS'",
    "8664 machine \\(x64\\)",
    "'/DEPENDENTS'",
    "'/DIRECTIVES'",
    "/DEFAULTLIB:MSVCRT",
    "LIBCMTD?",
    "MSVCRTD",
    "shared_compile_flags = @('/MD', '/O2', '/DNDEBUG')",
    "scripts/release/collect_build_identity.py",
    "'--kind', 'windows'",
    "build-identity-windows-x64-v143-md.json",
    "mcp-cpp-sdk-$Version-windows-x64-v143-md.zip",
    'mcp-cpp-sdk.$Version.nupkg',
    "'release.package_validation', 'chocolatey-adapt'",
    "'release.package_validation', 'compare-trees'",
    "'release.loopback_archive'",
    "'install', 'mcp-cpp-sdk'",
    "'uninstall', 'mcp-cpp-sdk'",
    "[EnvironmentVariableTarget]::Machine",
)

FORBIDDEN_PATTERNS = (
    re.compile(r"scripts[\\/](?:init|build)\.py", re.IGNORECASE),
    re.compile(r"(?:^|[ '\"])-G(?:[ '\"]+)Ninja(?:$|[ '\"])", re.IGNORECASE),
    re.compile(r"packaging[\\/]render_release_metadata\.py", re.IGNORECASE),
    re.compile(r"(?:C:\\Users\\|/Users/|/home/)[^ '\";]+", re.IGNORECASE),
    re.compile(r"(?:USERPROFILE|COMPUTERNAME|HOSTNAME)", re.IGNORECASE),
)


def validate_script_contract(text: str) -> None:
    missing = [fragment for fragment in REQUIRED_FRAGMENTS if fragment not in text]
    forbidden = [pattern.pattern for pattern in FORBIDDEN_PATTERNS if pattern.search(text)]
    if missing or forbidden:
        raise AssertionError(f"missing={missing!r}; forbidden={forbidden!r}")


class WindowsReleaseScriptContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = SCRIPT_PATH.read_text(encoding="utf-8")

    def test_checked_in_script_satisfies_the_release_contract(self) -> None:
        validate_script_contract(self.script)
        profile = PROFILE_PATH.read_text(encoding="utf-8")
        for fragment in (
            "compiler=msvc", "compiler.cppstd=20", "compiler.runtime=dynamic",
            "compiler.runtime_type=Release", "build_type=Release",
        ):
            self.assertIn(fragment, profile)

    def test_all_build_tool_versions_are_explicit_inputs(self) -> None:
        expected_parameters = {
            "ExpectedCMakeVersion",
            "ExpectedConanVersion",
            "ExpectedChocolateyVersion",
            "ExpectedPythonVersion",
            "ExpectedVCToolsVersion",
            "ExpectedMscVer",
        }
        for parameter in expected_parameters:
            with self.subTest(parameter=parameter):
                self.assertRegex(
                    self.script,
                    rf"\[Parameter\(Mandatory = \$true\)\]\s+"
                    rf"(?:\[ValidateRange\([^\]]+\)\]\s+)?"
                    rf"\[(?:string|int)\]\${parameter}\b",
                )

    def test_contract_rejects_removed_guards_and_legacy_builder_insertion(self) -> None:
        for fragment in (
            "$sourceVersion -cne $Version",
            "$actualVCToolsVersion -cne $ExpectedVCToolsVersion",
            "-DMCP_CPP_SDK_BUILD_STATIC=ON",
            "-Linkage 'shared'",
            "scripts/release/collect_build_identity.py",
        ):
            with self.subTest(fragment=fragment), self.assertRaises(AssertionError):
                validate_script_contract(self.script.replace(fragment, ""))

        with self.assertRaises(AssertionError):
            validate_script_contract(self.script + "\npython scripts/build.py --release\n")

    def test_pre_one_abi_identity_uses_the_exact_semver_core(self) -> None:
        """Keep the PowerShell derivation aligned with the cross-platform policy."""

        for version, expected in (
            ("0.2.0", "0.2.0"),
            ("0.2.0-rc.1", "0.2.0"),
            ("0.12.34", "0.12.34"),
            ("0.12.34-rc.9", "0.12.34"),
            ("1.2.3", "1"),
            ("1.2.3-rc.4", "1"),
        ):
            with self.subTest(version=version):
                self.assertEqual(SemVer.parse(version).abi_version, expected)
        self.assertIn(
            "$versionCore = \"$($versionMatch.Groups['major'].Value)."
            "$($versionMatch.Groups['minor'].Value)."
            "$($versionMatch.Groups['patch'].Value)\"",
            self.script,
        )
        self.assertRegex(
            self.script,
            re.compile(
                r"\$abiVersion = if \(\$versionMatch\.Groups\['major'\]\.Value -eq '0'\) \{\s*"
                r"\$versionCore\s*\}\s*else \{\s*"
                r"\$versionMatch\.Groups\['major'\]\.Value\s*\}",
                re.DOTALL,
            ),
        )
        self.assertIn("if ($sourceVersion -cne $Version)", self.script)
        self.assertNotIn("if ($sourceVersion -cne $versionCore)", self.script)

    def test_only_reviewed_public_artifacts_are_admitted(self) -> None:
        inventory_match = re.search(
            r"Assert-ExactOutputInventory.+?-ExpectedNames @\((.*?)\n\s*\)",
            self.script,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(inventory_match)
        assert inventory_match is not None
        names = re.findall(r"\$(identityName|archiveName|nupkgName)\b", inventory_match.group(1))
        self.assertEqual(names, ["identityName", "archiveName", "nupkgName"])


if __name__ == "__main__":
    unittest.main()
