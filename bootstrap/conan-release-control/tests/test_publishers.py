from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from publisher.validate_dispatch import validate
from publisher.verify_recipe_tree import verify as verify_recipe


class DispatchTests(unittest.TestCase):
    def test_stable_request(self) -> None:
        request = {
            "source_tag": "v0.2.0",
            "source_commit_sha": "0" * 40,
            "github_release_id": "1",
            "source_workflow_run_id": "2",
            "release_manifest_sha256": "1" * 64,
            "fork_branch": "package/mcp-cpp-sdk-0.2.0",
            "fork_head_sha": "2" * 40,
            "recipe_tree_sha256": "3" * 64,
            "request_uuid": "00000000-0000-0000-0000-000000000001",
        }
        self.assertEqual(validate(request), request)

    def test_rejects_rc(self) -> None:
        with self.assertRaises(ValueError):
            validate({"source_tag": "v0.2.0-rc.1"})


class RecipeTests(unittest.TestCase):
    def test_recipe_binds_source_and_locked_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "recipe"
            (root / "all/test_package").mkdir(parents=True)
            source_url = "https://github.com/yurirocha15/mcp-cpp-sdk/releases/download/v0.2.0/mcp-cpp-sdk-0.2.0.tar.gz"
            digest = "1" * 64
            (root / "all/conanfile.py").write_text('license = "MIT"\nrequires = "boost/1.86.0"\n')
            (root / "all/conandata.yml").write_text(f'0.2.0:\n  url: "{source_url}"\n  sha256: "{digest}"\n')
            (root / "all/test_package/conanfile.py").write_text("# test_package\n")
            manifest = Path(directory) / "manifest.json"
            manifest.write_text(json.dumps({"dependency_closure": [{"reference": "boost/1.86.0"}]}))
            verify_recipe(
                root,
                version="0.2.0",
                source_url=source_url,
                source_sha256=digest,
                release_manifest=manifest,
            )
