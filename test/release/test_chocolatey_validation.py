from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import textwrap
import unittest

from release.chocolatey_validation import ChocolateyValidationError, validate

# Stands in for the publisher's run_publisher_tool.py: it records its arguments and the
# assets it was given, then behaves as FAKE_MODE in its own directory says.
FAKE_TOOL = textwrap.dedent(
    """\
    import json, os, sys
    from pathlib import Path
    here = Path(__file__).parent
    mode = (here / "mode").read_text().strip()
    args = sys.argv[1:]
    directory = Path(args[args.index("--directory") + 1])
    (here / "record.json").write_text(json.dumps({
        "args": args, "assets": sorted(path.name for path in directory.iterdir()),
    }))
    if mode == "refuse":
        print("verify-release: the package has a forbidden file")
        raise SystemExit(2)
    package = directory / "mcp-cpp-sdk.0.3.0.nupkg"
    digest = "0" * 64 if mode == "other-digest" else __import__("hashlib").sha256(package.read_bytes()).hexdigest()
    with open(os.environ["GITHUB_OUTPUT"], "a") as stream:
        stream.write(f"package_name={package.name}\\npackage_sha256={digest}\\n")
    """
)


class ChocolateyValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.candidate = root / "candidate"
        self.candidate.mkdir()
        for name, content in (
            ("RELEASE_NOTES.md", "notes\n"),
            ("release-manifest.json", "{}\n"),
            ("mcp-cpp-sdk.0.3.0.nupkg", "package"),
            ("SHA256SUMS", "sums\n"),
        ):
            (self.candidate / name).write_text(content, encoding="utf-8")
        self.publisher = root / "publisher"
        (self.publisher / "keys").mkdir(parents=True)
        (self.publisher / "keys" / "release-signing-key.asc").write_text("key\n", encoding="utf-8")
        (self.publisher / "run_publisher_tool.py").write_text(FAKE_TOOL, encoding="utf-8")
        self.work = root / "work"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_validation(self, mode: str, tag: str = "v0.3.0") -> str:
        (self.publisher / "mode").write_text(mode, encoding="utf-8")
        return validate(
            candidate=self.candidate,
            publisher=self.publisher,
            tag=tag,
            commit="a" * 40,
            primary_fingerprint="P" * 40,
            artifact_fingerprint="A" * 40,
            publisher_repository_id="1305071592",
            work_directory=self.work,
        )

    def record(self) -> dict[str, object]:
        return json.loads((self.publisher / "record.json").read_text(encoding="utf-8"))

    def test_publisher_checks_the_release_assets(self) -> None:
        self.assertEqual(self.run_validation("accept"), "mcp-cpp-sdk.0.3.0.nupkg")
        record = self.record()
        self.assertEqual(record["assets"], ["SHA256SUMS", "mcp-cpp-sdk.0.3.0.nupkg", "release-manifest.json"])
        args = record["args"]
        self.assertEqual(args[0], "verify_release")
        manifest_digest = hashlib.sha256(b"{}\n").hexdigest()
        self.assertEqual(args[args.index("--manifest-sha256") + 1], manifest_digest)
        self.assertEqual(args[args.index("--publisher-repository-id") + 1], "1305071592")
        self.assertTrue(args[args.index("--public-key") + 1].endswith("keys/release-signing-key.asc"))

    def test_a_refusal_fails_with_the_publisher_message(self) -> None:
        with self.assertRaisesRegex(ChocolateyValidationError, "forbidden file"):
            self.run_validation("refuse")

    def test_the_accepted_package_must_be_the_candidate_package(self) -> None:
        with self.assertRaisesRegex(ChocolateyValidationError, "other than the candidate"):
            self.run_validation("other-digest")

    def test_only_a_stable_tag_and_a_complete_publisher_are_accepted(self) -> None:
        with self.assertRaises(ChocolateyValidationError):
            self.run_validation("accept", tag="v0.3.0-rc.1")
        (self.publisher / "run_publisher_tool.py").unlink()
        with self.assertRaisesRegex(ChocolateyValidationError, "lacks its release tool"):
            self.run_validation("accept")


if __name__ == "__main__":
    unittest.main()
