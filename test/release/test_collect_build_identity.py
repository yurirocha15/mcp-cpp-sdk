from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/release/collect_build_identity.py"
SPEC = importlib.util.spec_from_file_location("collect_build_identity", SCRIPT)
assert SPEC and SPEC.loader
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


class CollectBuildIdentityTest(unittest.TestCase):
    @staticmethod
    def builder_environment() -> dict[str, str]:
        return {
            "MCP_RELEASE_BUILDER_IMAGE": (
                f"ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders@sha256:{'1' * 64}"
            ),
            "MCP_RELEASE_BUILDER_IMAGE_ID": f"sha256:{'2' * 64}",
        }

    def test_os_release_parser_never_executes_shell_content(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "os-release"
            path.write_text(
                'ID=ubuntu\nVERSION_ID="24.04"\nVERSION_CODENAME=noble\n'
                'NAME="$(touch should-not-exist)"\n',
                encoding="utf-8",
            )
            values = collector.parse_os_release(path)
            self.assertEqual(values["NAME"], "$(touch should-not-exist)")
            self.assertFalse((Path(directory) / "should-not-exist").exists())

    def test_apt_collection_binds_commands_and_exact_release(self):
        outputs = {("dpkg", "--print-architecture"): "amd64", ("uname", "-m"): "x86_64"}
        with mock.patch.object(
            collector, "parse_os_release",
            return_value={"ID": "ubuntu", "VERSION_ID": "24.04", "VERSION_CODENAME": "noble"},
        ), mock.patch.object(collector, "command", side_effect=lambda *args: outputs[args]), mock.patch.dict(
            collector.os.environ, self.builder_environment(), clear=False
        ):
            result = collector.collect("apt", "ubuntu-noble-amd64")
        self.assertEqual(result["target_id"], "ubuntu-noble-amd64")
        self.assertEqual(result["schema_version"], 2)
        self.assertNotIn("hostname", result)

    def test_rpm_collection_rejects_a_runner_label_that_lies_about_el_version(self):
        outputs = {
            ("rpm", "--eval", "%{?fedora}"): "",
            ("rpm", "--eval", "%{?rhel}"): "9",
            ("rpm", "--eval", "%{?dist}"): ".el9",
            ("rpm", "--eval", "%{_arch}"): "x86_64",
            ("uname", "-m"): "x86_64",
        }
        with mock.patch.object(
            collector, "parse_os_release", return_value={"ID": "almalinux", "VERSION_ID": "9.7"},
        ), mock.patch.object(collector, "command", side_effect=lambda *args: outputs[args]):
            with self.assertRaises(Exception):
                collector.collect("rpm", "el-9-x86_64")


if __name__ == "__main__":
    unittest.main()
