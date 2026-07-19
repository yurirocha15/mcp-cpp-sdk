"""Build and test the exact rendered ConanCenter candidate before anchoring.

ConanCenter's production recipe references the immutable GitHub Release, which
does not exist during candidate validation.  The validator separately proves
that URL and digest, then runs an otherwise byte-identical recipe against a
loopback-only HTTP endpoint serving the same candidate source archive.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from typing import Mapping, Sequence
from urllib.parse import urlsplit

from .artifacts import canonical_json_bytes, write_atomic
from .conan_center_merge import (
    ConanCenterMergeError,
    SignedVersionEntry,
    render_conandata,
    render_config,
)
from .model import ValidationError
from .loopback_archive import archive_server


_CONAN_VERSION = re.compile(r"Conan version ([0-9]+\.[0-9]+\.[0-9]+)")
_LOCK_REFERENCE = re.compile(r"[a-z0-9_+.-]+/[0-9][^#]*#[0-9a-f]{32}%[0-9]+(?:\.[0-9]+)?")
_EXACT_ASSETS = {
    "conan-recipe-config-entry.json",
    "conan-recipe-conandata-entry.json",
    "conan-recipe-conanfile.py",
    "conan-recipe-test-CMakeLists.txt",
    "conan-recipe-test-conanfile.py",
    "conan-recipe-test-test_package.cpp",
}
_STATIC_ASSETS = {
    "conanfile.py": "conan-recipe-conanfile.py",
    "test_package/CMakeLists.txt": "conan-recipe-test-CMakeLists.txt",
    "test_package/conanfile.py": "conan-recipe-test-conanfile.py",
    "test_package/test_package.cpp": "conan-recipe-test-test_package.cpp",
}


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValidationError(f"Conan candidate JSON contains duplicate field: {key!r}")
        value[key] = item
    return value


def _reject_constant(value: str) -> None:
    raise ValidationError(f"Conan candidate JSON contains non-finite number: {value}")


def _json_object(path: Path) -> Mapping[str, object]:
    if path.is_symlink() or not path.is_file():
        raise ValidationError(f"Conan candidate input is missing: {path.name}")
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
        )
    except (json.JSONDecodeError, UnicodeError) as error:
        raise ValidationError(f"Conan candidate input is malformed: {path.name}") from error
    if not isinstance(value, Mapping):
        raise ValidationError(f"Conan candidate input is not an object: {path.name}")
    return value


def _empty_directory(path: Path) -> None:
    if path.is_symlink() or path.exists():
        raise ValidationError("Conan validation output directories must be new")
    path.mkdir(parents=True)


def _validate_lockfile(path: Path) -> str:
    value = _json_object(path)
    if set(value) != {"version", "requires", "build_requires", "python_requires", "config_requires"}:
        raise ValidationError("Conan dependency lock schema is not exact")
    if value["version"] != "0.5":
        raise ValidationError("Conan dependency lock version is unsupported")
    references: list[str] = []
    for field in ("requires", "build_requires", "python_requires", "config_requires"):
        items = value[field]
        if not isinstance(items, list) or any(
            not isinstance(item, str) or _LOCK_REFERENCE.fullmatch(item) is None
            for item in items
        ):
            raise ValidationError("Conan dependency lock contains an invalid recipe revision")
        references.extend(items)
    if len(references) != len(set(references)):
        raise ValidationError("Conan dependency lock contains duplicate recipe revisions")
    for required in ("boost/1.86.0#", "nlohmann_json/3.12.0#", "openssl/3.6.3#"):
        if sum(reference.startswith(required) for reference in references) != 1:
            raise ValidationError(f"Conan dependency lock does not bind {required[:-1]}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _copy_regular(source: Path, destination: Path) -> None:
    if source.is_symlink() or not source.is_file() or not source.stat().st_size:
        raise ValidationError(f"Conan candidate input is empty or unsafe: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def _tree_sha256(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValidationError("Conan candidate tree contains a symbolic link")
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix().encode("utf-8")
        content = path.read_bytes()
        digest.update(len(relative).to_bytes(8, "big"))
        digest.update(relative)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def materialize_candidate(
    *,
    assets: Path,
    archive: Path,
    version: str,
    production: Path,
    adapted: Path,
    local_url: str,
) -> tuple[str, str]:
    """Materialize production and single-field-adapted recipe trees."""
    if assets.is_symlink() or not assets.is_dir():
        raise ValidationError("Conan candidate assets must be a real directory")
    present = {path.name for path in assets.iterdir() if path.is_file()}
    if not _EXACT_ASSETS.issubset(present):
        raise ValidationError("Conan candidate asset inventory is incomplete")
    if archive.is_symlink() or not archive.is_file():
        raise ValidationError("Conan candidate source archive must be a regular file")
    parsed = urlsplit(local_url)
    if (
        parsed.scheme != "http"
        or parsed.hostname != "127.0.0.1"
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise ValidationError("Conan adapted source must use credential-free loopback HTTP")

    try:
        entry = SignedVersionEntry.from_mappings(
            _json_object(assets / "conan-recipe-config-entry.json"),
            _json_object(assets / "conan-recipe-conandata-entry.json"),
            expected_version=version,
        )
    except ConanCenterMergeError as error:
        raise ValidationError(f"Conan candidate version entry is invalid: {error}") from error
    if archive.name != f"mcp-cpp-sdk-{version}.tar.gz":
        raise ValidationError("Conan candidate source archive name is unexpected")
    if hashlib.sha256(archive.read_bytes()).hexdigest() != entry.source.sha256:
        raise ValidationError("Conan candidate source archive differs from conandata")

    _empty_directory(production)
    _empty_directory(adapted)
    production_all = production / "all"
    production_all.mkdir()
    (production / "config.yml").write_text(
        render_config({version: "all"}), encoding="utf-8", newline="\n"
    )
    production_conandata = render_conandata({version: entry.source})
    (production_all / "conandata.yml").write_text(
        production_conandata, encoding="utf-8", newline="\n"
    )
    for destination, source in _STATIC_ASSETS.items():
        _copy_regular(assets / source, production_all / destination)

    shutil.copytree(production, adapted, dirs_exist_ok=True)
    adapted_conandata = production_conandata.replace(entry.source.url, local_url, 1)
    if (
        production_conandata.count(entry.source.url) != 1
        or local_url in production_conandata
        or adapted_conandata.replace(local_url, entry.source.url, 1) != production_conandata
    ):
        raise ValidationError("Conan source adaptation changed more than one URL field")
    (adapted / "all/conandata.yml").write_text(
        adapted_conandata, encoding="utf-8", newline="\n"
    )
    production_digest = _tree_sha256(production)
    if _tree_sha256(adapted) == production_digest:
        raise ValidationError("Conan adapted recipe unexpectedly equals the production recipe")
    return production_digest, hashlib.sha256(adapted_conandata.encode("utf-8")).hexdigest()


def _conan_version(conan: str, expected: str) -> None:
    completed = subprocess.run(
        [conan, "--version"],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    match = _CONAN_VERSION.fullmatch(completed.stdout.strip())
    if match is None or match.group(1) != expected:
        raise ValidationError("Conan executable differs from the reviewed exact version")


def validate_candidate(
    *,
    assets: Path,
    archive: Path,
    version: str,
    work: Path,
    conan: str,
    expected_conan_version: str,
    build_profile: Path,
    host_profile: Path,
    lockfile: Path,
    evidence: Path,
) -> None:
    """Run exact static and shared ``conan create`` package tests."""
    if work.exists():
        raise ValidationError("Conan validation work directory must be new")
    if build_profile.is_symlink() or not build_profile.is_file():
        raise ValidationError("Conan build profile must be a regular file")
    if host_profile.is_symlink() or not host_profile.is_file():
        raise ValidationError("Conan host profile must be a regular file")
    lockfile_digest = _validate_lockfile(lockfile)
    _conan_version(conan, expected_conan_version)
    work.mkdir(parents=True)
    results: list[dict[str, object]] = []
    with archive_server(archive) as local_url:
        production = work / "production"
        adapted = work / "adapted"
        production_digest, adapted_conandata_digest = materialize_candidate(
            assets=assets,
            archive=archive,
            version=version,
            production=production,
            adapted=adapted,
            local_url=local_url,
        )
        for shared in (False, True):
            graph = work / f"conan-create-shared-{str(shared).lower()}.json"
            command = [
                conan,
                "create",
                str(adapted / "all"),
                f"--version={version}",
                f"--profile:build={build_profile}",
                f"--profile:host={host_profile}",
                f"--lockfile={lockfile.resolve()}",
                f"-o=mcp-cpp-sdk/*:shared={shared}",
                "--build=mcp-cpp-sdk/*",
                "--build=missing",
                "--format=json",
                f"--out-file={graph}",
            ]
            subprocess.run(command, check=True, timeout=3600)
            value = _json_object(graph)
            if "graph" not in value:
                raise ValidationError("Conan create evidence has no dependency graph")
            results.append(
                {
                    "shared": shared,
                    "graph_sha256": hashlib.sha256(graph.read_bytes()).hexdigest(),
                }
            )
    write_atomic(
        evidence,
        canonical_json_bytes(
            {
                "schema_version": 1,
                "version": version,
                "conan_version": expected_conan_version,
                "dependency_lock_sha256": lockfile_digest,
                "source_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "production_recipe_tree_sha256": production_digest,
                "adapted_conandata_sha256": adapted_conandata_digest,
                "results": results,
            }
        ),
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--conan", default="conan")
    parser.add_argument("--expected-conan-version", required=True)
    parser.add_argument("--build-profile", type=Path, required=True)
    parser.add_argument("--host-profile", type=Path, required=True)
    parser.add_argument("--lockfile", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        validate_candidate(
            assets=args.assets,
            archive=args.archive,
            version=args.version,
            work=args.work,
            conan=args.conan,
            expected_conan_version=args.expected_conan_version,
            build_profile=args.build_profile,
            host_profile=args.host_profile,
            lockfile=args.lockfile,
            evidence=args.evidence,
        )
    except (
        OSError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
        ValidationError,
    ) as error:
        raise SystemExit(f"conan-validation: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
