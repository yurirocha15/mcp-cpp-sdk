from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.request import urlopen

from release.conan_validation import (
    LOCKED_REQUIREMENTS,
    _validate_lockfile,
    materialize_candidate,
)
from release.loopback_archive import archive_server
from release.model import ValidationError


ROOT = Path(__file__).resolve().parents[2]
CONAN_SCRIPT = ROOT / "scripts/release/validate_conan_candidate.ps1"


class ConanCandidateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.archive = self.assets / "mcp-cpp-sdk-0.2.0.tar.gz"
        self.archive.write_bytes(b"candidate source archive")
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        values = {
            "conan-recipe-config-entry.json": {
                "schema_version": 1,
                "version": "0.2.0",
                "folder": "all",
            },
            "conan-recipe-conandata-entry.json": {
                "schema_version": 1,
                "version": "0.2.0",
                "url": "https://github.com/yurirocha15/mcp-cpp-sdk/releases/"
                "download/v0.2.0/mcp-cpp-sdk-0.2.0.tar.gz",
                "sha256": digest,
            },
        }
        for name, value in values.items():
            (self.assets / name).write_text(json.dumps(value) + "\n", encoding="utf-8")
        static = {
            "conan-recipe-conanfile.py": "from conan import ConanFile\n",
            "conan-recipe-test-CMakeLists.txt": "cmake_minimum_required(VERSION 3.20)\n",
            "conan-recipe-test-conanfile.py": "from conan import ConanFile\n",
            "conan-recipe-test-test_package.cpp": "int main() { return 0; }\n",
        }
        for name, content in static.items():
            (self.assets / name).write_text(content, encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def materialize(self) -> tuple[Path, Path]:
        production = self.root / "production"
        adapted = self.root / "adapted"
        materialize_candidate(
            assets=self.assets,
            archive=self.archive,
            version="0.2.0",
            production=production,
            adapted=adapted,
            local_url="http://127.0.0.1:12345/mcp-cpp-sdk-0.2.0.tar.gz",
        )
        return production, adapted

    def test_materializes_exact_recipe_with_one_url_adaptation(self) -> None:
        production, adapted = self.materialize()
        expected_files = {
            "config.yml",
            "all/conandata.yml",
            "all/conanfile.py",
            "all/test_package/CMakeLists.txt",
            "all/test_package/conanfile.py",
            "all/test_package/test_package.cpp",
        }
        self.assertEqual(
            {path.relative_to(production).as_posix() for path in production.rglob("*") if path.is_file()},
            expected_files,
        )
        production_data = (production / "all/conandata.yml").read_text(encoding="utf-8")
        adapted_data = (adapted / "all/conandata.yml").read_text(encoding="utf-8")
        local_url = "http://127.0.0.1:12345/mcp-cpp-sdk-0.2.0.tar.gz"
        production_url = (
            "https://github.com/yurirocha15/mcp-cpp-sdk/releases/"
            "download/v0.2.0/mcp-cpp-sdk-0.2.0.tar.gz"
        )
        self.assertEqual(adapted_data.replace(local_url, production_url), production_data)
        for relative in expected_files - {"all/conandata.yml"}:
            self.assertEqual((production / relative).read_bytes(), (adapted / relative).read_bytes())

    def test_rejects_archive_digest_mismatch(self) -> None:
        self.archive.write_bytes(b"modified")
        with self.assertRaisesRegex(ValidationError, "differs from conandata"):
            self.materialize()

    def test_rejects_non_loopback_adaptation(self) -> None:
        with self.assertRaisesRegex(ValidationError, "loopback HTTP"):
            materialize_candidate(
                assets=self.assets,
                archive=self.archive,
                version="0.2.0",
                production=self.root / "production",
                adapted=self.root / "adapted",
                local_url="https://example.invalid/mcp-cpp-sdk-0.2.0.tar.gz",
            )

    def test_rejects_missing_exact_recipe_file(self) -> None:
        (self.assets / "conan-recipe-test-test_package.cpp").unlink()
        with self.assertRaisesRegex(ValidationError, "inventory is incomplete"):
            self.materialize()

    def test_rejects_duplicate_signed_entry_fields(self) -> None:
        (self.assets / "conan-recipe-config-entry.json").write_text(
            '{"schema_version":1,"version":"0.2.0","version":"0.2.0","folder":"all"}\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValidationError, "duplicate key"):
            self.materialize()

    def test_dependency_lock_binds_exact_recipe_revisions(self) -> None:
        lock = ROOT / "release/windows/conan.lock"
        self.assertRegex(_validate_lockfile(lock), r"^[0-9a-f]{64}$")
        value = json.loads(lock.read_text(encoding="utf-8"))
        value["requires"] = [
            reference
            for reference in value["requires"]
            if not reference.startswith("openssl/3.6.5#")
        ]
        mutated = self.root / "conan.lock"
        mutated.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValidationError, "does not bind openssl/3.6.5"):
            _validate_lockfile(mutated)


    def test_locked_requirements_match_both_recipes(self) -> None:
        for recipe in ("conanfile.py", "packaging/conan-center/all/conanfile.py.in"):
            text = (ROOT / recipe).read_text(encoding="utf-8")
            with self.subTest(recipe=recipe):
                self.assertEqual(
                    tuple(sorted(re.findall(r'self\.requires\(\s*"([^"]+)"', text))),
                    LOCKED_REQUIREMENTS,
                )


class ConanCandidatePowerShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = CONAN_SCRIPT.read_text(encoding="utf-8")

    def test_checks_exact_windows_toolchain_and_native_architecture(self) -> None:
        for fragment in (
            "Set-StrictMode -Version Latest",
            "$env:VCToolsVersion.Trim().TrimEnd('\\') -cne $ExpectedVCToolsVersion",
            "$env:VisualStudioVersion -notmatch '^17\\.'",
            "$env:VSCMD_ARG_TGT_ARCH -cne 'x64'",
            "$env:VSCMD_ARG_HOST_ARCH -cne 'x64'",
            "$env:PROCESSOR_ARCHITECTURE -cne 'AMD64'",
            "std::cout << _MSC_VER",
            "$actualMscVer -cne [string]$ExpectedMscVer",
            "^compiler\\.version=$profileCompilerVersion$",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, self.text)

    def test_runs_exact_rendered_recipe_validator_without_publication(self) -> None:
        for fragment in (
            "release.conan_validation",
            "--assets $candidateRoot",
            "--archive $archive",
            "--build-profile $profile",
            "--host-profile $profile",
            "--lockfile $lockfile",
            "--evidence $Evidence",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, self.text)
        self.assertIsNone(
            re.search(
                r"\b(?:upload|publish|push|api[_-]?key|token|password)\b",
                self.text,
                re.I,
            )
        )


class LoopbackArchiveTests(unittest.TestCase):
    def test_serves_only_the_exact_archive_on_loopback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "candidate archive.tar.gz"
            archive.write_bytes(b"candidate")
            try:
                with archive_server(archive) as url:
                    self.assertTrue(url.startswith("http://127.0.0.1:"))
                    with urlopen(url, timeout=2) as response:
                        self.assertEqual(response.read(), b"candidate")
                    with self.assertRaises(HTTPError) as missing:
                        urlopen(url.rsplit("/", 1)[0] + "/other", timeout=2)
                    self.assertEqual(missing.exception.code, 404)
            except PermissionError:
                self.skipTest("loopback sockets are unavailable in this sandbox")

    def test_rejects_empty_or_missing_archive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "empty.zip"
            archive.touch()
            with self.assertRaisesRegex(ValidationError, "must not be empty"):
                with archive_server(archive):
                    pass
            with self.assertRaisesRegex(ValidationError, "regular file"):
                with archive_server(archive.with_name("missing.zip")):
                    pass


if __name__ == "__main__":
    unittest.main()
