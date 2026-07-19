from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from uuid import UUID

from publisher.handoff import HandoffError, artifact_name, create_handoff


class HandoffTests(unittest.TestCase):
    def arguments(self, root: Path) -> dict[str, object]:
        package = root / "mcp-cpp-sdk.0.2.0.nupkg"
        package.write_bytes(b"package")
        client = root / "push_package.ps1"
        client.write_text("# protected publisher\n", encoding="utf-8")
        return {
            "source_package": package,
            "output_directory": root / "submission",
            "source_tag": "v0.2.0",
            "source_commit_sha": "1" * 40,
            "release_manifest_sha256": "2" * 64,
            "package_name": package.name,
            "package_sha256": hashlib.sha256(b"package").hexdigest(),
            "request_uuid": str(UUID(int=1)),
            "run_id": "17",
            "run_attempt": "2",
            "publisher_client": client,
        }

    def test_creates_attempt_scoped_exact_handoff(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arguments = self.arguments(root)
            self.assertEqual(
                create_handoff(**arguments), "chocolatey-submission-17-2"
            )
            output = arguments["output_directory"]
            self.assertEqual(
                {path.name for path in output.iterdir()},
                {"identity.json", "mcp-cpp-sdk.0.2.0.nupkg", "push_package.ps1"},
            )
            identity = json.loads((output / "identity.json").read_text())
            self.assertEqual(identity["run_id"], "17")
            self.assertEqual(identity["run_attempt"], "2")
            self.assertEqual(identity["schema_version"], 2)
            self.assertEqual(identity["publisher_client_name"], "push_package.ps1")
            self.assertEqual(
                identity["publisher_client_sha256"],
                hashlib.sha256(b"# protected publisher\n").hexdigest(),
            )

    def test_rejects_malformed_or_changed_handoff_inputs(self) -> None:
        for field, replacement in (
            ("source_tag", "v0.2.0-rc.1"),
            ("source_commit_sha", "A" * 40),
            ("package_name", "different.0.2.0.nupkg"),
            ("package_sha256", "0" * 64),
            ("run_id", "0"),
            ("run_attempt", "01"),
        ):
            with tempfile.TemporaryDirectory() as directory:
                arguments = self.arguments(Path(directory))
                arguments[field] = replacement
                with self.subTest(field=field), self.assertRaises(HandoffError):
                    create_handoff(**arguments)

    def test_rejects_symlink_and_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arguments = self.arguments(root)
            linked = root / "linked.nupkg"
            linked.symlink_to(arguments["source_package"])
            arguments["source_package"] = linked
            with self.assertRaises(HandoffError):
                create_handoff(**arguments)

            arguments = self.arguments(root)
            linked_client_directory = root / "linked-client"
            linked_client_directory.mkdir()
            linked_client = linked_client_directory / "push_package.ps1"
            linked_client.symlink_to(arguments["publisher_client"])
            arguments["publisher_client"] = linked_client
            with self.assertRaises(HandoffError):
                create_handoff(**arguments)

            arguments = self.arguments(root)
            arguments["output_directory"].mkdir()
            with self.assertRaises(HandoffError):
                create_handoff(**arguments)

    def test_artifact_name_is_canonical(self) -> None:
        self.assertEqual(artifact_name("1", "9"), "chocolatey-submission-1-9")
        for run_id, attempt in (("01", "1"), ("1", "0"), ("x", "1")):
            with self.assertRaises(HandoffError):
                artifact_name(run_id, attempt)


if __name__ == "__main__":
    unittest.main()
