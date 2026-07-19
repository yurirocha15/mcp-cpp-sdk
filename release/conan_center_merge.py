#!/usr/bin/env python3
"""Fail-closed merge policy for a ConanCenter shared ``all`` recipe.

ConanCenter convention keeps version-independent recipe and test files in one
``all`` folder.  A later release may extend only ``config.yml`` and
``all/conandata.yml``; changing any shared static byte requires an explicit
manual migration to version-specific folders.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Mapping, Sequence


VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
DIGEST = re.compile(r"[0-9a-f]{64}")
VERSION_LINE = re.compile(r'  "([^"]+)":')
URL_LINE = re.compile(r'    url: "([^"]+)"')
DIGEST_LINE = re.compile(r'    sha256: "([0-9a-f]{64})"')
MAX_INPUT_BYTES = 2 * 1024 * 1024
SOURCE_URL_PREFIX = "https://github.com/yurirocha15/mcp-cpp-sdk/releases/download"
STATIC_RECIPE_FILES = (
    "conanfile.py",
    "test_package/CMakeLists.txt",
    "test_package/conanfile.py",
    "test_package/test_package.cpp",
)
ALL_FOLDER_FILES = ("conandata.yml", *STATIC_RECIPE_FILES)


class ConanCenterMergeError(ValueError):
    """Raised when upstream state or candidate recipe content is unsafe."""


def version_key(value: object) -> tuple[int, int, int]:
    if not isinstance(value, str) or len(value) > 64:
        raise ConanCenterMergeError("ConanCenter version must be a bounded string")
    match = VERSION.fullmatch(value)
    if match is None:
        raise ConanCenterMergeError(f"noncanonical ConanCenter version: {value!r}")
    return tuple(int(component) for component in match.groups())


def source_url(version: str) -> str:
    version_key(version)
    return f"{SOURCE_URL_PREFIX}/v{version}/mcp-cpp-sdk-{version}.tar.gz"


def _validate_canonical_text(text: object, *, label: str) -> str:
    if not isinstance(text, str):
        raise ConanCenterMergeError(f"{label} must be UTF-8 text")
    try:
        size = len(text.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise ConanCenterMergeError(f"{label} is not valid UTF-8 text") from error
    if size > MAX_INPUT_BYTES:
        raise ConanCenterMergeError(f"{label} exceeds the size limit")
    if text and (not text.endswith("\n") or "\r" in text or "\0" in text or text.startswith("\ufeff")):
        raise ConanCenterMergeError(f"{label} is not canonical LF-terminated UTF-8 text")
    return text


def parse_config(text: object) -> dict[str, str]:
    """Parse canonical newest-first version mappings to the shared folder."""
    text = _validate_canonical_text(text, label="upstream Conan config.yml")
    if not text:
        return {}
    lines = text.splitlines()
    if not lines or lines[0] != "versions:" or (len(lines) - 1) % 2:
        raise ConanCenterMergeError("upstream Conan config.yml is not canonical")
    result: dict[str, str] = {}
    for index in range(1, len(lines), 2):
        match = VERSION_LINE.fullmatch(lines[index])
        if match is None or lines[index + 1] != "    folder: all":
            raise ConanCenterMergeError("upstream Conan config.yml entry is not canonical shared-all YAML")
        version = match.group(1)
        version_key(version)
        if version in result:
            raise ConanCenterMergeError("upstream Conan config.yml contains a duplicate version")
        result[version] = "all"
    if text != render_config(result):
        raise ConanCenterMergeError("upstream Conan config.yml is not canonically newest-first")
    return result


def render_config(versions: Mapping[str, str]) -> str:
    ordered = sorted(versions, key=version_key, reverse=True)
    if any(versions[version] != "all" for version in ordered):
        raise ConanCenterMergeError("ConanCenter recipe versions must use the shared all folder")
    return "versions:\n" + "".join(f'  "{version}":\n    folder: all\n' for version in ordered)


@dataclass(frozen=True)
class SourceRecord:
    url: str
    sha256: str


def parse_conandata(text: object) -> dict[str, SourceRecord]:
    """Parse canonical newest-first source records for the shared recipe."""
    text = _validate_canonical_text(text, label="upstream Conan all/conandata.yml")
    if not text:
        return {}
    lines = text.splitlines()
    if not lines or lines[0] != "sources:" or (len(lines) - 1) % 3:
        raise ConanCenterMergeError("upstream Conan all/conandata.yml is not canonical")
    result: dict[str, SourceRecord] = {}
    for index in range(1, len(lines), 3):
        version_match = VERSION_LINE.fullmatch(lines[index])
        url_match = URL_LINE.fullmatch(lines[index + 1]) if version_match else None
        digest_match = DIGEST_LINE.fullmatch(lines[index + 2]) if version_match else None
        if version_match is None or url_match is None or digest_match is None:
            raise ConanCenterMergeError("upstream Conan source entry is not canonical")
        version = version_match.group(1)
        version_key(version)
        if version in result:
            raise ConanCenterMergeError("upstream Conan conandata.yml contains a duplicate version")
        if url_match.group(1) != source_url(version):
            raise ConanCenterMergeError("upstream Conan source URL conflicts with its version")
        result[version] = SourceRecord(url_match.group(1), digest_match.group(1))
    if text != render_conandata(result):
        raise ConanCenterMergeError("upstream Conan conandata.yml is not canonically newest-first")
    return result


def render_conandata(sources: Mapping[str, SourceRecord]) -> str:
    ordered = sorted(sources, key=version_key, reverse=True)
    records: list[str] = []
    for version in ordered:
        source = sources[version]
        if source.url != source_url(version) or DIGEST.fullmatch(source.sha256) is None:
            raise ConanCenterMergeError("ConanCenter source record is malformed")
        records.append(
            f'  "{version}":\n'
            f'    url: "{source.url}"\n'
            f'    sha256: "{source.sha256}"\n'
        )
    return "sources:\n" + "".join(records)


def parse_folder_inventory(value: object) -> tuple[str, ...]:
    if not isinstance(value, Mapping) or set(value) != {"schema_version", "folders"}:
        raise ConanCenterMergeError("ConanCenter folder inventory fields are not exact")
    if type(value["schema_version"]) is not int or value["schema_version"] != 1:
        raise ConanCenterMergeError("ConanCenter folder inventory schema is unsupported")
    folders = value["folders"]
    if folders not in ([], ["all"]):
        raise ConanCenterMergeError("ConanCenter folder inventory must be exactly empty or ['all']")
    return tuple(folders)


@dataclass(frozen=True)
class SignedVersionEntry:
    version: str
    source: SourceRecord

    @classmethod
    def from_mappings(
        cls,
        config_entry: object,
        conandata_entry: object,
        *,
        expected_version: str,
    ) -> "SignedVersionEntry":
        version_key(expected_version)
        config_fields = {"schema_version", "version", "folder"}
        source_fields = {"schema_version", "version", "url", "sha256"}
        if not isinstance(config_entry, Mapping) or set(config_entry) != config_fields:
            raise ConanCenterMergeError("signed Conan config entry fields are not exact")
        if not isinstance(conandata_entry, Mapping) or set(conandata_entry) != source_fields:
            raise ConanCenterMergeError("signed Conan source entry fields are not exact")
        if type(config_entry["schema_version"]) is not int or config_entry["schema_version"] != 1:
            raise ConanCenterMergeError("signed Conan config entry schema is unsupported")
        if type(conandata_entry["schema_version"]) is not int or conandata_entry["schema_version"] != 1:
            raise ConanCenterMergeError("signed Conan source entry schema is unsupported")
        if config_entry["version"] != expected_version or config_entry["folder"] != "all":
            raise ConanCenterMergeError("signed Conan config entry does not select the shared all folder")
        if conandata_entry["version"] != expected_version:
            raise ConanCenterMergeError("signed Conan source entry does not match the selected release")
        url = conandata_entry["url"]
        digest = conandata_entry["sha256"]
        if url != source_url(expected_version) or not isinstance(digest, str) or DIGEST.fullmatch(digest) is None:
            raise ConanCenterMergeError("signed Conan source entry identity is malformed")
        return cls(expected_version, SourceRecord(url, digest))


def _validate_file_mapping(value: object, *, expected: set[str], label: str) -> dict[str, bytes]:
    if not isinstance(value, Mapping) or set(value) != expected:
        raise ConanCenterMergeError(f"{label} file inventory is not exact")
    result: dict[str, bytes] = {}
    for name in sorted(expected):
        content = value[name]
        if not isinstance(content, bytes) or not content or len(content) > MAX_INPUT_BYTES:
            raise ConanCenterMergeError(f"{label} file is empty, oversized, or not bytes: {name}")
        result[name] = content
    return result


@dataclass(frozen=True)
class MergeResult:
    state: str
    version: str
    version_count: int
    files: Mapping[str, bytes]

    def summary(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "state": self.state,
            "version": self.version,
            "version_count": self.version_count,
            "files": [
                {"path": path, "sha256": hashlib.sha256(self.files[path]).hexdigest()}
                for path in sorted(self.files)
            ],
        }


def merge_version_entry(
    upstream_config: object,
    upstream_conandata: object,
    folder_inventory: object,
    config_entry: object,
    conandata_entry: object,
    candidate_static_files: object,
    *,
    expected_version: str,
    existing_all_files: object | None = None,
) -> MergeResult:
    """Merge one version while protecting every prior shared recipe byte."""
    config = parse_config(upstream_config)
    conandata = parse_conandata(upstream_conandata)
    folders = parse_folder_inventory(folder_inventory)
    if set(config) != set(conandata):
        raise ConanCenterMergeError("upstream Conan version and source inventories disagree")
    expected_folders = () if not config else ("all",)
    if folders != expected_folders:
        raise ConanCenterMergeError("ConanCenter config and folder inventories disagree")
    entry = SignedVersionEntry.from_mappings(
        config_entry,
        conandata_entry,
        expected_version=expected_version,
    )
    static_files = _validate_file_mapping(
        candidate_static_files,
        expected=set(STATIC_RECIPE_FILES),
        label="signed Conan static recipe",
    )
    if config:
        if existing_all_files is None:
            raise ConanCenterMergeError("an existing shared all recipe requires exact file verification")
        existing = _validate_file_mapping(
            existing_all_files,
            expected=set(ALL_FOLDER_FILES),
            label="upstream Conan all folder",
        )
        if existing["conandata.yml"] != upstream_conandata.encode("utf-8"):
            raise ConanCenterMergeError("upstream Conan all/conandata.yml does not match the verified folder")
        existing_static = {name: existing[name] for name in STATIC_RECIPE_FILES}
        if existing_static != static_files:
            raise ConanCenterMergeError(
                "shared Conan recipe, dependency, or test bytes changed; "
                "manual version-folder migration required"
            )
    elif existing_all_files is not None:
        raise ConanCenterMergeError("a first Conan release unexpectedly has an existing all folder")

    existing_config = config.get(entry.version)
    existing_source = conandata.get(entry.version)
    if existing_config is None and existing_source is None:
        config[entry.version] = "all"
        conandata[entry.version] = entry.source
        state = "prepared"
    elif existing_config == "all" and existing_source == entry.source:
        state = "identical"
    else:
        raise ConanCenterMergeError("ConanCenter already contains a conflicting current version")

    rendered_config = render_config(config).encode("utf-8")
    rendered_conandata = render_conandata(conandata).encode("utf-8")
    files: dict[str, bytes] = {
        "config.yml": rendered_config,
        "all/conandata.yml": rendered_conandata,
    }
    files.update({f"all/{name}": content for name, content in static_files.items()})
    return MergeResult(state, entry.version, len(config), files)


def _read_bounded_file(path: Path, *, allow_empty: bool = False) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ConanCenterMergeError(f"input is missing or not a regular file: {path}")
    data = path.read_bytes()
    if len(data) > MAX_INPUT_BYTES or (not allow_empty and not data):
        raise ConanCenterMergeError(f"input is empty or exceeds the size limit: {path}")
    return data


def _read_utf8(path: Path, *, allow_empty: bool = False) -> str:
    data = _read_bounded_file(path, allow_empty=allow_empty)
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ConanCenterMergeError(f"input is not UTF-8: {path}") from error


def _read_json(path: Path) -> object:
    try:
        return json.loads(_read_utf8(path))
    except json.JSONDecodeError as error:
        raise ConanCenterMergeError(f"input is not JSON: {path}") from error


def _read_existing_all(path: Path) -> dict[str, bytes]:
    if path.is_symlink() or not path.is_dir():
        raise ConanCenterMergeError("upstream Conan all folder is missing or unsafe")
    directories: set[str] = set()
    files: dict[str, bytes] = {}
    for candidate in path.rglob("*"):
        relative = candidate.relative_to(path).as_posix()
        if candidate.is_symlink():
            raise ConanCenterMergeError("upstream Conan all folder contains a symlink")
        if candidate.is_dir():
            directories.add(relative)
        elif candidate.is_file():
            files[relative] = _read_bounded_file(candidate)
        else:
            raise ConanCenterMergeError("upstream Conan all folder contains a special file")
    if directories != {"test_package"}:
        raise ConanCenterMergeError("upstream Conan all folder directory inventory is not exact")
    return files


def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    except BaseException:
        Path(temporary_name).unlink(missing_ok=True)
        raise


def _write_output_tree(root: Path, files: Mapping[str, bytes]) -> None:
    if root.is_symlink() or (root.exists() and not root.is_dir()):
        raise ConanCenterMergeError("output root is unsafe")
    if root.exists() and any(root.iterdir()):
        raise ConanCenterMergeError("output root must be empty")
    root.mkdir(parents=True, exist_ok=True)
    for relative, content in files.items():
        _write_atomic(root / relative, content)


def _write_github_output(path: Path, result: MergeResult) -> None:
    with path.open("a", encoding="ascii") as output:
        output.write(f"state={result.state}\nversion_count={result.version_count}\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--upstream-config", type=Path, required=True)
    parser.add_argument("--upstream-conandata", type=Path, required=True)
    parser.add_argument("--folder-inventory", type=Path, required=True)
    parser.add_argument("--config-entry", type=Path, required=True)
    parser.add_argument("--conandata-entry", type=Path, required=True)
    parser.add_argument("--conanfile", type=Path, required=True)
    parser.add_argument("--test-cmakelists", type=Path, required=True)
    parser.add_argument("--test-conanfile", type=Path, required=True)
    parser.add_argument("--test-source", type=Path, required=True)
    parser.add_argument("--existing-all", type=Path)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--result", type=Path)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args(argv)
    try:
        output_root = args.output_root.resolve()
        sidecars = (path for path in (args.result, args.github_output) if path is not None)
        if any(path.resolve().is_relative_to(output_root) for path in sidecars):
            raise ConanCenterMergeError("result and GitHub output files must be outside the recipe output root")
        static_files = {
            "conanfile.py": _read_bounded_file(args.conanfile),
            "test_package/CMakeLists.txt": _read_bounded_file(args.test_cmakelists),
            "test_package/conanfile.py": _read_bounded_file(args.test_conanfile),
            "test_package/test_package.cpp": _read_bounded_file(args.test_source),
        }
        existing_all = _read_existing_all(args.existing_all) if args.existing_all else None
        result = merge_version_entry(
            _read_utf8(args.upstream_config, allow_empty=True),
            _read_utf8(args.upstream_conandata, allow_empty=True),
            _read_json(args.folder_inventory),
            _read_json(args.config_entry),
            _read_json(args.conandata_entry),
            static_files,
            expected_version=args.version,
            existing_all_files=existing_all,
        )
        summary = (json.dumps(result.summary(), sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
        _write_output_tree(args.output_root, result.files)
        if args.result is not None:
            _write_atomic(args.result, summary)
        if args.result is None:
            sys.stdout.buffer.write(summary)
        if args.github_output is not None:
            _write_github_output(args.github_output, result)
    except (ConanCenterMergeError, OSError, UnicodeError) as error:
        print(f"conan-center-merge: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
