from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from release.model import ValidationError
from release.native_builder_bootstrap import (
    _pushed_digest,
    bootstrap_matrix,
    bootstrap_tag,
    merge_evidence,
    select_platform_digest,
)
from release.native_builder_lock import load_builder_lock


ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "release/native-builders/lock.json"
ONE = "1" * 64
TWO = "2" * 64


class NativeBuilderBootstrapTests(unittest.TestCase):
    def test_isolated_runner_can_load_the_bootstrap_module(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                "-I",
                "-S",
                str(ROOT / "scripts/run_release_tool.py"),
                "release.native_builder_bootstrap",
                "--help",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_matrix_uses_the_exact_hosted_runner_inventory(self) -> None:
        matrix = json.loads(bootstrap_matrix(lock=LOCK, stage="resolve-bases"))
        self.assertEqual(len(matrix["include"]), 20)
        self.assertEqual(
            {row["runner"] for row in matrix["include"]},
            {"ubuntu-24.04", "ubuntu-24.04-arm"},
        )

    def test_selects_one_platform_manifest(self) -> None:
        index = json.dumps(
            {
                "mediaType": "application/vnd.oci.image.index.v1+json",
                "manifests": [
                    {
                        "digest": f"sha256:{ONE}",
                        "platform": {"os": "linux", "architecture": "amd64"},
                    },
                    {
                        "digest": f"sha256:{TWO}",
                        "platform": {
                            "os": "linux",
                            "architecture": "arm64",
                            "variant": "v8",
                        },
                    },
                ],
            }
        ).encode("utf-8")
        self.assertEqual(select_platform_digest(index, "x86_64"), f"sha256:{ONE}")
        self.assertEqual(select_platform_digest(index, "aarch64"), f"sha256:{TWO}")
        with self.assertRaisesRegex(ValidationError, "exactly one"):
            select_platform_digest(index.replace(b'"amd64"', b'"386"'), "x86_64")

    def test_push_output_and_tags_are_unambiguous(self) -> None:
        output = f"tag: digest: sha256:{ONE} size: 123\n".encode("ascii")
        self.assertEqual(_pushed_digest(output), f"sha256:{ONE}")
        with self.assertRaises(ValidationError):
            _pushed_digest(
                output + f"other: digest: sha256:{TWO} size: 1\n".encode("ascii")
            )
        builders = load_builder_lock(LOCK, require_resolved=False)
        commit = "a" * 40
        tags = {bootstrap_tag(target, commit) for target in builders}
        self.assertEqual(len(tags), len(builders))

    def test_base_merge_requires_complete_evidence(self) -> None:
        builders = load_builder_lock(LOCK, require_resolved=False)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evidence = root / "evidence"
            evidence.mkdir()
            for target in builders:
                value = {
                    "schema_version": 1,
                    "stage": "base",
                    "target_id": target["id"],
                    "architecture": target["architecture"],
                    "base_image": target["base_image"],
                    "base_digest": f"sha256:{ONE}",
                    "manifest_index_sha256": TWO,
                }
                (evidence / f"{target['id']}.json").write_text(
                    json.dumps(value), encoding="utf-8"
                )
            output = root / "proposed-lock.json"
            merge_evidence(
                lock=LOCK,
                stage="base",
                evidence_directory=evidence,
                output=output,
            )
            merged = load_builder_lock(output, require_resolved=False)
            self.assertTrue(
                all(target["base_digest"] == f"sha256:{ONE}" for target in merged)
            )
            (evidence / f"{builders[-1]['id']}.json").unlink()
            with self.assertRaisesRegex(ValidationError, "inventory"):
                merge_evidence(
                    lock=LOCK,
                    stage="base",
                    evidence_directory=evidence,
                    output=root / "second-lock.json",
                )


if __name__ == "__main__":
    unittest.main()
