#!/usr/bin/env python3
"""Create or verify the exact ConanCenter recipe branch in the maintainer fork."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile
from typing import Callable, Mapping, Sequence
from urllib.parse import quote

from .conan_center_merge import main as merge_recipe
from .conan_center_merge import version_key
from .github_provider import ApiRequest, GhApi, GitHubProviderError


UPSTREAM = "conan-io/conan-center-index"
BRANCH_PREFIX = "package/mcp-cpp-sdk-"
RECIPE_FILES = {
    "recipes/mcp-cpp-sdk/config.yml": "config.yml",
    "recipes/mcp-cpp-sdk/all/conandata.yml": "all/conandata.yml",
    "recipes/mcp-cpp-sdk/all/conanfile.py": "all/conanfile.py",
    "recipes/mcp-cpp-sdk/all/test_package/CMakeLists.txt": "all/test_package/CMakeLists.txt",
    "recipes/mcp-cpp-sdk/all/test_package/conanfile.py": "all/test_package/conanfile.py",
    "recipes/mcp-cpp-sdk/all/test_package/test_package.cpp": "all/test_package/test_package.cpp",
}
SHA = re.compile(r"[0-9a-f]{40}")
REPOSITORY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}")
ArchiveDigest = Callable[[str, str, str], str]


class ConanForkError(ValueError):
    """Raised when the fork branch cannot be proven exact and recipe-only."""


def _mapping(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise ConanForkError(f"{label} is not an object")
    return value


def _sha(value: object, label: str) -> str:
    if not isinstance(value, str) or SHA.fullmatch(value) is None:
        raise ConanForkError(f"{label} is not a full lowercase commit SHA")
    return value


def _recipe_files(root: Path) -> dict[str, bytes]:
    if root.is_symlink() or not root.is_dir():
        raise ConanForkError("merged recipe root is missing or unsafe")
    files: dict[str, bytes] = {}
    for remote, relative in RECIPE_FILES.items():
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise ConanForkError(f"merged recipe file is missing or unsafe: {relative}")
        content = path.read_bytes()
        if not content or len(content) > 2 * 1024 * 1024:
            raise ConanForkError(f"merged recipe file is empty or oversized: {relative}")
        files[remote] = content
    return files


def _decode_content(value: object) -> bytes:
    response = _mapping(value, "GitHub content response")
    content = response.get("content")
    if not isinstance(content, str):
        raise ConanForkError("GitHub content response lacks base64 content")
    compact = "".join(content.split())
    try:
        decoded = base64.b64decode(compact, validate=True)
    except (ValueError, binascii.Error) as error:
        raise ConanForkError("GitHub content response is not canonical base64") from error
    if base64.b64encode(decoded).decode("ascii") != compact:
        raise ConanForkError("GitHub content response is not canonical base64")
    encoding = response.get("encoding")
    size = response.get("size")
    if encoding not in (None, "base64") or size not in (None, len(decoded)):
        raise ConanForkError("GitHub content response metadata is inconsistent")
    if not decoded or len(decoded) > 2 * 1024 * 1024:
        raise ConanForkError("GitHub content response is empty or oversized")
    return decoded


def _fork_base(
    api: ApiRequest, *, owner: str, repository: str, repository_id: int
) -> tuple[str, str]:
    if REPOSITORY.fullmatch(owner) is None or REPOSITORY.fullmatch(repository) is None:
        raise ConanForkError("Conan fork repository identity is malformed")
    if type(repository_id) is not int or repository_id < 1:
        raise ConanForkError("Conan fork repository ID is malformed")
    target = f"{owner}/{repository}"
    repository_value = _mapping(api("GET", f"repos/{target}"), "Conan fork")
    parent = _mapping(repository_value.get("parent"), "Conan fork parent")
    if (
        repository_value.get("id") != repository_id
        or repository_value.get("full_name") != target
        or repository_value.get("fork") is not True
        or repository_value.get("default_branch") != "master"
        or parent.get("full_name") != UPSTREAM
    ):
        raise ConanForkError("Conan fork identity differs from reviewed configuration")
    fork_ref = _mapping(
        api("GET", f"repos/{target}/git/ref/heads/master"), "Conan fork base ref"
    )
    upstream_ref = _mapping(
        api("GET", f"repos/{UPSTREAM}/git/ref/heads/master"),
        "Conan upstream base ref",
    )
    fork_base = _sha(
        _mapping(fork_ref.get("object"), "fork ref object").get("sha"),
        "fork ref",
    )
    upstream_base = _sha(
        _mapping(upstream_ref.get("object"), "upstream ref object").get("sha"),
        "upstream ref",
    )
    if fork_base != upstream_base:
        raise ConanForkError("Conan fork master is not synchronized to upstream")
    return target, fork_base


def _tree(api: ApiRequest, tree_sha: str, label: str) -> dict[str, Mapping[str, object]]:
    response = _mapping(api("GET", f"repos/{UPSTREAM}/git/trees/{tree_sha}"), label)
    if response.get("truncated") is not False:
        raise ConanForkError(f"{label} truncation state is not exact")
    values = response.get("tree")
    if not isinstance(values, list):
        raise ConanForkError(f"{label} lacks a tree")
    entries: dict[str, Mapping[str, object]] = {}
    for raw_entry in values:
        entry = _mapping(raw_entry, f"{label} entry")
        name = entry.get("path")
        if (
            not isinstance(name, str)
            or not name
            or "/" in name
            or name in entries
            or entry.get("type") not in {"blob", "tree"}
        ):
            raise ConanForkError(f"{label} contains an unsafe or duplicate entry")
        expected_mode = "100644" if entry["type"] == "blob" else "040000"
        if entry.get("mode") != expected_mode:
            raise ConanForkError(f"{label} contains an unsafe entry mode")
        _sha(entry.get("sha"), f"{label} {name}")
        entries[name] = entry
    return entries


def _child(
    entries: Mapping[str, Mapping[str, object]],
    name: str,
    entry_type: str,
    label: str,
) -> str:
    entry = entries.get(name)
    if entry is None or entry.get("type") != entry_type:
        raise ConanForkError(f"{label} must contain {name} as a {entry_type}")
    return _sha(entry.get("sha"), f"{label} {name}")


def _exact_names(
    entries: Mapping[str, Mapping[str, object]], expected: set[str], label: str
) -> None:
    if set(entries) != expected:
        raise ConanForkError(f"{label} inventory differs from the reviewed recipe")


def load_upstream_recipe(
    api: ApiRequest,
    *,
    owner: str,
    repository: str,
    repository_id: int,
) -> tuple[str, dict[str, bytes] | None]:
    """Read the exact upstream recipe tree at the synchronized fork base."""

    _, fork_base = _fork_base(
        api, owner=owner, repository=repository, repository_id=repository_id
    )
    commit = _mapping(
        api("GET", f"repos/{UPSTREAM}/git/commits/{fork_base}"),
        "Conan upstream commit",
    )
    root_sha = _sha(
        _mapping(commit.get("tree"), "Conan upstream commit tree").get("sha"),
        "Conan upstream root tree",
    )
    recipes = _tree(
        api,
        _child(_tree(api, root_sha, "upstream root"), "recipes", "tree", "upstream root"),
        "upstream recipes",
    )
    package_entry = recipes.get("mcp-cpp-sdk")
    if package_entry is None:
        return fork_base, None
    if package_entry.get("type") != "tree":
        raise ConanForkError("upstream mcp-cpp-sdk recipe is not a tree")
    package = _tree(
        api,
        _sha(package_entry.get("sha"), "upstream package tree"),
        "upstream package",
    )
    _exact_names(package, {"config.yml", "all"}, "upstream package")
    all_entries = _tree(
        api,
        _child(package, "all", "tree", "upstream package"),
        "upstream all",
    )
    _exact_names(
        all_entries,
        {"conandata.yml", "conanfile.py", "test_package"},
        "upstream all",
    )
    test_entries = _tree(
        api,
        _child(all_entries, "test_package", "tree", "upstream all"),
        "upstream test_package",
    )
    _exact_names(
        test_entries,
        {"CMakeLists.txt", "conanfile.py", "test_package.cpp"},
        "upstream test_package",
    )
    blobs = {
        "config.yml": _child(package, "config.yml", "blob", "upstream package"),
        "all/conandata.yml": _child(
            all_entries, "conandata.yml", "blob", "upstream all"
        ),
        "all/conanfile.py": _child(
            all_entries, "conanfile.py", "blob", "upstream all"
        ),
        "all/test_package/CMakeLists.txt": _child(
            test_entries, "CMakeLists.txt", "blob", "upstream test_package"
        ),
        "all/test_package/conanfile.py": _child(
            test_entries, "conanfile.py", "blob", "upstream test_package"
        ),
        "all/test_package/test_package.cpp": _child(
            test_entries, "test_package.cpp", "blob", "upstream test_package"
        ),
    }
    return fork_base, {
        path: _decode_content(api("GET", f"repos/{UPSTREAM}/git/blobs/{sha}"))
        for path, sha in blobs.items()
    }


def prepare_recipe(
    api: ApiRequest,
    *,
    owner: str,
    repository: str,
    repository_id: int,
    version: str,
    release_assets: Path,
    output_root: Path,
    result: Path,
    github_output: Path,
) -> None:
    """Merge signed entries with the exact synchronized upstream recipe."""

    version_key(version)
    fork_base, upstream = load_upstream_recipe(
        api, owner=owner, repository=repository, repository_id=repository_id
    )
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        config = root / "config.yml"
        conandata = root / "conandata.yml"
        inventory = root / "folder-inventory.json"
        arguments = [
            "--version", version,
            "--upstream-config", str(config),
            "--upstream-conandata", str(conandata),
            "--folder-inventory", str(inventory),
            "--config-entry", str(release_assets / "conan-recipe-config-entry.json"),
            "--conandata-entry", str(release_assets / "conan-recipe-conandata-entry.json"),
            "--conanfile", str(release_assets / "conan-recipe-conanfile.py"),
            "--test-cmakelists", str(release_assets / "conan-recipe-test-CMakeLists.txt"),
            "--test-conanfile", str(release_assets / "conan-recipe-test-conanfile.py"),
            "--test-source", str(release_assets / "conan-recipe-test-test_package.cpp"),
            "--output-root", str(output_root),
            "--result", str(result),
            "--github-output", str(github_output),
        ]
        if upstream is None:
            config.write_bytes(b"")
            conandata.write_bytes(b"")
            inventory.write_text('{"schema_version":1,"folders":[]}\n', encoding="ascii")
        else:
            config.write_bytes(upstream["config.yml"])
            conandata.write_bytes(upstream["all/conandata.yml"])
            inventory.write_text(
                '{"schema_version":1,"folders":["all"]}\n', encoding="ascii"
            )
            existing = root / "all"
            for relative, content in upstream.items():
                if not relative.startswith("all/"):
                    continue
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            arguments.extend(("--existing-all", str(existing)))
        if merge_recipe(arguments) != 0:
            raise ConanForkError("ConanCenter recipe merge failed")
    with github_output.open("a", encoding="ascii") as output:
        output.write(f"fork_base={fork_base}\n")


def _git_archive_digest(repository: str, branch: str, head: str) -> str:
    with tempfile.TemporaryDirectory() as directory:
        bare = Path(directory) / "recipe.git"
        subprocess.run(
            ["git", "init", "--bare", str(bare)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=30,
        )
        subprocess.run(
            [
                "git", "-C", str(bare), "fetch", "--no-tags", "--depth=1",
                f"https://github.com/{repository}.git",
                f"refs/heads/{branch}:refs/heads/recipe",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=120,
        )
        observed = subprocess.run(
            ["git", "-C", str(bare), "rev-parse", "refs/heads/recipe"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=30,
        ).stdout.strip()
        if observed != head:
            raise ConanForkError("fetched recipe branch head changed after publication")
        archive = subprocess.run(
            [
                "git", "-C", str(bare), "archive", "--format=tar", "--prefix=recipe/",
                "refs/heads/recipe:recipes/mcp-cpp-sdk",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=30,
        ).stdout
    return hashlib.sha256(archive).hexdigest()


def publish_recipe_branch(
    api: ApiRequest,
    *,
    owner: str,
    repository: str,
    repository_id: int,
    version: str,
    fork_base: str,
    recipe_root: Path,
    archive_digest: ArchiveDigest = _git_archive_digest,
) -> tuple[str, str, str]:
    """Create an absent branch or prove an existing branch byte-identical."""

    version_key(version)
    fork_base = _sha(fork_base, "fork base")
    target, observed_base = _fork_base(
        api, owner=owner, repository=repository, repository_id=repository_id
    )
    if observed_base != fork_base:
        raise ConanForkError("Conan fork master is not the approved upstream base")

    files = _recipe_files(recipe_root)
    branch = f"{BRANCH_PREFIX}{version}"
    refs = api("GET", f"repos/{target}/git/matching-refs/heads/{branch}")
    if not isinstance(refs, list):
        raise ConanForkError("Conan fork branch response is not a list")
    exact = [
        _mapping(item, "Conan branch ref")
        for item in refs
        if isinstance(item, Mapping) and item.get("ref") == f"refs/heads/{branch}"
    ]
    if len(exact) > 1:
        raise ConanForkError("Conan fork branch identity is ambiguous")
    if exact:
        head = _sha(
            _mapping(exact[0].get("object"), "branch ref object").get("sha"),
            "branch head",
        )
        encoded_branch = quote(branch, safe="")
        for remote, content in files.items():
            observed = api(
                "GET", f"repos/{target}/contents/{remote}?ref={encoded_branch}"
            )
            if _decode_content(observed) != content:
                raise ConanForkError(f"existing Conan recipe branch differs: {remote}")
    else:
        base_commit = _mapping(
            api("GET", f"repos/{target}/git/commits/{fork_base}"), "base commit"
        )
        base_tree = _sha(
            _mapping(base_commit.get("tree"), "base tree").get("sha"), "base tree"
        )
        entries: list[dict[str, str]] = []
        for remote, content in sorted(files.items()):
            response = _mapping(
                api(
                    "POST",
                    f"repos/{target}/git/blobs",
                    {"content": base64.b64encode(content).decode("ascii"), "encoding": "base64"},
                ),
                "created blob",
            )
            entries.append(
                {
                    "path": remote,
                    "mode": "100644",
                    "type": "blob",
                    "sha": _sha(response.get("sha"), "blob"),
                }
            )
        tree = _mapping(
            api("POST", f"repos/{target}/git/trees", {"base_tree": base_tree, "tree": entries}),
            "created tree",
        )
        commit = _mapping(
            api(
                "POST",
                f"repos/{target}/git/commits",
                {
                    "message": f"mcp-cpp-sdk {version}",
                    "tree": _sha(tree.get("sha"), "tree"),
                    "parents": [fork_base],
                },
            ),
            "created commit",
        )
        head = _sha(commit.get("sha"), "created commit")
        api("POST", f"repos/{target}/git/refs", {"ref": f"refs/heads/{branch}", "sha": head})

    commit = _mapping(api("GET", f"repos/{target}/git/commits/{head}"), "recipe commit")
    parents = commit.get("parents")
    if (
        not isinstance(parents, list)
        or len(parents) != 1
        or _mapping(parents[0], "recipe parent").get("sha") != fork_base
    ):
        raise ConanForkError("recipe commit is not a single child of the approved base")
    comparison = _mapping(
        api("GET", f"repos/{target}/compare/master...{head}"), "recipe comparison"
    )
    changed = comparison.get("files")
    if not isinstance(changed, list):
        raise ConanForkError("recipe comparison lacks a file list")
    changed_names: set[str] = set()
    for item in changed:
        filename = _mapping(item, "changed recipe file").get("filename")
        if not isinstance(filename, str) or filename in changed_names:
            raise ConanForkError("recipe comparison contains an unsafe duplicate path")
        changed_names.add(filename)
    if (
        not changed_names <= set(RECIPE_FILES)
        or not {"recipes/mcp-cpp-sdk/config.yml", "recipes/mcp-cpp-sdk/all/conandata.yml"} <= changed_names
    ):
        raise ConanForkError("recipe branch changes are incomplete or outside the package recipe")
    digest = archive_digest(target, branch, head)
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ConanForkError("recipe archive digest is malformed")
    return branch, head, digest


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare")
    publish = commands.add_parser("publish")
    for command in (prepare, publish):
        command.add_argument("--owner", required=True)
        command.add_argument("--repository", required=True)
        command.add_argument("--repository-id", required=True, type=int)
        command.add_argument("--version", required=True)
        command.add_argument("--github-output", required=True, type=Path)
    prepare.add_argument("--release-assets", required=True, type=Path)
    prepare.add_argument("--output-root", required=True, type=Path)
    prepare.add_argument("--result", required=True, type=Path)
    publish.add_argument("--fork-base", required=True)
    publish.add_argument("--recipe-root", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            prepare_recipe(
                GhApi(),
                owner=args.owner,
                repository=args.repository,
                repository_id=args.repository_id,
                version=args.version,
                release_assets=args.release_assets,
                output_root=args.output_root,
                result=args.result,
                github_output=args.github_output,
            )
        else:
            branch, head, digest = publish_recipe_branch(
                GhApi(),
                owner=args.owner,
                repository=args.repository,
                repository_id=args.repository_id,
                version=args.version,
                fork_base=args.fork_base,
                recipe_root=args.recipe_root,
            )
            with args.github_output.open("a", encoding="ascii") as output:
                output.write(
                    f"branch={branch}\nhead_sha={head}\ntree_sha256={digest}\n"
                )
    except (OSError, subprocess.SubprocessError, GitHubProviderError, ConanForkError, ValueError) as error:
        raise SystemExit(f"conan-fork: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
