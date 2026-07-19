from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from release.abi_baseline import select_baseline
from release.abi_build import (
    ABI_BUILD_TUPLE,
    ABI_IDENTITY_NAME,
    ABI_RECIPE,
    EXPECTED_COMPONENT_ARTIFACTS,
    EXPECTED_COMPONENTS,
    EXPECTED_PLATFORM,
    AbiBuildError,
    container_build_command,
    container_compare_command,
    environment_sha256,
    require_compatible_environments,
    validate_build_identity,
    validate_candidate_pair,
    validate_corpus,
)
from release.abi_policy import (
    AbiReleasePolicyError,
    authorize_abidiff,
    authorize_first_baseline,
    baseline_download_plan,
    compare_corpora,
    download_planned_assets,
    load_policy,
    normalize_release_catalog,
    verify_baseline_bundle,
)


PRIMARY = "A" * 40
TAG_SUBKEY = "B" * 40
ARTIFACT_SUBKEY = "C" * 40
SIGNERS = {
    "primary_fingerprint": PRIMARY,
    "tag_subkey_fingerprint": TAG_SUBKEY,
    "artifact_subkey_fingerprint": ARTIFACT_SUBKEY,
}
BUILDER_IMAGE = "ghcr.io/example/mcp-cpp-sdk-abi-builder@sha256:" + "d" * 64


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")


def release(tag: str, database_id: int, *, immutable: bool = True) -> dict[str, object]:
    return {
        "databaseId": database_id,
        "isDraft": False,
        "isImmutable": immutable,
        "isPrerelease": False,
        "tagName": tag,
    }


def identity(tag: str, commit: str, corpus_sha256: str) -> dict[str, object]:
    components = []
    for index, name in enumerate(EXPECTED_COMPONENTS, 1):
        fixed_versions = {"gcc": "13.3.0", "libabigail": "2.4"}
        version = fixed_versions.get(name, f"1.{index}.0")
        target = "headers" if name in {"boost", "nlohmann_json"} else "x86_64-linux-gnu"
        components.append(
            {
                "name": name,
                "upstream_version": version,
                "target": target,
                "packages": [
                    {
                        "name": f"{name.replace('_', '-')}-dev",
                        "version": f"{version}-1ubuntu1",
                        "architecture": "amd64",
                        "content_sha256": f"{index:x}" * 64,
                    }
                ],
                "artifacts": [
                    {"name": artifact, "sha256": f"{index + offset + 7:x}"[-1] * 64}
                    for offset, artifact in enumerate(EXPECTED_COMPONENT_ARTIFACTS[name])
                ],
            }
        )
    version = tag.removeprefix("v")
    major, _minor, _patch = version.split(".")
    loader_identity = version if major == "0" else major
    value: dict[str, object] = {
        "schema_version": 1,
        "kind": "abi-build-identity",
        "build_tuple": ABI_BUILD_TUPLE,
        "builder_image": BUILDER_IMAGE,
        "source": {"tag": tag, "commit": commit},
        "platform": dict(EXPECTED_PLATFORM),
        "components": components,
        "recipe": copy.deepcopy(ABI_RECIPE),
        "environment_sha256": "",
        "outputs": {
            "library_name": f"libmcp-cpp-sdk.so.{loader_identity}",
            "library_sha256": "a" * 64,
            "corpus_name": f"mcp-cpp-sdk-{version}-{ABI_BUILD_TUPLE}.abi.xml",
            "corpus_sha256": corpus_sha256,
            "needed": ["libc.so.6", "libcrypto.so.3", "libgcc_s.so.1", "libstdc++.so.6"],
        },
    }
    value["environment_sha256"] = environment_sha256(value)
    return value


def policy(*, exceptions: list[dict[str, object]] | None = None) -> dict[str, object]:
    return {
        "schema_version": 1,
        "build_tuple": ABI_BUILD_TUPLE,
        "initial_baselines": [
            {
                "abi_line": "0.2",
                "tag": "v0.2.0",
                "rationale": "Record exact stable 0.2.0 ABI evidence before any provider publication.",
            }
        ],
        "exceptions": exceptions or [],
    }


class AbiBuildIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.baseline = identity("v1.2.1", "1" * 40, "a" * 64)
        self.candidate = identity("v1.2.2", "2" * 40, "b" * 64)

    def test_exact_compiler_runtime_tools_and_dependencies_are_bound(self) -> None:
        normalized = validate_build_identity(self.baseline)
        self.assertEqual(normalized["build_tuple"], ABI_BUILD_TUPLE)
        self.assertEqual(
            [component["name"] for component in normalized["components"]],
            list(EXPECTED_COMPONENTS),
        )
        self.assertEqual(normalized["environment_sha256"], environment_sha256(normalized))
        require_compatible_environments(self.baseline, self.candidate)

    def test_zero_major_patch_keeps_exact_loader_identity_while_remaining_comparable(self) -> None:
        baseline = identity("v0.2.0", "1" * 40, "a" * 64)
        candidate = identity("v0.2.1", "2" * 40, "b" * 64)
        self.assertEqual(candidate["outputs"]["library_name"], "libmcp-cpp-sdk.so.0.2.1")
        require_compatible_environments(baseline, candidate)

    def test_dependency_or_tool_update_is_not_treated_as_equivalent(self) -> None:
        changed = copy.deepcopy(self.candidate)
        changed["components"][0]["packages"][0]["version"] = "99.0-1"
        changed["environment_sha256"] = environment_sha256(changed)
        with self.assertRaisesRegex(AbiBuildError, "different compiler"):
            require_compatible_environments(self.baseline, changed)

    def test_builder_image_is_digest_pinned_and_part_of_environment_identity(self) -> None:
        changed = copy.deepcopy(self.candidate)
        changed["builder_image"] = (
            "ghcr.io/example/mcp-cpp-sdk-abi-builder@sha256:" + "e" * 64
        )
        changed["environment_sha256"] = environment_sha256(changed)
        with self.assertRaisesRegex(AbiBuildError, "different compiler"):
            require_compatible_environments(self.baseline, changed)
        changed["builder_image"] = "ghcr.io/example/mcp-cpp-sdk-abi-builder:latest"
        with self.assertRaisesRegex(AbiBuildError, "immutable OCI digest"):
            validate_build_identity(changed)

    def test_container_build_is_offline_locked_down_and_passes_no_token(self) -> None:
        command = container_build_command(
            image=BUILDER_IMAGE,
            source=Path("/checkout"),
            io_directory=Path("/io"),
            tag="v1.2.2",
            commit="2" * 40,
            source_date_epoch=1_700_000_000,
        )
        for expected in (
            "--platform=linux/amd64",
            "--network=none",
            "--read-only",
            "--cap-drop=ALL",
            "--security-opt=no-new-privileges",
        ):
            self.assertIn(expected, command)
        rendered = " ".join(command)
        self.assertNotIn("GITHUB_TOKEN", rendered)
        self.assertNotIn("ACTIONS_ID_TOKEN", rendered)
        self.assertEqual(command.count(BUILDER_IMAGE), 2)

    def test_container_compare_uses_the_same_locked_builder_without_credentials(self) -> None:
        selection = select_baseline("v1.2.2", [release("v1.2.1", 7)])
        command = container_compare_command(
            image=BUILDER_IMAGE,
            source=Path("/checkout"),
            io_directory=Path("/io"),
            selection=selection,
        )
        for expected in (
            "--network=none",
            "--read-only",
            "--cap-drop=ALL",
            "--security-opt=no-new-privileges",
            "release.abi_policy",
            "compare",
            "/source/release/abi-policy.json",
        ):
            self.assertIn(expected, command)
        rendered = " ".join(command)
        self.assertNotIn("GITHUB_TOKEN", rendered)
        self.assertNotIn("ACTIONS_ID_TOKEN", rendered)

    def test_builder_definition_pins_base_and_ubuntu_snapshot(self) -> None:
        root = Path(__file__).resolve().parents[2]
        dockerfile = (root / "release/abi-builder/Dockerfile").read_text(encoding="utf-8")
        sources = (root / "release/abi-builder/ubuntu.sources").read_text(encoding="ascii")
        self.assertRegex(dockerfile.splitlines()[1], r"^FROM ubuntu@sha256:[0-9a-f]{64}$")
        self.assertNotIn("apt-get upgrade", dockerfile)
        self.assertNotIn("AllowUnauthenticated", dockerfile)
        self.assertNotIn("trusted=yes", dockerfile)
        self.assertLess(
            dockerfile.index("rm /etc/apt/apt.conf.d/99snapshot-ca-bootstrap"),
            dockerfile.rindex("apt-get update"),
        )
        self.assertIn("Snapshot: 20260710T000000Z", sources)

    def test_identity_fails_closed_on_platform_gcc_abigail_and_output_mutations(self) -> None:
        mutations = []
        platform = copy.deepcopy(self.baseline)
        platform["platform"]["os_version_id"] = "24.10"
        platform["environment_sha256"] = environment_sha256(platform)
        mutations.append(platform)
        gcc = copy.deepcopy(self.baseline)
        gcc["components"][2]["upstream_version"] = "14.1.0"
        gcc["environment_sha256"] = environment_sha256(gcc)
        mutations.append(gcc)
        abigail = copy.deepcopy(self.baseline)
        abigail["components"][3]["upstream_version"] = "2.5"
        abigail["environment_sha256"] = environment_sha256(abigail)
        mutations.append(abigail)
        soname = copy.deepcopy(self.baseline)
        soname["outputs"]["library_name"] = "libmcp-cpp-sdk.so.0"
        mutations.append(soname)
        for mutated in mutations:
            with self.subTest(mutated=mutated), self.assertRaises(AbiBuildError):
                validate_build_identity(mutated)

    def test_corpus_must_be_xml_and_must_not_embed_host_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus = root / "corpus.xml"
            corpus.write_text("<abi-corpus version='2.4'/>", encoding="utf-8")
            validate_corpus(corpus, forbidden_paths=(root / "source", root / "build"))
            corpus.write_text(f"<abi-corpus path='{root}/source'/>", encoding="utf-8")
            with self.assertRaisesRegex(AbiBuildError, "host-specific"):
                validate_corpus(corpus, forbidden_paths=(root / "source",))
            corpus.write_text("not xml", encoding="utf-8")
            with self.assertRaisesRegex(AbiBuildError, "well-formed"):
                validate_corpus(corpus, forbidden_paths=())

    def test_candidate_pair_binds_canonical_identity_source_and_corpus(self) -> None:
        corpus_bytes = b"<abi-corpus version='2.4'/>\n"
        value = identity("v1.2.2", "2" * 40, digest(corpus_bytes))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            corpus = directory / value["outputs"]["corpus_name"]
            identity_path = directory / ABI_IDENTITY_NAME
            corpus.write_bytes(corpus_bytes)
            identity_path.write_bytes(canonical(value))
            validate_candidate_pair(
                identity_path,
                corpus,
                tag="v1.2.2",
                commit="2" * 40,
            )
            corpus.write_bytes(corpus_bytes + b"tampered")
            with self.assertRaisesRegex(AbiBuildError, "corpus digest"):
                validate_candidate_pair(
                    identity_path,
                    corpus,
                    tag="v1.2.2",
                    commit="2" * 40,
                )


class AbiPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.selection = select_baseline("v1.2.2", [release("v1.2.1", 7)])
        self.baseline = canonical(identity("v1.2.1", "1" * 40, "a" * 64))
        self.candidate = canonical(identity("v1.2.2", "2" * 40, "b" * 64))
        self.report = b"public ABI change report\n"

    def result(self, exit_code: int) -> dict[str, object]:
        return {
            "schema_version": 1,
            "tool": "abidiff",
            "tool_version": "2.4",
            "baseline_tag": "v1.2.1",
            "candidate_tag": "v1.2.2",
            "abi_line": "1",
            "baseline_sha256": "a" * 64,
            "candidate_sha256": "b" * 64,
            "report_sha256": digest(self.report),
            "exit_code": exit_code,
        }

    def test_first_comparison_series_requires_exact_checked_in_authorization(self) -> None:
        first = select_baseline("v0.2.0", [])
        authorization = authorize_first_baseline(first, policy())
        self.assertEqual(authorization["tag"], "v0.2.0")
        with self.assertRaisesRegex(AbiReleasePolicyError, "not explicitly"):
            authorize_first_baseline(select_baseline("v0.3.0", []), policy())
        patch_release = select_baseline("v0.2.1", [release("v0.2.0", 7)])
        self.assertEqual(patch_release.baseline.version.tag, "v0.2.0")
        with self.assertRaisesRegex(AbiReleasePolicyError, "existing comparison series"):
            authorize_first_baseline(patch_release, policy())

    def test_paginated_rest_release_catalog_is_normalized_without_trusting_extra_fields(self) -> None:
        pages = [
            [
                {
                    "id": 7,
                    "draft": False,
                    "immutable": True,
                    "prerelease": False,
                    "tag_name": "v1.2.1",
                    "browser_download_url": "https://untrusted.example.invalid/ignored",
                }
            ]
        ]
        normalized = normalize_release_catalog(pages)
        self.assertEqual(select_baseline("v1.2.2", normalized).baseline.database_id, 7)
        del pages[0][0]["immutable"]
        with self.assertRaisesRegex(AbiReleasePolicyError, "incomplete"):
            normalize_release_catalog(pages)

    def test_select_cli_emits_safe_github_outputs_for_later_steps(self) -> None:
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            catalog = directory / "catalog.json"
            policy_path = directory / "policy.json"
            selection = directory / "selection.json"
            decision = directory / "decision.json"
            github_output = directory / "github-output"
            catalog.write_text(
                json.dumps(
                    [[{"id": 7, "draft": False, "immutable": True, "prerelease": False, "tag_name": "v1.2.1"}]]
                ),
                encoding="utf-8",
            )
            policy_path.write_text(json.dumps(policy()), encoding="utf-8")
            completed = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "-S",
                    str(root / "scripts/run_release_tool.py"),
                    "release.abi_policy",
                    "select",
                    "--tag",
                    "v1.2.2",
                    "--catalog",
                    str(catalog),
                    "--policy",
                    str(policy_path),
                    "--selection",
                    str(selection),
                    "--decision",
                    str(decision),
                    "--github-output",
                    str(github_output),
                ],
                cwd=root,
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                github_output.read_text(encoding="ascii"),
                "has_baseline=true\nabi_line=1\nbaseline_tag=v1.2.1\nbaseline_release_id=7\n",
            )

    def test_policy_is_canonical_and_rejects_inconsistent_exceptions(self) -> None:
        self.assertEqual(load_policy(policy())["build_tuple"], ABI_BUILD_TUPLE)
        checked_in = Path(__file__).resolve().parents[2] / "release/abi-policy.json"
        checked_in_value = json.loads(checked_in.read_text(encoding="ascii"))
        self.assertEqual(canonical(load_policy(checked_in_value)), checked_in.read_bytes())
        invalid = policy(
            exceptions=[
                {
                    **self.exception(),
                    "exit_code": 8,
                }
            ]
        )
        with self.assertRaisesRegex(AbiReleasePolicyError, "status 4"):
            load_policy(invalid)

    def exception(self) -> dict[str, object]:
        return {
            "id": "reviewed-public-type-change",
            "baseline_tag": "v1.2.1",
            "candidate_tag": "v1.2.2",
            "abi_line": "1",
            "baseline_corpus_sha256": "a" * 64,
            "candidate_corpus_sha256": "b" * 64,
            "baseline_build_identity_sha256": digest(self.baseline),
            "candidate_build_identity_sha256": digest(self.candidate),
            "report_sha256": digest(self.report),
            "abidiff_version": "2.4",
            "exit_code": 4,
            "rationale": "The public type addition was reviewed and preserves existing binary contracts.",
        }

    def test_no_change_is_accepted_without_an_exception(self) -> None:
        self.assertIsNone(
            authorize_abidiff(
                self.result(0),
                self.selection,
                baseline_identity_bytes=self.baseline,
                candidate_identity_bytes=self.candidate,
                report_bytes=self.report,
                policy=policy(),
            )
        )

    def test_status_four_is_never_autoaccepted(self) -> None:
        with self.assertRaisesRegex(AbiReleasePolicyError, "exact checked-in"):
            authorize_abidiff(
                self.result(4),
                self.selection,
                baseline_identity_bytes=self.baseline,
                candidate_identity_bytes=self.candidate,
                report_bytes=self.report,
                policy=policy(),
            )
        self.assertEqual(
            authorize_abidiff(
                self.result(4),
                self.selection,
                baseline_identity_bytes=self.baseline,
                candidate_identity_bytes=self.candidate,
                report_bytes=self.report,
                policy=policy(exceptions=[self.exception()]),
            ),
            "reviewed-public-type-change",
        )

    def test_incompatible_zero_major_patch_requires_exact_reviewed_exception(self) -> None:
        selection = select_baseline("v0.2.1", [release("v0.2.0", 7)])
        baseline = canonical(identity("v0.2.0", "1" * 40, "a" * 64))
        candidate = canonical(identity("v0.2.1", "2" * 40, "b" * 64))
        result = {
            "schema_version": 1,
            "tool": "abidiff",
            "tool_version": "2.4",
            "baseline_tag": "v0.2.0",
            "candidate_tag": "v0.2.1",
            "abi_line": "0.2",
            "baseline_sha256": "a" * 64,
            "candidate_sha256": "b" * 64,
            "report_sha256": digest(self.report),
            "exit_code": 12,
        }
        exception = {
            "id": "reviewed-zero-major-break",
            "baseline_tag": "v0.2.0",
            "candidate_tag": "v0.2.1",
            "abi_line": "0.2",
            "baseline_corpus_sha256": "a" * 64,
            "candidate_corpus_sha256": "b" * 64,
            "baseline_build_identity_sha256": digest(baseline),
            "candidate_build_identity_sha256": digest(candidate),
            "report_sha256": digest(self.report),
            "abidiff_version": "2.4",
            "exit_code": 12,
            "rationale": (
                "This pre-1.0 break was reviewed and is isolated by the exact loader identity."
            ),
        }
        with self.assertRaisesRegex(AbiReleasePolicyError, "exact checked-in"):
            authorize_abidiff(
                result,
                selection,
                baseline_identity_bytes=baseline,
                candidate_identity_bytes=candidate,
                report_bytes=self.report,
                policy=policy(),
            )
        self.assertEqual(
            authorize_abidiff(
                result,
                selection,
                baseline_identity_bytes=baseline,
                candidate_identity_bytes=candidate,
                report_bytes=self.report,
                policy=policy(exceptions=[exception]),
            ),
            "reviewed-zero-major-break",
        )

    def test_exception_is_bound_to_every_evidence_digest(self) -> None:
        fields = (
            "baseline_corpus_sha256",
            "candidate_corpus_sha256",
            "baseline_build_identity_sha256",
            "candidate_build_identity_sha256",
            "report_sha256",
        )
        for field in fields:
            exception = self.exception()
            exception[field] = "f" * 64
            with self.subTest(field=field), self.assertRaisesRegex(
                AbiReleasePolicyError, "exact checked-in"
            ):
                authorize_abidiff(
                    self.result(4),
                    self.selection,
                    baseline_identity_bytes=self.baseline,
                    candidate_identity_bytes=self.candidate,
                    report_bytes=self.report,
                    policy=policy(exceptions=[exception]),
                )

    def test_incompatible_and_tool_error_statuses_can_never_be_excepted(self) -> None:
        for status in (1, 2, 8, 12):
            with self.subTest(status=status), self.assertRaises(AbiReleasePolicyError):
                authorize_abidiff(
                    self.result(status),
                    self.selection,
                    baseline_identity_bytes=self.baseline,
                    candidate_identity_bytes=self.candidate,
                    report_bytes=self.report,
                    policy=policy(exceptions=[self.exception()]),
                )

    def test_compare_invokes_only_the_conservative_bound_abidiff_command(self) -> None:
        baseline_corpus_bytes = b"<abi-corpus version='2.4' name='baseline'/>\n"
        candidate_corpus_bytes = b"<abi-corpus version='2.4' name='candidate'/>\n"
        baseline_value = identity("v1.2.1", "1" * 40, digest(baseline_corpus_bytes))
        candidate_value = identity("v1.2.2", "2" * 40, digest(candidate_corpus_bytes))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = directory / "abidiff"
            executable.write_bytes(b"reviewed abidiff executable")
            executable_digest = digest(executable.read_bytes())
            for value in (baseline_value, candidate_value):
                component = next(item for item in value["components"] if item["name"] == "libabigail")
                component["artifacts"] = [
                    {"name": "abidiff", "sha256": executable_digest},
                    {"name": "abidw", "sha256": "c" * 64},
                ]
                value["environment_sha256"] = environment_sha256(value)
            baseline_corpus = directory / "baseline.xml"
            candidate_corpus = directory / "candidate.xml"
            baseline_identity = directory / "baseline.json"
            candidate_identity = directory / "candidate.json"
            report = directory / "report.txt"
            result = directory / "result.json"
            baseline_corpus.write_bytes(baseline_corpus_bytes)
            candidate_corpus.write_bytes(candidate_corpus_bytes)
            baseline_identity.write_bytes(canonical(baseline_value))
            candidate_identity.write_bytes(canonical(candidate_value))
            version_process = mock.Mock(returncode=0, stdout="abidiff: 2.4\n")
            diff_process = mock.Mock(returncode=0, stdout=b"")
            with mock.patch("release.abi_policy.shutil.which", return_value=str(executable)), mock.patch(
                "release.abi_policy.subprocess.run", side_effect=(version_process, diff_process)
            ) as run:
                self.assertIsNone(
                    compare_corpora(
                        self.selection,
                        baseline_corpus=baseline_corpus,
                        candidate_corpus=candidate_corpus,
                        baseline_identity=baseline_identity,
                        candidate_identity=candidate_identity,
                        report=report,
                        result_path=result,
                        policy=policy(),
                    )
                )
            self.assertEqual(
                run.call_args_list[1].args[0],
                [
                    str(executable.resolve()),
                    "--no-default-suppression",
                    "baseline.abi.xml",
                    "candidate.abi.xml",
                ],
            )
            self.assertEqual(run.call_args_list[1].kwargs["env"]["LC_ALL"], "C")
            self.assertEqual(json.loads(result.read_text())["exit_code"], 0)


class BaselineBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.selection = select_baseline("v1.2.2", [[release("v1.2.1", 7)]])
        self.corpus_name = f"mcp-cpp-sdk-1.2.1-{ABI_BUILD_TUPLE}.abi.xml"
        self.corpus = b"<abi-corpus version='2.4'/>\n"
        self.identity = canonical(identity("v1.2.1", "1" * 40, digest(self.corpus)))
        self.source = b"canonical source\n"
        self.key = b"public key\n"
        self.records = sorted(
            [
                {
                    "name": self.corpus_name,
                    "size": len(self.corpus),
                    "sha256": digest(self.corpus),
                    "role": "abi-corpus",
                    "build_tuple": ABI_BUILD_TUPLE,
                },
                {
                    "name": ABI_IDENTITY_NAME,
                    "size": len(self.identity),
                    "sha256": digest(self.identity),
                    "role": "abi-build-identity",
                    "build_tuple": ABI_BUILD_TUPLE,
                },
                {
                    "name": "mcp-cpp-sdk-1.2.1.tar.gz",
                    "size": len(self.source),
                    "sha256": digest(self.source),
                    "role": "source-or-binary-archive",
                },
                {
                    "name": "release-signing-key.asc",
                    "size": len(self.key),
                    "sha256": digest(self.key),
                    "role": "publisher-input",
                },
            ],
            key=lambda item: item["name"],
        )
        manifest = {
            "schema_version": 2,
            "package": "mcp-cpp-sdk",
            "version": "1.2.1",
            "tag": "v1.2.1",
            "commit": "1" * 40,
            "source_tree_sha256": "2" * 64,
            "release_ledger": {"issue_id": "1", "issue_url": "https://example.invalid/1"},
            "signers": SIGNERS,
            "channel_capabilities": [
                "github",
                "conan2",
                "apt",
                "rpm",
                "aur",
                "homebrew",
                "chocolatey",
            ],
            "payloads": self.records,
            "dependency_closure": [],
            "conan_requirements": ["boost/1.86.0", "nlohmann_json/3.12.0", "openssl/3.6.3"],
            "provenance_subjects": [],
        }
        self.manifest = canonical(manifest)
        contents = {
            ABI_IDENTITY_NAME: self.identity,
            self.corpus_name: self.corpus,
            "mcp-cpp-sdk-1.2.1.tar.gz": self.source,
            "mcp-cpp-sdk-1.2.1.tar.gz.asc": b"source signature\n",
            "release-manifest.json": self.manifest,
            "release-manifest.json.asc": b"manifest signature\n",
            "release-signing-key.asc": self.key,
        }
        sums = "".join(f"{digest(contents[name])}  {name}\n" for name in sorted(contents)).encode("ascii")
        contents["SHA256SUMS"] = sums
        contents["SHA256SUMS.asc"] = b"checksum signature\n"
        self.contents = contents
        self.assets = [
            {
                "id": index,
                "name": name,
                "size": len(content),
                "state": "uploaded",
                "digest": f"sha256:{digest(content)}",
            }
            for index, (name, content) in enumerate(sorted(contents.items()), 10)
        ]
        for name in (
            ABI_IDENTITY_NAME,
            self.corpus_name,
            "release-manifest.json",
            "release-manifest.json.asc",
            "SHA256SUMS",
            "SHA256SUMS.asc",
        ):
            (self.directory / name).write_bytes(contents[name])

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_paginated_plan_uses_asset_ids_and_bundle_verifies_all_boundaries(self) -> None:
        pages = [self.assets[:4], self.assets[4:]]
        plan = baseline_download_plan(self.selection, pages)
        self.assertEqual(plan["release_id"], 7)
        self.assertEqual(
            {asset["name"] for asset in plan["assets"]},
            {
                ABI_IDENTITY_NAME,
                self.corpus_name,
                "release-manifest.json",
                "release-manifest.json.asc",
                "SHA256SUMS",
                "SHA256SUMS.asc",
            },
        )
        verified: list[tuple[str, str]] = []
        evidence = verify_baseline_bundle(
            self.selection,
            pages,
            self.directory,
            signature_verifier=lambda signed, signature: verified.append((signed.name, signature.name)),
            expected_signers=SIGNERS,
        )
        self.assertEqual(evidence["corpus_sha256"], digest(self.corpus))
        self.assertEqual(evidence["identity_sha256"], digest(self.identity))
        self.assertEqual(
            verified,
            [
                ("release-manifest.json", "release-manifest.json.asc"),
                ("SHA256SUMS", "SHA256SUMS.asc"),
            ],
        )

    def test_download_uses_only_planned_asset_ids_and_checks_each_digest(self) -> None:
        plan = baseline_download_plan(self.selection, self.assets)
        destination = self.directory / "downloaded"
        contents_by_id = {
            asset["id"]: self.contents[asset["name"]]
            for asset in plan["assets"]
        }
        requested: list[int] = []

        def download(asset_id: int, output: Path) -> None:
            requested.append(asset_id)
            output.write_bytes(contents_by_id[asset_id])

        download_planned_assets(
            plan,
            repository="yurirocha15/mcp-cpp-sdk",
            directory=destination,
            downloader=download,
        )
        self.assertEqual(requested, [asset["id"] for asset in plan["assets"]])
        self.assertEqual({path.name for path in destination.iterdir()}, {asset["name"] for asset in plan["assets"]})

        bad_destination = self.directory / "bad-download"
        with self.assertRaisesRegex(AbiReleasePolicyError, "differs"):
            download_planned_assets(
                plan,
                repository="yurirocha15/mcp-cpp-sdk",
                directory=bad_destination,
                downloader=lambda _asset_id, output: output.write_bytes(b"tampered"),
            )

    def test_server_digest_checksum_and_identity_mismatches_fail_closed(self) -> None:
        (self.directory / self.corpus_name).write_bytes(self.corpus + b"tampered")
        with self.assertRaisesRegex(AbiReleasePolicyError, "GitHub digest"):
            verify_baseline_bundle(
                self.selection,
                self.assets,
                self.directory,
                signature_verifier=lambda _signed, _signature: None,
                expected_signers=SIGNERS,
            )
        (self.directory / self.corpus_name).write_bytes(self.corpus)
        assets = copy.deepcopy(self.assets)
        next(item for item in assets if item["name"] == self.corpus_name)["digest"] = "sha256:" + "f" * 64
        with self.assertRaises(AbiReleasePolicyError):
            verify_baseline_bundle(
                self.selection,
                assets,
                self.directory,
                signature_verifier=lambda _signed, _signature: None,
                expected_signers=SIGNERS,
            )

    def test_download_inventory_must_be_exact(self) -> None:
        (self.directory / "unexpected.txt").write_text("x", encoding="ascii")
        with self.assertRaisesRegex(AbiReleasePolicyError, "inventory"):
            verify_baseline_bundle(
                self.selection,
                self.assets,
                self.directory,
                signature_verifier=lambda _signed, _signature: None,
                expected_signers=SIGNERS,
            )


if __name__ == "__main__":
    unittest.main()
