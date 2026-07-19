from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from release.conan_validation import _validate_lockfile, materialize_candidate
from release.model import ValidationError


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
        with self.assertRaisesRegex(ValidationError, "duplicate field"):
            self.materialize()

    def test_dependency_lock_binds_exact_recipe_revisions(self) -> None:
        lock = Path(__file__).resolve().parents[2] / "release/windows/conan.lock"
        self.assertRegex(_validate_lockfile(lock), r"^[0-9a-f]{64}$")
        value = json.loads(lock.read_text(encoding="utf-8"))
        value["requires"] = [
            reference
            for reference in value["requires"]
            if not reference.startswith("openssl/3.6.3#")
        ]
        mutated = self.root / "conan.lock"
        mutated.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValidationError, "does not bind openssl/3.6.3"):
            _validate_lockfile(mutated)


if __name__ == "__main__":
    unittest.main()
