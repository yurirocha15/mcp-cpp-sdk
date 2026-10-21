from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from release.cloudsmith_publish import CloudsmithPublishError, preflight, publish_routes


ROOT = Path(__file__).resolve().parents[2]


class CloudsmithPublisherTest(unittest.TestCase):
    PUBLISH_USERNAME = "release-publisher"
    REPOSITORY_GPG_FINGERPRINT = "A" * 40

    def identity_response(self, username: str | None = None) -> str:
        return json.dumps(
            {
                "data": {
                    "is_authenticated": True,
                    "username": username or self.PUBLISH_USERNAME,
                }
            }
        )

    def route(self, asset: str) -> dict[str, str]:
        return {
            "asset": asset,
            "format": "apt",
            "route_id": "ubuntu-noble-amd64",
            "distribution": "ubuntu",
            "release": "noble",
            "target_architecture": "amd64",
            "package_name": "libmcp-cpp-sdk-dev",
            "package_version": "0.2.0-1",
            "package_architecture": "amd64",
            "build_tuple": "apt-ubuntu-noble-amd64",
            "identity_asset": "build-identity-ubuntu-noble-amd64.json",
        }

    def record(self, asset: str, digest: str, *, completed: bool = True, failed: bool = False):
        return {
            "filename": asset,
            "checksum_sha256": digest,
            "format": "deb",
            "distro": {"slug": "ubuntu"},
            "distro_version": {"slug": "noble"},
            "is_sync_completed": completed,
            "is_sync_failed": failed,
        }

    def rpm_route(self, asset: str) -> dict[str, str]:
        route = self.route(asset)
        route.update(
            format="rpm",
            route_id="fedora-43-x86_64",
            distribution="fedora",
            release="43",
            target_architecture="x86_64",
            package_name="mcp-cpp-sdk",
            package_version="0.2.0-1.fc43",
            package_architecture="x86_64",
            build_tuple="rpm-fedora-43-x86_64",
            identity_asset="build-identity-fedora-43-x86_64.json",
        )
        return route

    def rpm_record(self, asset: str, digest: str) -> dict[str, object]:
        record = self.record(asset, digest)
        record.update(
            format="rpm",
            distro={"slug": "fedora"},
            distro_version={"slug": "43"},
            cdn_url=(
                "https://dl.cloudsmith.io/public/example/mcp-cpp-sdk/"
                f"rpm/fedora/43/x86_64/{asset}"
            ),
        )
        return record

    def assert_rpm_readback(
        self,
        *,
        published_bytes: bytes = b"signed-rpm",
        published_identity: tuple[str, ...] | None = None,
        signature_key_id: str | None = None,
        record_digest: str | None = None,
    ) -> None:
        local_identity = (
            "mcp-cpp-sdk", "0", "0.2.0", "1.fc43", "x86_64", "1" * 64, "2" * 64,
        )
        published_identity = published_identity or local_identity
        signature_key_id = signature_key_id or self.REPOSITORY_GPG_FINGERPRINT[-8:]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = "fedora-43-x86_64--mcp-cpp-sdk-0.2.0-1.fc43.x86_64.rpm"
            local = root / asset
            local.write_bytes(b"unsigned-rpm")
            routes = root / "routes.json"
            routes.write_text(json.dumps([self.rpm_route(asset)]), encoding="utf-8")
            digest = record_digest or hashlib.sha256(published_bytes).hexdigest()

            def download(url, destination, maximum_bytes):
                content = b"key" if "/cfg/gpg/" in url else published_bytes
                self.assertLessEqual(len(content), maximum_bytes)
                destination.write_bytes(content)

            def run(arguments):
                if arguments in (["rpm", "--version"], ["rpmkeys", "--version"]):
                    return "RPM version 4.18.2\n"
                if arguments[0] == "gpg":
                    return (
                        "pub:::::::::\n"
                        f"fpr:::::::::{self.REPOSITORY_GPG_FINGERPRINT}:\n"
                    )
                if arguments[0] == "cloudsmith":
                    if arguments[1] == "whoami":
                        return self.identity_response()
                    return json.dumps({"data": [self.rpm_record(asset, digest)]})
                if arguments[0] == "rpmkeys" and "--checksig" in arguments:
                    return f"Header V4 RSA/SHA256 Signature, key ID {signature_key_id}: OK\n"
                if arguments[0] == "rpm" and "-qp" in arguments:
                    identity = local_identity if Path(arguments[-1]) == local else published_identity
                    return "\t".join(identity) + "\n"
                return ""

            self.assertEqual(
                publish_routes(
                    root,
                    routes,
                    package_format="rpm",
                    namespace="example",
                    repository="mcp-cpp-sdk",
                    expected_username=self.PUBLISH_USERNAME,
                    repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                    run=run,
                    download=download,
                ),
                "SKIPPED_ALREADY_IDENTICAL",
            )

    def test_rpm_readback_accepts_only_matching_signed_output(self):
        self.assert_rpm_readback()

    def test_rpm_readback_rejects_changed_header_or_payload(self):
        identity = [
            "mcp-cpp-sdk", "0", "0.2.0", "1.fc43", "x86_64", "1" * 64, "2" * 64,
        ]
        for index in (5, 6):
            changed = list(identity)
            changed[index] = "3" * 64
            with self.subTest(field=index), self.assertRaises(CloudsmithPublishError):
                self.assert_rpm_readback(published_identity=tuple(changed))

    def test_rpm_readback_rejects_wrong_repository_signature(self):
        with self.assertRaises(CloudsmithPublishError):
            self.assert_rpm_readback(signature_key_id="B" * 8)

    def test_rpm_readback_rejects_download_checksum_mismatch(self):
        with self.assertRaises(CloudsmithPublishError):
            self.assert_rpm_readback(record_digest="0" * 64)

    def test_actual_signed_route_schema_reaches_read_and_push(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = "ubuntu-noble-amd64--libmcp-cpp-sdk-dev_0.2.0-1_amd64.deb"
            (root / asset).write_bytes(b"deb")
            route_file = root / "routes.json"
            route_file.write_text(json.dumps([self.route(asset)]), encoding="utf-8")
            calls = []
            digest = hashlib.sha256(b"deb").hexdigest()
            listings = [
                {"data": []},
                {"data": [self.record(asset, digest, completed=False)]},
                {"data": [self.record(asset, digest)]},
            ]
            def run(arguments):
                calls.append(arguments)
                if arguments[1] == "whoami":
                    return self.identity_response()
                if arguments[1:3] == ["list", "packages"]:
                    return json.dumps(listings.pop(0))
                return "upload queued"
            result = publish_routes(
                root, route_file, package_format="apt", namespace="example",
                repository="mcp-cpp-sdk", expected_username=self.PUBLISH_USERNAME,
                repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                run=run, sleep=lambda _: None,
            )
        self.assertEqual(result, "PUBLISHED")
        self.assertEqual(calls[0], ["cloudsmith", "whoami", "-F", "json"])
        pushes = [call for call in calls if call[1] == "push"]
        self.assertEqual(pushes[0][:4], ["cloudsmith", "push", "deb", "example/mcp-cpp-sdk/ubuntu/noble"])
        self.assertEqual(listings, [])

    def test_identical_remote_digest_is_skipped_and_conflict_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = "ubuntu-noble-amd64--libmcp-cpp-sdk-dev_0.2.0-1_amd64.deb"
            content = b"deb"
            (root / asset).write_bytes(content)
            route_file = root / "routes.json"
            route_file.write_text(json.dumps([self.route(asset)]), encoding="utf-8")
            def response(digest):
                def run(arguments):
                    if arguments[1] == "whoami":
                        return self.identity_response()
                    return json.dumps({"data": [self.record(asset, digest)]})
                return run
            self.assertEqual(
                publish_routes(
                    root, route_file, package_format="apt", namespace="example",
                    repository="mcp-cpp-sdk",
                    expected_username=self.PUBLISH_USERNAME,
                    repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                    run=response(hashlib.sha256(content).hexdigest()),
                ),
                "SKIPPED_ALREADY_IDENTICAL",
            )
            with self.assertRaises(CloudsmithPublishError):
                publish_routes(
                    root, route_file, package_format="apt", namespace="example",
                    repository="mcp-cpp-sdk",
                    expected_username=self.PUBLISH_USERNAME,
                    repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                    run=response("0" * 64),
                )

    def test_nonterminal_or_failed_upload_never_reports_published(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = "ubuntu-noble-amd64--libmcp-cpp-sdk-dev_0.2.0-1_amd64.deb"
            content = b"deb"
            digest = hashlib.sha256(content).hexdigest()
            (root / asset).write_bytes(content)
            route_file = root / "routes.json"
            route_file.write_text(json.dumps([self.route(asset)]), encoding="utf-8")

            for label, terminal in (
                ("timeout", self.record(asset, digest, completed=False)),
                ("failed", self.record(asset, digest, completed=False, failed=True)),
            ):
                calls = 0
                def run(arguments):
                    nonlocal calls
                    if arguments[1] == "whoami":
                        return self.identity_response()
                    if arguments[1:3] == ["list", "packages"]:
                        calls += 1
                        if calls == 1:
                            return '{"data":[]}'
                        return json.dumps({"data": [terminal]})
                    return "upload queued"
                with self.subTest(label=label), self.assertRaises(CloudsmithPublishError):
                    publish_routes(
                        root, route_file, package_format="apt", namespace="example",
                        repository="mcp-cpp-sdk",
                        expected_username=self.PUBLISH_USERNAME,
                        repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                        run=run, sleep=lambda _: None,
                        poll_attempts=2,
                    )

    def test_route_readback_requires_exact_format_distribution_and_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = "ubuntu-noble-amd64--libmcp-cpp-sdk-dev_0.2.0-1_amd64.deb"
            content = b"deb"
            digest = hashlib.sha256(content).hexdigest()
            (root / asset).write_bytes(content)
            route_file = root / "routes.json"
            route_file.write_text(json.dumps([self.route(asset)]), encoding="utf-8")
            for field, replacement in (
                ("format", "rpm"),
                ("distro", {"slug": "debian"}),
                ("distro_version", {"slug": "jammy"}),
            ):
                record = self.record(asset, digest)
                record[field] = replacement
                with self.subTest(field=field), self.assertRaises(CloudsmithPublishError):
                    def run(arguments):
                        if arguments[1] == "whoami":
                            return self.identity_response()
                        return json.dumps({"data": [record]})

                    publish_routes(
                        root, route_file, package_format="apt", namespace="example",
                        repository="mcp-cpp-sdk",
                        expected_username=self.PUBLISH_USERNAME,
                        repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                        run=run,
                    )

    def test_publish_rejects_wrong_account_identity_before_provider_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            asset = "ubuntu-noble-amd64--libmcp-cpp-sdk-dev_0.2.0-1_amd64.deb"
            (root / asset).write_bytes(b"deb")
            route_file = root / "routes.json"
            route_file.write_text(json.dumps([self.route(asset)]), encoding="utf-8")
            calls = []

            def run(arguments):
                calls.append(arguments)
                return self.identity_response("unexpected-publisher")

            with self.assertRaisesRegex(
                CloudsmithPublishError, "account identity differs"
            ):
                publish_routes(
                    root,
                    route_file,
                    package_format="apt",
                    namespace="example",
                    repository="mcp-cpp-sdk",
                    expected_username=self.PUBLISH_USERNAME,
                    repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                    run=run,
                )
        self.assertEqual(calls, [["cloudsmith", "whoami", "-F", "json"]])

    def preflight_response(self, arguments, *, omit_noble=False):
        if arguments[0] == "gpg":
            return f"pub:::::::::\nfpr:::::::::{self.REPOSITORY_GPG_FINGERPRINT}:\n"
        if arguments[1] == "whoami":
            data = {"is_authenticated": True, "username": "release-validator"}
        elif arguments[1:3] == ["list", "repos"]:
            data = [
                {
                    "namespace": "example",
                    "slug": "mcp-cpp-sdk",
                    "repository_type_str": "Open-Source",
                }
            ]
        elif arguments[1:4] == ["list", "distros", "deb"]:
            ubuntu = ["jammy", "resolute"] if omit_noble else ["jammy", "noble", "resolute"]
            data = [
                {"slug": "ubuntu", "versions": [{"slug": item} for item in ubuntu]},
                {"slug": "debian", "versions": [{"slug": "bookworm"}, {"slug": "trixie"}]},
            ]
        elif arguments[1:4] == ["list", "distros", "rpm"]:
            data = [
                {"slug": "fedora", "versions": [{"slug": "43"}, {"slug": "44"}]},
                {"slug": "el", "versions": [{"slug": "9"}, {"slug": "10"}]},
            ]
        else:
            raise AssertionError(arguments)
        return json.dumps({"data": data})

    def download_key(self, url, destination, maximum_bytes):
        self.assertIn(self.REPOSITORY_GPG_FINGERPRINT[-16:], url)
        self.assertGreaterEqual(maximum_bytes, 3)
        destination.write_bytes(b"key")

    def test_read_only_preflight_binds_identity_repository_and_routes(self):
        preflight(
            "example",
            "mcp-cpp-sdk",
            expected_username="release-validator",
            repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
            formats=("apt", "rpm"),
            native_targets=ROOT / "packaging/targets.json",
            run=self.preflight_response,
            download=self.download_key,
        )

    def test_read_only_preflight_rejects_missing_route_or_identity(self):
        with self.assertRaisesRegex(CloudsmithPublishError, "ubuntu/noble"):
            preflight(
                "example",
                "mcp-cpp-sdk",
                expected_username="release-validator",
                repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                formats=("apt", "rpm"),
                native_targets=ROOT / "packaging/targets.json",
                run=lambda arguments: self.preflight_response(arguments, omit_noble=True),
                download=self.download_key,
            )
        with self.assertRaises(CloudsmithPublishError):
            preflight(
                "example",
                "mcp-cpp-sdk",
                expected_username="different-validator",
                repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                formats=("apt", "rpm"),
                native_targets=ROOT / "packaging/targets.json",
                run=self.preflight_response,
                download=self.download_key,
            )

    def test_preflight_checks_only_selected_format_routes(self):
        calls = []

        def apt_only(arguments):
            calls.append(arguments)
            if arguments[1:4] == ["list", "distros", "rpm"]:
                raise AssertionError("unselected RPM route was queried")
            return self.preflight_response(arguments)

        preflight(
            "example",
            "mcp-cpp-sdk",
            expected_username="release-validator",
            repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
            formats=("apt",),
            native_targets=ROOT / "packaging/targets.json",
            run=apt_only,
        )
        self.assertNotIn(
            ["cloudsmith", "list", "distros", "rpm", "-F", "json"], calls
        )
        with self.assertRaisesRegex(CloudsmithPublishError, "selected Cloudsmith formats"):
            preflight(
                "example",
                "mcp-cpp-sdk",
                expected_username="release-validator",
                repository_gpg_fingerprint=self.REPOSITORY_GPG_FINGERPRINT,
                formats=(),
                native_targets=ROOT / "packaging/targets.json",
                run=apt_only,
            )


if __name__ == "__main__":
    unittest.main()
