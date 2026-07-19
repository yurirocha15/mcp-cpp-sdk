from __future__ import annotations

import base64
from pathlib import Path
import tempfile
import unittest

from release.conan_fork import (
    ConanForkError,
    RECIPE_FILES,
    load_upstream_recipe,
    publish_recipe_branch,
)


BASE = "a" * 40
HEAD = "b" * 40
TREE = "c" * 40
DIGEST = "d" * 64
TARGET = "owner/conan-center-index"


def sha(character: str) -> str:
    return character * 40


class FakeApi:
    def __init__(self, *, refs: list[object], files: dict[str, bytes]) -> None:
        self.refs = refs
        self.files = files
        self.calls: list[tuple[str, str, object | None]] = []
        self.blob = 0

    def __call__(self, method: str, endpoint: str, payload=None):
        self.calls.append((method, endpoint, payload))
        if endpoint == f"repos/{TARGET}":
            return {
                "id": 17,
                "full_name": TARGET,
                "fork": True,
                "default_branch": "master",
                "parent": {"full_name": "conan-io/conan-center-index"},
            }
        if endpoint in {
            f"repos/{TARGET}/git/ref/heads/master",
            "repos/conan-io/conan-center-index/git/ref/heads/master",
        }:
            return {"object": {"sha": BASE}}
        if endpoint == f"repos/{TARGET}/git/matching-refs/heads/package/mcp-cpp-sdk-0.2.0":
            return self.refs
        if endpoint == f"repos/{TARGET}/git/commits/{BASE}":
            return {"tree": {"sha": TREE}}
        if method == "POST" and endpoint == f"repos/{TARGET}/git/blobs":
            self.blob += 1
            return {"sha": f"{self.blob:040x}"}
        if method == "POST" and endpoint == f"repos/{TARGET}/git/trees":
            return {"sha": TREE}
        if method == "POST" and endpoint == f"repos/{TARGET}/git/commits":
            return {"sha": HEAD}
        if method == "POST" and endpoint == f"repos/{TARGET}/git/refs":
            return {"ref": "created"}
        if endpoint == f"repos/{TARGET}/git/commits/{HEAD}":
            return {"parents": [{"sha": BASE}]}
        if endpoint == f"repos/{TARGET}/compare/master...{HEAD}":
            return {"files": [{"filename": name} for name in RECIPE_FILES]}
        prefix = f"repos/{TARGET}/contents/"
        if endpoint.startswith(prefix):
            remote = endpoint[len(prefix) :].split("?", 1)[0]
            content = self.files[remote]
            return {
                "content": base64.b64encode(content).decode("ascii"),
                "encoding": "base64",
                "size": len(content),
            }
        raise AssertionError(f"unexpected API request: {method} {endpoint}")


class UpstreamApi(FakeApi):
    def __init__(
        self, *, extra_package_entry: bool = False, package_exists: bool = True
    ) -> None:
        super().__init__(refs=[], files={})
        self.contents = {
            "config.yml": b"versions:\n",
            "all/conandata.yml": b"sources:\n",
            "all/conanfile.py": b"recipe\n",
            "all/test_package/CMakeLists.txt": b"cmake\n",
            "all/test_package/conanfile.py": b"test recipe\n",
            "all/test_package/test_package.cpp": b"test source\n",
        }
        package_entries = [
            self.entry("config.yml", "blob", "1"),
            self.entry("all", "tree", "e"),
        ]
        if extra_package_entry:
            package_entries.append(self.entry("README.md", "blob", "2"))
        self.trees = {
            sha("c"): [self.entry("recipes", "tree", "d")],
            sha("d"): (
                [self.entry("mcp-cpp-sdk", "tree", "f")] if package_exists else []
            ),
            sha("f"): package_entries,
            sha("e"): [
                self.entry("conandata.yml", "blob", "2"),
                self.entry("conanfile.py", "blob", "3"),
                self.entry("test_package", "tree", "4"),
            ],
            sha("4"): [
                self.entry("CMakeLists.txt", "blob", "5"),
                self.entry("conanfile.py", "blob", "6"),
                self.entry("test_package.cpp", "blob", "7"),
            ],
        }
        self.blobs = {
            sha("1"): self.contents["config.yml"],
            sha("2"): self.contents["all/conandata.yml"],
            sha("3"): self.contents["all/conanfile.py"],
            sha("5"): self.contents["all/test_package/CMakeLists.txt"],
            sha("6"): self.contents["all/test_package/conanfile.py"],
            sha("7"): self.contents["all/test_package/test_package.cpp"],
        }

    @staticmethod
    def entry(path: str, entry_type: str, digest: str) -> dict[str, str]:
        return {
            "path": path,
            "type": entry_type,
            "mode": "100644" if entry_type == "blob" else "040000",
            "sha": sha(digest),
        }

    def __call__(self, method: str, endpoint: str, payload=None):
        if endpoint == f"repos/conan-io/conan-center-index/git/commits/{BASE}":
            return {"tree": {"sha": sha("c")}}
        tree_prefix = "repos/conan-io/conan-center-index/git/trees/"
        if endpoint.startswith(tree_prefix):
            return {
                "tree": self.trees[endpoint.removeprefix(tree_prefix)],
                "truncated": False,
            }
        blob_prefix = "repos/conan-io/conan-center-index/git/blobs/"
        if endpoint.startswith(blob_prefix):
            content = self.blobs[endpoint.removeprefix(blob_prefix)]
            return {
                "content": base64.b64encode(content).decode("ascii"),
                "encoding": "base64",
                "size": len(content),
            }
        return super().__call__(method, endpoint, payload)


class ConanForkTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "merged-recipe"
        self.files: dict[str, bytes] = {}
        for remote, relative in RECIPE_FILES.items():
            content = f"content for {relative}\n".encode()
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            self.files[remote] = content

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def publish(self, api: FakeApi):
        return publish_recipe_branch(
            api,
            owner="owner",
            repository="conan-center-index",
            repository_id=17,
            version="0.2.0",
            fork_base=BASE,
            recipe_root=self.root,
            archive_digest=lambda repository, branch, head: DIGEST,
        )

    def test_creates_exact_recipe_only_branch_from_approved_base(self) -> None:
        api = FakeApi(refs=[], files=self.files)
        self.assertEqual(
            self.publish(api),
            ("package/mcp-cpp-sdk-0.2.0", HEAD, DIGEST),
        )
        blobs = [call for call in api.calls if call[1].endswith("/git/blobs")]
        self.assertEqual(len(blobs), len(RECIPE_FILES))
        commit = next(call for call in api.calls if call[1].endswith("/git/commits") and call[0] == "POST")
        self.assertEqual(commit[2]["parents"], [BASE])

    def test_existing_branch_must_be_byte_identical(self) -> None:
        refs = [{"ref": "refs/heads/package/mcp-cpp-sdk-0.2.0", "object": {"sha": HEAD}}]
        self.assertEqual(self.publish(FakeApi(refs=refs, files=self.files))[1], HEAD)
        changed = dict(self.files)
        changed[next(iter(changed))] = b"different\n"
        with self.assertRaisesRegex(ConanForkError, "differs"):
            self.publish(FakeApi(refs=refs, files=changed))

    def test_rejects_ambiguous_branch_and_wrong_fork_identity(self) -> None:
        ref = {"ref": "refs/heads/package/mcp-cpp-sdk-0.2.0", "object": {"sha": HEAD}}
        with self.assertRaisesRegex(ConanForkError, "ambiguous"):
            self.publish(FakeApi(refs=[ref, ref], files=self.files))

        api = FakeApi(refs=[], files=self.files)
        original = api.__call__

        def wrong_repository(method: str, endpoint: str, payload=None):
            value = original(method, endpoint, payload)
            if endpoint == f"repos/{TARGET}":
                value["id"] = 18
            return value

        with self.assertRaises(ConanForkError):
            publish_recipe_branch(
                wrong_repository,
                owner="owner",
                repository="conan-center-index",
                repository_id=17,
                version="0.2.0",
                fork_base=BASE,
                recipe_root=self.root,
                archive_digest=lambda *_: DIGEST,
            )

    def test_reads_exact_synchronized_upstream_recipe_tree(self) -> None:
        api = UpstreamApi()
        base, files = load_upstream_recipe(
            api, owner="owner", repository="conan-center-index", repository_id=17
        )
        self.assertEqual(base, BASE)
        self.assertEqual(files, api.contents)

    def test_rejects_unreviewed_upstream_recipe_inventory(self) -> None:
        with self.assertRaisesRegex(ConanForkError, "inventory"):
            load_upstream_recipe(
                UpstreamApi(extra_package_entry=True),
                owner="owner",
                repository="conan-center-index",
                repository_id=17,
            )

    def test_absent_upstream_recipe_is_an_explicit_first_release(self) -> None:
        base, files = load_upstream_recipe(
            UpstreamApi(package_exists=False),
            owner="owner",
            repository="conan-center-index",
            repository_id=17,
        )
        self.assertEqual((base, files), (BASE, None))


if __name__ == "__main__":
    unittest.main()
