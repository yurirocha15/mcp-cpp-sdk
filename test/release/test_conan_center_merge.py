from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from release.conan_center_merge import (
    ConanCenterMergeError,
    SourceRecord,
    merge_version_entry,
    parse_conandata,
    parse_config,
    parse_folder_inventory,
    render_conandata,
    render_config,
    source_url,
    version_key,
)


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "release" / "conan_center_merge.py"
ONE = "1" * 64
TWO = "2" * 64
THREE = "3" * 64
STATIC_FILES = {
    "conanfile.py": (
        b"from conan import ConanFile\n"
        b"REQUIRES = ('boost/1.86.0', 'nlohmann_json/3.12.0', 'openssl/3.6.3')\n"
    ),
    "test_package/CMakeLists.txt": b"cmake_minimum_required(VERSION 3.20)\n",
    "test_package/conanfile.py": b"from conan import ConanFile\n# test package\n",
    "test_package/test_package.cpp": b"int main() { return 0; }\n",
}
UNSET = object()


def config_entry(version: str) -> dict[str, object]:
    return {"schema_version": 1, "version": version, "folder": "all"}


def source_entry(version: str, digest: str = ONE) -> dict[str, object]:
    return {
        "schema_version": 1,
        "version": version,
        "url": source_url(version),
        "sha256": digest,
    }


def source_records(entries: list[tuple[str, str]]) -> dict[str, SourceRecord]:
    return {version: SourceRecord(source_url(version), digest) for version, digest in entries}


def folder_inventory(exists: bool) -> dict[str, object]:
    return {"schema_version": 1, "folders": ["all"] if exists else []}


def all_folder(entries: list[tuple[str, str]], static_files=None) -> dict[str, bytes]:
    files = dict(STATIC_FILES if static_files is None else static_files)
    files["conandata.yml"] = render_conandata(source_records(entries)).encode("utf-8")
    return files


class MergePolicyTests(unittest.TestCase):
    def merge(
        self,
        version: str,
        *,
        upstream: list[tuple[str, str]] | None = None,
        digest: str = ONE,
        candidate_static=None,
        existing=UNSET,
    ):
        upstream = [] if upstream is None else upstream
        config = "" if not upstream else render_config({item[0]: "all" for item in upstream})
        conandata = "" if not upstream else render_conandata(source_records(upstream))
        if existing is UNSET:
            existing = all_folder(upstream) if upstream else None
        return merge_version_entry(
            config,
            conandata,
            folder_inventory(bool(upstream)),
            config_entry(version),
            source_entry(version, digest),
            STATIC_FILES if candidate_static is None else candidate_static,
            expected_version=version,
            existing_all_files=existing,
        )

    def test_empty_first_release_creates_shared_all_recipe(self) -> None:
        result = self.merge("0.2.0")
        self.assertEqual(result.state, "prepared")
        self.assertEqual(result.version_count, 1)
        self.assertEqual(
            set(result.files),
            {
                "config.yml",
                "all/conandata.yml",
                "all/conanfile.py",
                "all/test_package/CMakeLists.txt",
                "all/test_package/conanfile.py",
                "all/test_package/test_package.cpp",
            },
        )
        self.assertEqual(result.files["config.yml"], b'versions:\n  "0.2.0":\n    folder: all\n')
        self.assertEqual(parse_conandata(result.files["all/conandata.yml"].decode())["0.2.0"].sha256, ONE)

    def test_canonical_empty_yaml_headers_are_also_first_release(self) -> None:
        result = merge_version_entry(
            "versions:\n",
            "sources:\n",
            folder_inventory(False),
            config_entry("1.0.0"),
            source_entry("1.0.0"),
            STATIC_FILES,
            expected_version="1.0.0",
        )
        self.assertEqual(result.state, "prepared")
        self.assertEqual(result.version_count, 1)

    def test_preserves_multiple_earlier_versions_newest_first(self) -> None:
        upstream = [("0.1.9", ONE), ("0.2.2", TWO), ("0.2.10", THREE), ("1.0.0", ONE)]
        result = self.merge("0.2.3", upstream=upstream, digest=TWO)
        expected_order = ["1.0.0", "0.2.10", "0.2.3", "0.2.2", "0.1.9"]
        self.assertEqual(result.state, "prepared")
        self.assertEqual(result.version_count, 5)
        self.assertEqual(list(parse_config(result.files["config.yml"].decode())), expected_order)
        merged_sources = parse_conandata(result.files["all/conandata.yml"].decode())
        self.assertEqual(list(merged_sources), expected_order)
        self.assertEqual(merged_sources["0.1.9"].sha256, ONE)
        self.assertEqual(merged_sources["0.2.2"].sha256, TWO)
        self.assertEqual(merged_sources["0.2.10"].sha256, THREE)
        self.assertEqual(merged_sources["1.0.0"].sha256, ONE)

    def test_identical_current_version_is_idempotent_after_static_verification(self) -> None:
        upstream = [("0.1.0", TWO), ("0.2.0", ONE)]
        result = self.merge("0.2.0", upstream=upstream, digest=ONE)
        self.assertEqual(result.state, "identical")
        self.assertEqual(result.version_count, 2)
        self.assertEqual(
            result.files["config.yml"].decode(),
            render_config({"0.1.0": "all", "0.2.0": "all"}),
        )

    def test_existing_recipe_requires_exact_all_folder_inventory(self) -> None:
        upstream = [("0.1.0", ONE)]
        with self.assertRaisesRegex(ConanCenterMergeError, "requires exact"):
            self.merge("0.2.0", upstream=upstream, existing=None)
        missing = all_folder(upstream)
        del missing["test_package/test_package.cpp"]
        extra = all_folder(upstream)
        extra["README.md"] = b"unexpected\n"
        for label, files in (("missing", missing), ("extra", extra)):
            with self.subTest(label=label), self.assertRaisesRegex(ConanCenterMergeError, "inventory"):
                self.merge("0.2.0", upstream=upstream, existing=files)

    def test_every_shared_static_file_byte_is_immutable(self) -> None:
        upstream = [("0.1.0", ONE)]
        for name in STATIC_FILES:
            candidate = dict(STATIC_FILES)
            candidate[name] += b"changed\n"
            with self.subTest(name=name), self.assertRaisesRegex(
                ConanCenterMergeError,
                "manual version-folder migration required",
            ):
                self.merge("0.2.0", upstream=upstream, candidate_static=candidate)

    def test_changed_dependency_reference_requires_manual_migration(self) -> None:
        candidate = dict(STATIC_FILES)
        candidate["conanfile.py"] = candidate["conanfile.py"].replace(b"boost/1.86.0", b"boost/1.87.0")
        with self.assertRaisesRegex(ConanCenterMergeError, "dependency.*manual version-folder migration"):
            self.merge("0.2.0", upstream=[("0.1.0", ONE)], candidate_static=candidate)

    def test_verified_folder_conandata_must_match_separately_fetched_upstream(self) -> None:
        upstream = [("0.1.0", ONE)]
        existing = all_folder(upstream)
        existing["conandata.yml"] += b" "
        with self.assertRaisesRegex(ConanCenterMergeError, "does not match"):
            self.merge("0.2.0", upstream=upstream, existing=existing)

    def test_current_version_source_conflict_is_rejected(self) -> None:
        with self.assertRaisesRegex(ConanCenterMergeError, "conflicting current version"):
            self.merge("0.2.0", upstream=[("0.2.0", ONE)], digest=TWO)

    def test_first_release_rejects_orphan_all_folder(self) -> None:
        with self.assertRaisesRegex(ConanCenterMergeError, "unexpectedly has"):
            self.merge("0.2.0", existing=all_folder([]))

    def test_config_source_and_folder_inventories_must_agree(self) -> None:
        config = render_config({"0.1.0": "all"})
        sources = render_conandata(source_records([("0.2.0", ONE)]))
        with self.assertRaisesRegex(ConanCenterMergeError, "inventories disagree"):
            merge_version_entry(
                config,
                sources,
                folder_inventory(True),
                config_entry("0.3.0"),
                source_entry("0.3.0"),
                STATIC_FILES,
                expected_version="0.3.0",
                existing_all_files=all_folder([("0.1.0", ONE)]),
            )
        with self.assertRaisesRegex(ConanCenterMergeError, "folder inventories disagree"):
            merge_version_entry(
                config,
                render_conandata(source_records([("0.1.0", ONE)])),
                folder_inventory(False),
                config_entry("0.2.0"),
                source_entry("0.2.0"),
                STATIC_FILES,
                expected_version="0.2.0",
            )

    def test_signed_entry_and_static_file_inventories_are_exact(self) -> None:
        valid_config = config_entry("0.2.0")
        valid_source = source_entry("0.2.0")
        mutations = []
        for field in valid_config:
            value = copy.deepcopy(valid_config)
            del value[field]
            mutations.append((value, valid_source))
        value = copy.deepcopy(valid_config)
        value["extra"] = "ignored"
        mutations.append((value, valid_source))
        for schema in (True, 2, "1"):
            value = copy.deepcopy(valid_config)
            value["schema_version"] = schema
            mutations.append((value, valid_source))
        for field, replacement in (("version", "0.2.1"), ("folder", "0.2.0")):
            value = copy.deepcopy(valid_config)
            value[field] = replacement
            mutations.append((value, valid_source))
        for field in valid_source:
            value = copy.deepcopy(valid_source)
            del value[field]
            mutations.append((valid_config, value))
        value = copy.deepcopy(valid_source)
        value["extra"] = "ignored"
        mutations.append((valid_config, value))
        for field, replacement in (
            ("schema_version", True),
            ("schema_version", 2),
            ("version", "0.2.1"),
            ("url", source_url("0.2.1")),
            ("sha256", "A" * 64),
            ("sha256", True),
        ):
            value = copy.deepcopy(valid_source)
            value[field] = replacement
            mutations.append((valid_config, value))
        for candidate_config, candidate_source in mutations:
            with self.subTest(candidate_config=candidate_config), self.assertRaises(ConanCenterMergeError):
                merge_version_entry(
                    "",
                    "",
                    folder_inventory(False),
                    candidate_config,
                    candidate_source,
                    STATIC_FILES,
                    expected_version="0.2.0",
                )

        missing = dict(STATIC_FILES)
        del missing["conanfile.py"]
        extra = dict(STATIC_FILES)
        extra["README.md"] = b"unexpected\n"
        empty = dict(STATIC_FILES)
        empty["conanfile.py"] = b""
        for files in (missing, extra, empty):
            with self.subTest(files=set(files)), self.assertRaises(ConanCenterMergeError):
                self.merge("0.2.0", candidate_static=files)

    def test_summary_exposes_complete_technical_file_plan(self) -> None:
        result = self.merge("0.2.0")
        summary = result.summary()
        self.assertEqual(summary["schema_version"], 1)
        self.assertEqual(summary["state"], "prepared")
        self.assertEqual([item["path"] for item in summary["files"]], sorted(result.files))
        self.assertTrue(all(len(item["sha256"]) == 64 for item in summary["files"]))


class CanonicalYamlTests(unittest.TestCase):
    def test_config_rejects_malformed_or_noncanonical_yaml(self) -> None:
        canonical = render_config({"0.2.0": "all", "0.1.0": "all"})
        malformed = {
            "version folder": canonical.replace("folder: all", "folder: 0.2.0", 1),
            "duplicate": canonical + '  "0.2.0":\n    folder: all\n',
            "oldest first": render_config({"0.1.0": "all"}) + render_config({"0.2.0": "all"})[10:],
            "missing LF": canonical.removesuffix("\n"),
            "CRLF": canonical.replace("\n", "\r\n"),
            "BOM": "\ufeff" + canonical,
            "NUL": canonical + "\0",
            "header": canonical.replace("versions:", "version:", 1),
            "comment": canonical + "# comment\n",
            "blank": canonical.replace("versions:\n", "versions:\n\n", 1),
            "trailing": canonical.replace("folder: all\n", "folder: all \n", 1),
            "unquoted": canonical.replace('"0.2.0"', "0.2.0", 1),
            "single quote": canonical.replace('"0.2.0"', "'0.2.0'", 1),
            "indent": canonical.replace('  "0.2.0"', ' "0.2.0"', 1),
            "flow": 'versions: {"0.2.0": {folder: all}}\n',
            "leading zero": canonical.replace("0.2.0", "00.2.0", 1),
            "RC": canonical.replace("0.2.0", "0.2.0-rc.1", 1),
            "whitespace": " \n",
        }
        for label, text in malformed.items():
            with self.subTest(label=label), self.assertRaises(ConanCenterMergeError):
                parse_config(text)

    def test_conandata_rejects_malformed_or_noncanonical_yaml(self) -> None:
        canonical = render_conandata(source_records([("0.2.0", ONE), ("0.1.0", TWO)]))
        url = source_url("0.2.0")
        malformed = {
            "duplicate": canonical
            + f'  "0.2.0":\n    url: "{url}"\n    sha256: "{ONE}"\n',
            "oldest first": render_conandata(source_records([("0.1.0", TWO)]))
            + render_conandata(source_records([("0.2.0", ONE)]))[9:],
            "wrong URL": canonical.replace("https://", "http://", 1),
            "wrong archive": canonical.replace(".tar.gz", ".zip", 1),
            "uppercase digest": canonical.replace(ONE, "A" * 64, 1),
            "short digest": canonical.replace(ONE, "1" * 63, 1),
            "unquoted URL": canonical.replace(f'url: "{url}"', f"url: {url}", 1),
            "unquoted digest": canonical.replace(f'sha256: "{ONE}"', f"sha256: {ONE}", 1),
            "missing LF": canonical.removesuffix("\n"),
            "CRLF": canonical.replace("\n", "\r\n"),
            "BOM": "\ufeff" + canonical,
            "comment": canonical + "# comment\n",
            "blank": canonical.replace("sources:\n", "sources:\n\n", 1),
            "trailing": canonical.replace(f'sha256: "{ONE}"\n', f'sha256: "{ONE}" \n', 1),
            "indent": canonical.replace('    url: "', '   url: "', 1),
        }
        for label, text in malformed.items():
            with self.subTest(label=label), self.assertRaises(ConanCenterMergeError):
                parse_conandata(text)

    def test_folder_inventory_is_exact(self) -> None:
        self.assertEqual(parse_folder_inventory(folder_inventory(False)), ())
        self.assertEqual(parse_folder_inventory(folder_inventory(True)), ("all",))
        malformed = [
            [],
            {"schema_version": 1, "folders": [], "extra": True},
            {"schema_version": True, "folders": []},
            {"schema_version": 1, "folders": "all"},
            {"schema_version": 1, "folders": ["all", "all"]},
            {"schema_version": 1, "folders": ["0.2.0"]},
        ]
        for value in malformed:
            with self.subTest(value=value), self.assertRaises(ConanCenterMergeError):
                parse_folder_inventory(value)

    def test_version_parser_rejects_noncanonical_versions(self) -> None:
        for version in ("v0.2.0", "00.2.0", "0.02.0", "0.2", "0.2.0-rc.1", " 0.2.0"):
            with self.subTest(version=version), self.assertRaises(ConanCenterMergeError):
                version_key(version)


class CommandLineTests(unittest.TestCase):
    def write_cli_inputs(
        self,
        directory: Path,
        *,
        upstream: list[tuple[str, str]] | None = None,
        digest: str = ONE,
        candidate_static=None,
    ) -> list[str]:
        upstream = [] if upstream is None else upstream
        candidate_static = STATIC_FILES if candidate_static is None else candidate_static
        content = {
            "upstream-config": "" if not upstream else render_config({item[0]: "all" for item in upstream}),
            "upstream-conandata": "" if not upstream else render_conandata(source_records(upstream)),
            "folder-inventory": json.dumps(folder_inventory(bool(upstream))),
            "config-entry": json.dumps(config_entry("0.2.0")),
            "conandata-entry": json.dumps(source_entry("0.2.0", digest)),
            "conanfile": candidate_static["conanfile.py"],
            "test-cmakelists": candidate_static["test_package/CMakeLists.txt"],
            "test-conanfile": candidate_static["test_package/conanfile.py"],
            "test-source": candidate_static["test_package/test_package.cpp"],
        }
        arguments: list[str] = []
        for option, value in content.items():
            path = directory / option
            path.write_bytes(value if isinstance(value, bytes) else value.encode("utf-8"))
            arguments.extend((f"--{option}", str(path)))
        return arguments

    def command(self, directory: Path, arguments: list[str], *extra: str) -> list[str]:
        return [
            sys.executable,
            "-I",
            "-S",
            str(SCRIPT),
            "--version",
            "0.2.0",
            *arguments,
            "--output-root",
            str(directory / "output"),
            *extra,
        ]

    def write_existing_all(self, directory: Path, upstream: list[tuple[str, str]]) -> Path:
        root = directory / "existing-all"
        for relative, value in all_folder(upstream).items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        return root

    def test_cli_first_release_emits_complete_tree_plan(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            arguments = self.write_cli_inputs(directory)
            result_path = directory / "result.json"
            completed = subprocess.run(
                self.command(directory, arguments, "--result", str(result_path)),
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            summary = json.loads(result_path.read_text(encoding="ascii"))
            output_files = {
                path.relative_to(directory / "output").as_posix()
                for path in (directory / "output").rglob("*")
                if path.is_file()
            }
            self.assertEqual(output_files, {item["path"] for item in summary["files"]})
            self.assertEqual(summary["state"], "prepared")

    def test_cli_later_release_requires_exact_existing_all(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            upstream = [("0.1.0", ONE)]
            arguments = self.write_cli_inputs(directory, upstream=upstream)
            missing = subprocess.run(
                self.command(directory, arguments),
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(missing.returncode, 2)
            existing = self.write_existing_all(directory, upstream)
            completed = subprocess.run(
                self.command(directory, arguments, "--existing-all", str(existing)),
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(completed.stdout)["state"], "prepared")

    def test_cli_changed_dependency_fails_without_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            upstream = [("0.1.0", ONE)]
            candidate = dict(STATIC_FILES)
            candidate["conanfile.py"] = candidate["conanfile.py"].replace(
                b"openssl/3.6.3",
                b"openssl/3.7.0",
            )
            arguments = self.write_cli_inputs(directory, upstream=upstream, candidate_static=candidate)
            existing = self.write_existing_all(directory, upstream)
            completed = subprocess.run(
                self.command(directory, arguments, "--existing-all", str(existing)),
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("manual version-folder migration required", completed.stderr)
            self.assertFalse((directory / "output").exists())


if __name__ == "__main__":
    unittest.main()
