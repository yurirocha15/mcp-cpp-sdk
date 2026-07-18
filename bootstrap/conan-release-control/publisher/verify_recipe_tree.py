#!/usr/bin/env python3
"""Reject unsafe or incomplete ConanCenter recipe trees."""

from __future__ import annotations

from pathlib import Path
import json
import stat
import sys


ALLOWED_NAMES = {
    "config.yml",
    "conandata.yml",
    "conanfile.py",
    "CMakeLists.txt",
    "test_package.cpp",
}
ALLOWED_SUFFIXES = {".patch", ".py", ".txt", ".yml", ".yaml", ".cmake", ".cpp", ".hpp"}


def verify(
    root: Path, *, version: str, source_url: str, source_sha256: str, release_manifest: Path
) -> None:
    if not root.is_dir() or root.is_symlink():
        raise ValueError("recipe root is not a real directory")
    files = []
    for path in sorted(root.rglob("*")):
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise ValueError(f"recipe contains a non-regular entry: {path.relative_to(root)}")
        if mode & 0o111:
            raise ValueError(f"recipe contains an executable file: {path.relative_to(root)}")
        if path.name not in ALLOWED_NAMES and path.suffix not in ALLOWED_SUFFIXES:
            raise ValueError(f"recipe contains an unapproved file type: {path.relative_to(root)}")
        files.append(path)
    if not files:
        raise ValueError("recipe tree is empty")
    if not any(path.name == "conanfile.py" for path in files):
        raise ValueError("recipe tree has no conanfile.py")
    if not any("test_package" in path.parts and path.name == "conanfile.py" for path in files):
        raise ValueError("recipe tree has no test_package/conanfile.py")
    combined = "\n".join(path.read_text(encoding="utf-8") for path in files)
    for required in (version, source_url, source_sha256, "license"):
        if required not in combined:
            raise ValueError(f"recipe tree does not bind required release value: {required}")
    if "http://" in combined:
        raise ValueError("recipe tree contains an insecure source URL")
    manifest = json.loads(release_manifest.read_text(encoding="utf-8"))
    closure = manifest.get("dependency_closure")
    if not isinstance(closure, list) or not closure:
        raise ValueError("release manifest dependency closure is missing")
    for dependency in closure:
        if not isinstance(dependency, dict):
            raise ValueError("release dependency record is malformed")
        reference = dependency.get("reference")
        if not isinstance(reference, str):
            name = dependency.get("name")
            dependency_version = dependency.get("version")
            if not isinstance(name, str) or not isinstance(dependency_version, str):
                raise ValueError("release dependency has no canonical reference")
            reference = f"{name}/{dependency_version}"
        if reference not in combined:
            raise ValueError(f"recipe omits locked dependency: {reference}")


def main() -> int:
    if len(sys.argv) != 6:
        print("usage: verify_recipe_tree.py TREE VERSION SOURCE_URL SOURCE_SHA256 RELEASE_MANIFEST", file=sys.stderr)
        return 2
    try:
        verify(
            Path(sys.argv[1]),
            version=sys.argv[2],
            source_url=sys.argv[3],
            source_sha256=sys.argv[4],
            release_manifest=Path(sys.argv[5]),
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(f"verify-recipe-tree: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
