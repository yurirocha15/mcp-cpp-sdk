from __future__ import annotations

import argparse
from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from release.build_identity import APT_TARGETS, validate_apt_build_identity
from release.aur_package_installer import validate_arguments as validate_aur_install_arguments
from release.model import ValidationError
from release.native_build import privileged_command
from release.native_builder import (
    _container_command,
    bind_build_matrices,
    bind_container_identity,
    load_builder_lock,
    run_locked_builder,
    validate_container_identity,
    verify_output_writable,
)
from release.native_builder_bootstrap import (
    _pushed_digest,
    bootstrap_tag,
    bootstrap_matrix,
    merge_evidence,
    select_platform_digest,
)


ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "release/native-builders/lock.json"
ONE = "1" * 64
TWO = "2" * 64


class NativeBuilderLockTests(unittest.TestCase):
    def resolved_document(self) -> dict[str, object]:
        document = json.loads(LOCK.read_text(encoding="utf-8"))
        for target in document["targets"]:
            target["base_digest"] = f"sha256:{ONE}"
            target["image_digest"] = f"sha256:{TWO}"
        return document

    def write(self, document: object, root: Path) -> Path:
        path = root / "lock.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_checked_in_bootstrap_lock_is_exact_but_release_use_fails_closed(self) -> None:
        targets = load_builder_lock(LOCK, require_resolved=False)
        self.assertEqual(len(targets), 20)
        self.assertEqual({target["runner"] for target in targets}, {"ubuntu-24.04", "ubuntu-24.04-arm"})
        with self.assertRaisesRegex(ValidationError, "two-stage builder bootstrap"):
            load_builder_lock(LOCK, require_resolved=True)

    def test_resolved_lock_binds_every_native_matrix_and_x86_aur(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            builders = load_builder_lock(
                self.write(self.resolved_document(), root), require_resolved=True
            )
            native = json.loads((ROOT / "packaging/native-targets.json").read_text(encoding="utf-8"))
            matrix, aur, conan = bind_build_matrices(native, builders)
        self.assertEqual(len(matrix), 18)
        self.assertEqual(aur["id"], "aur-x86_64")
        self.assertEqual(aur["runner"], "ubuntu-24.04")
        self.assertTrue(all("@sha256:" in target["builder_image"] for target in matrix))
        self.assertEqual(conan["id"], "conan-linux-x86_64")
        self.assertEqual(conan["runner"], "ubuntu-24.04")
        self.assertIn("@sha256:", conan["builder_image"])

    def test_conan_builder_is_exact_hash_locked_and_x86_only(self) -> None:
        builders = load_builder_lock(LOCK, require_resolved=False)
        builder = next(target for target in builders if target["id"] == "conan-linux-x86_64")
        self.assertEqual(builder["runner"], "ubuntu-24.04")
        self.assertEqual(builder["architecture"], "x86_64")
        requirements = (ROOT / "release/native-builders/conan-requirements.txt").read_text(
            encoding="utf-8"
        ).splitlines()
        self.assertEqual(len(requirements), 15)
        self.assertEqual(sum(line.startswith("conan==2.30.0 ") for line in requirements), 1)
        for line in requirements:
            self.assertRegex(line, r"^[A-Za-z0-9_-]+==[^ ]+ --hash=sha256:[0-9a-f]{64}$")

    def test_lock_rejects_every_security_boundary_mutation(self) -> None:
        mutations = {
            "order": lambda value: value["targets"].reverse(),
            "base": lambda value: value["targets"][0].update(base_image="ubuntu:latest"),
            "dockerfile": lambda value: value["targets"][0].update(
                dockerfile="release/native-builders/el.Dockerfile"
            ),
            "runner": lambda value: value["targets"][0].update(runner="self-hosted"),
            "repository": lambda value: value["targets"][0].update(image="ghcr.io/attacker/image"),
            "digest": lambda value: value["targets"][0].update(image_digest="sha256:short"),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, mutate in mutations.items():
                document = self.resolved_document()
                mutate(document)
                with self.subTest(name=name), self.assertRaises(ValidationError):
                    load_builder_lock(self.write(document, root), require_resolved=True)


class NativeBuilderIdentityTests(unittest.TestCase):
    def test_container_output_write_probe_creates_no_residue(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "files"
            verify_output_writable(output)
            self.assertEqual(list(output.iterdir()), [])

    def test_container_identity_binds_platform_reference_and_local_image_id(self) -> None:
        platform_identity = validate_apt_build_identity(
            "ubuntu-noble-amd64",
            APT_TARGETS["ubuntu-noble-amd64"].expected_facts(),
        )
        image = f"ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders@sha256:{ONE}"
        identity = bind_container_identity(
            platform_identity, image=image, image_id=f"sha256:{TWO}"
        )
        self.assertEqual(
            validate_container_identity(
                identity,
                expected_platform_identity=platform_identity,
                expected_image=image,
            ),
            identity,
        )
        for field, value in (("image", image.replace(ONE, TWO)), ("image_id", f"sha256:{ONE}")):
            mutated = deepcopy(identity)
            mutated["evidence"]["builder"][field] = value
            with self.subTest(field=field), self.assertRaises(ValidationError):
                validate_container_identity(
                    mutated,
                    expected_platform_identity=platform_identity,
                    expected_image=image,
                )

    def test_locked_runner_checks_host_and_uses_only_the_exact_image(self) -> None:
        builders = list(load_builder_lock(LOCK, require_resolved=False))
        builder = dict(builders[0])
        builder["base_digest"] = f"sha256:{ONE}"
        builder["image_digest"] = f"sha256:{TWO}"
        image = f"{builder['image']}@{builder['image_digest']}"
        args = argparse.Namespace(
            lock=LOCK,
            target_id=builder["id"],
            kind="apt",
            image=image,
        )
        with mock.patch("release.native_builder.load_builder_lock", return_value=(builder,)), mock.patch(
            "release.native_builder.platform.machine", return_value="x86_64"
        ), mock.patch("release.native_builder._inspect_image", return_value=f"sha256:{ONE}"), mock.patch(
            "release.native_builder._container_command", return_value=["docker", "run", image]
        ), mock.patch("release.native_builder.subprocess.run") as run:
            run_locked_builder(args)
        run.assert_called_once_with(["docker", "run", image], check=True)

    def test_conan_validator_has_dependency_network_but_no_credentials(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            args = argparse.Namespace(
                image=f"ghcr.io/yurirocha15/mcp-cpp-sdk-release-builders@sha256:{ONE}",
                output=root / "output",
                kind="conan",
                assets=assets,
                version="0.2.0",
                target_id="conan-linux-x86_64",
                architecture="x86_64",
                distribution=None,
                release=None,
                repository=None,
                source_date_epoch=None,
                tag=None,
            )
            command = _container_command(args, f"sha256:{TWO}")
        self.assertNotIn("--network", command)
        self.assertIn("type=bind", " ".join(command))
        self.assertIn("dst=/release-assets,readonly", " ".join(command))
        self.assertNotRegex(" ".join(command).lower(), r"token|secret|password|credential")
        self.assertIn("--kind", command)
        self.assertIn("conan", command)

    def test_root_containers_do_not_require_sudo(self) -> None:
        with mock.patch("release.native_build.os.geteuid", return_value=0):
            self.assertEqual(privileged_command("dnf", "install"), ["dnf", "install"])
        with mock.patch("release.native_build.os.geteuid", return_value=1001):
            self.assertEqual(
                privileged_command("apt-get", "install"),
                ["sudo", "apt-get", "install"],
            )

    def test_aur_privilege_wrapper_accepts_only_exact_split_packages(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shared = root / "mcp-cpp-sdk-0.2.0-1-x86_64.pkg.tar.zst"
            static = root / "mcp-cpp-sdk-static-0.2.0-1-x86_64.pkg.tar.zst"
            shared.write_bytes(b"shared")
            static.write_bytes(b"static")
            with mock.patch("release.aur_package_installer._DIRECTORY", root):
                self.assertEqual(
                    validate_aur_install_arguments((str(shared), str(static))),
                    (str(shared), str(static)),
                )
                with self.assertRaises(ValueError):
                    validate_aur_install_arguments((str(shared), str(root / "attacker.pkg.tar.zst")))
                other = root / "mcp-cpp-sdk-static-0.2.1-1-x86_64.pkg.tar.zst"
                other.write_bytes(b"other")
                with self.assertRaisesRegex(ValueError, "versions"):
                    validate_aur_install_arguments((str(shared), str(other)))


class WindowsConanLockTests(unittest.TestCase):
    def test_python_graph_is_exact_hash_locked_for_conan_2(self) -> None:
        requirements = (ROOT / "release/windows/conan-requirements.txt").read_text(
            encoding="utf-8"
        ).splitlines()
        self.assertEqual(len(requirements), 15)
        self.assertEqual(sum(line.startswith("conan==2.30.0 ") for line in requirements), 1)
        for line in requirements:
            self.assertRegex(line, r"^[A-Za-z0-9_-]+==[^ ]+ --hash=sha256:[0-9a-f]{64}$")

    def test_conan_dependency_lock_pins_every_recipe_revision(self) -> None:
        lock = json.loads((ROOT / "release/windows/conan.lock").read_text(encoding="utf-8"))
        self.assertEqual(set(lock), {"version", "requires", "build_requires", "python_requires", "config_requires"})
        self.assertEqual(lock["version"], "0.5")
        references = [*lock["requires"], *lock["build_requires"]]
        self.assertEqual(len(references), 9)
        for reference in references:
            self.assertRegex(reference, r"^[a-z0-9_+.-]+/[0-9][^#]*#[0-9a-f]{32}%[0-9]+\.[0-9]+$")


class NativeBuilderBootstrapTests(unittest.TestCase):
    def test_matrix_requires_exact_confirmation_and_stage_state(self) -> None:
        matrix = json.loads(
            bootstrap_matrix(
                lock=LOCK,
                stage="resolve-bases",
                confirmation="RESOLVE BASE DIGESTS",
            )
        )
        self.assertEqual(len(matrix["include"]), 20)
        self.assertEqual(
            {row["runner"] for row in matrix["include"]},
            {"ubuntu-24.04", "ubuntu-24.04-arm"},
        )
        with self.assertRaisesRegex(ValidationError, "confirmation"):
            bootstrap_matrix(lock=LOCK, stage="resolve-bases", confirmation="yes")

    def test_selects_platform_manifest_not_index_digest(self) -> None:
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
                        "platform": {"os": "linux", "architecture": "arm64", "variant": "v8"},
                    },
                ],
            }
        ).encode("utf-8")
        self.assertEqual(select_platform_digest(index, "x86_64"), f"sha256:{ONE}")
        self.assertEqual(select_platform_digest(index, "aarch64"), f"sha256:{TWO}")
        with self.assertRaisesRegex(ValidationError, "exactly one"):
            select_platform_digest(index.replace(b'"amd64"', b'"386"'), "x86_64")

    def test_push_output_requires_one_digest(self) -> None:
        output = f"tag: digest: sha256:{ONE} size: 123\n".encode("ascii")
        self.assertEqual(_pushed_digest(output), f"sha256:{ONE}")
        with self.assertRaises(ValidationError):
            _pushed_digest(output + f"other: digest: sha256:{TWO} size: 1\n".encode("ascii"))

    def test_staging_tags_are_target_specific(self) -> None:
        builders = load_builder_lock(LOCK, require_resolved=False)
        commit = "a" * 40
        tags = {bootstrap_tag(target, commit) for target in builders}
        self.assertEqual(len(tags), len(builders))
        self.assertTrue(all(tag.endswith(f"-{commit[:12]}") for tag in tags))

    def test_base_merge_requires_complete_exact_evidence(self) -> None:
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
                (evidence / f"{target['id']}.json").write_text(json.dumps(value), encoding="utf-8")
            output = root / "proposed-lock.json"
            merge_evidence(
                lock=LOCK, stage="base", evidence_directory=evidence, output=output
            )
            merged = load_builder_lock(output, require_resolved=False)
            self.assertTrue(all(target["base_digest"] == f"sha256:{ONE}" for target in merged))
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
