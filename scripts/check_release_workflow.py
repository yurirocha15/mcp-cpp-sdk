#!/usr/bin/env python3
"""Offline structural security checks for the source release workflow."""

from __future__ import annotations

import ast
import json
import re
import sys
from pathlib import Path

from workflow_yaml import WorkflowYamlError, validate_workflows, workflow_paths

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from release.model import ValidationError
from release.native_builder import load_builder_lock


WORKFLOW_PATH = Path(".github/workflows/release.yml")
CI_WORKFLOW_PATH = Path(".github/workflows/ci.yml")
CONTRACT_PATH = Path("scripts/release_dispatch_contract.py")
NATIVE_TARGETS_PATH = Path("packaging/native-targets.json")
NATIVE_BUILDER_LOCK_PATH = Path("release/native-builders/lock.json")
ACTIONLINT_RUNNER_PATH = Path("scripts/release/lint_workflows.sh")
POWERSHELL_SYNTAX_PATH = Path("scripts/release/check_powershell_syntax.ps1")
CLOUDSMITH_VERIFY_PATH = Path("scripts/release/verify_cloudsmith_cli.sh")
FULL_SHA = re.compile(r"^[0-9a-f]{40}$")
ACTION_ALLOWLIST = {
    "actions/checkout": "9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0",
    "actions/upload-artifact": "043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    "actions/download-artifact": "3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
    "actions/attest": "f7c74d28b9d84cb8768d0b8ca14a4bac6ef463e6",
    "actions/create-github-app-token": "bcd2ba49218906704ab6c1aa796996da409d3eb1",
    "actions/setup-python": "ece7cb06caefa5fff74198d8649806c4678c61a1",
    "cloudsmith-io/cloudsmith-cli-action": "159f1619275d5d3147f059c3cc110938ec221d16",
    "ilammy/msvc-dev-cmd": "0b201ec74fa43914dc39ae48a89fd1d8cb592756",
}
EXPECTED_JOB_PERMISSIONS = {
    "contract": {"contents": "read"},
    "policy": {"contents": "read"},
    "repository-readiness": {"contents": "read"},
    "ledger-readiness": {"contents": "read", "issues": "read"},
    "immutability-gate": {"contents": "read", "issues": "read"},
    "existing-anchor-policy": {"contents": "read"},
    "fresh-provider-controls": {"contents": "read"},
    "preflight-cloudsmith": {"contents": "read", "issues": "read", "id-token": "write"},
    "preflight-aur": {"contents": "read", "issues": "read"},
    "preflight-homebrew": {"contents": "read", "issues": "read"},
    "preflight-chocolatey": {"contents": "read", "issues": "read"},
    "preflight-conan-fork": {"contents": "read", "issues": "read"},
    "preflight-conan-broker": {"contents": "read", "issues": "read"},
    "provider-preflight-gate": {"contents": "read"},
    "publication-gate": {"contents": "read"},
    "preparation-gate": {"contents": "read"},
    "construct-core": {"contents": "read"},
    "build-apt": {"contents": "read"},
    "build-rpm": {"contents": "read"},
    "build-windows": {"contents": "read"},
    "build-abi": {"contents": "read"},
    "assemble-unsigned": {"contents": "read"},
    "signing": {"contents": "read", "issues": "read"},
    "candidate-gate": {"contents": "read"},
    "validate-homebrew-package": {"contents": "read"},
    "validate-conan-linux": {"contents": "read"},
    "validate-conan-windows": {"contents": "read"},
    "package-validation-gate": {"contents": "read"},
    "validate-aur-packages": {"contents": "read"},
    "aur-validation-gate": {"contents": "read"},
    "validation-complete": {"contents": "read"},
    "attestation": {"contents": "read", "issues": "read", "id-token": "write", "attestations": "write"},
    "github-release": {"contents": "write", "issues": "read"},
    "github-anchor": {"contents": "read"},
    "publish-apt": {"contents": "read", "issues": "read", "id-token": "write"},
    "publish-rpm": {"contents": "read", "issues": "read", "id-token": "write"},
    "aur": {"contents": "read", "issues": "read"},
    "homebrew": {"contents": "read", "issues": "read"},
    "chocolatey": {"contents": "read", "issues": "read"},
    "conan-recipe": {"contents": "read", "issues": "read"},
    "conan": {"contents": "read", "issues": "read"},
    "ledger-coordinator": {"contents": "read", "issues": "read"},
}
EXPECTED_ENVIRONMENTS = {
    "immutability-gate": "release-admin-read",
    "preflight-cloudsmith": "release-cloudsmith",
    "preflight-aur": "release-aur",
    "preflight-homebrew": "release-homebrew",
    "preflight-chocolatey": "release-chocolatey",
    "preflight-conan-fork": "release-conan-recipe",
    "preflight-conan-broker": "release-control-dispatch",
    "signing": "release-signing",
    "attestation": "release-github",
    "github-release": "release-github",
    "publish-apt": "release-cloudsmith",
    "publish-rpm": "release-cloudsmith",
    "aur": "release-aur",
    "homebrew": "release-homebrew",
    "chocolatey": "release-chocolatey",
    "conan-recipe": "release-conan-recipe",
    "conan": "release-control-dispatch",
    "ledger-coordinator": "release-ledger",
}
PRIVILEGED_JOBS = frozenset(EXPECTED_ENVIRONMENTS)
PUBLISHER_JOBS = ("publish-apt", "publish-rpm", "aur", "homebrew", "chocolatey", "conan")
PROVIDER_PREFLIGHT_CONDITIONS = {
    "preflight-cloudsmith": (
        "${{ needs.contract.outputs.target_apt == 'true' || "
        "needs.contract.outputs.target_rpm == 'true' }}"
    ),
    "preflight-aur": "${{ needs.contract.outputs.target_aur == 'true' }}",
    "preflight-homebrew": "${{ needs.contract.outputs.target_homebrew == 'true' }}",
    "preflight-chocolatey": "${{ needs.contract.outputs.target_chocolatey == 'true' }}",
    "preflight-conan-fork": "${{ needs.contract.outputs.target_conan2 == 'true' }}",
    "preflight-conan-broker": "${{ needs.contract.outputs.target_conan2 == 'true' }}",
}
ALLOWED_SECRET_REFERENCES = {
    "secrets.RELEASE_GPG_PRIVATE_KEY_B64",
    "secrets.RELEASE_GPG_PASSPHRASE",
    "secrets.AUR_SSH_PRIVATE_KEY_B64",
    "secrets.AUR_SSH_KEY_PASSPHRASE",
    "secrets.HOMEBREW_APP_PRIVATE_KEY",
    "secrets.CHOCOLATEY_APP_PRIVATE_KEY",
    "secrets.CONAN_BROKER_APP_PRIVATE_KEY",
    "secrets.CONAN_RECIPE_APP_PRIVATE_KEY",
    "secrets.RELEASE_ADMIN_APP_PRIVATE_KEY",
    "secrets.RELEASE_LEDGER_APP_PRIVATE_KEY",
}
FIXED_RESULTS = {
    "SIGNED",
    "ATTESTED",
    "PUBLISHED",
    "SKIPPED_ALREADY_IDENTICAL",
    "DISPATCHED_PENDING_REVIEW",
    "DISPATCHED_PENDING_MODERATION",
}


class PolicyError(RuntimeError):
    """Raised when the workflow violates a release security invariant."""


def indented_block(lines: list[str], start: int, indentation: int) -> list[str]:
    block: list[str] = []
    for line in lines[start + 1 :]:
        stripped = line.strip()
        if stripped and len(line) - len(line.lstrip(" ")) <= indentation:
            break
        block.append(line)
    return block


def top_level_block(lines: list[str], key: str) -> list[str]:
    marker = f"{key}:"
    for index, line in enumerate(lines):
        if line == marker:
            return indented_block(lines, index, 0)
    raise PolicyError(f"missing top-level {key!r} block")


def job_blocks(lines: list[str]) -> dict[str, list[str]]:
    jobs = top_level_block(lines, "jobs")
    blocks: dict[str, list[str]] = {}
    for index, line in enumerate(jobs):
        match = re.fullmatch(r"  ([a-z0-9-]+):", line)
        if match:
            blocks[match.group(1)] = indented_block(jobs, index, 2)
    return blocks


def mapping_from_block(block: list[str], key: str, indentation: int) -> dict[str, str]:
    marker = " " * indentation + f"{key}:"
    for index, line in enumerate(block):
        if line == marker:
            nested = indented_block(block, index, indentation)
            result: dict[str, str] = {}
            for nested_line in nested:
                match = re.fullmatch(rf"\s{{{indentation + 2}}}([a-z-]+):\s+([^#]+?)\s*", nested_line)
                if match:
                    result[match.group(1)] = match.group(2).strip("'\"")
            return result
    return {}


def scalar_from_block(block: list[str], key: str, indentation: int) -> str | None:
    pattern = re.compile(rf"\s{{{indentation}}}{re.escape(key)}:\s+([^#]+?)\s*$")
    for line in block:
        match = pattern.fullmatch(line)
        if match:
            return match.group(1).strip("'\"")
    return None


def check_trigger(lines: list[str]) -> None:
    trigger = top_level_block(lines, "on")
    trigger_keys = [
        match.group(1)
        for line in trigger
        if (match := re.fullmatch(r"  ([a-z_]+):", line))
    ]
    if trigger_keys != ["workflow_dispatch"]:
        raise PolicyError(f"release trigger must be workflow_dispatch only, found {trigger_keys}")
    forbidden = ("pull_request_target", "repository_dispatch", "workflow_run", "schedule", "release:")
    if any(item in "\n".join(trigger) for item in forbidden):
        raise PolicyError("release workflow contains a forbidden trigger")


def check_dispatch_contract(lines: list[str]) -> None:
    text = "\n".join(lines)
    for fragment in (
        "ledger_issue:",
        "Exact operation:tag:github[,channels]:ledger_issue (or :all:)",
        "vars.RELEASE_EXPECTED_REPOSITORY_ID",
        "vars.RELEASE_EXPECTED_OWNER_ID",
        "run: python3 -I -S scripts/release_dispatch_contract.py",
    ):
        if fragment not in text:
            raise PolicyError(f"dispatch contract is missing {fragment!r}")
    trigger = top_level_block(lines, "on")
    input_names = {
        match.group(1)
        for line in trigger
        if (match := re.fullmatch(r"      ([a-z0-9_]+):", line))
    }
    expected_inputs = {
        "tag", "operation", "conan2", "apt", "rpm", "aur", "homebrew",
        "chocolatey", "ledger_issue", "confirmation",
    }
    if input_names != expected_inputs:
        raise PolicyError(f"workflow-dispatch input inventory mismatch: {sorted(input_names)}")
    if text.count("        type: boolean") != 6 or text.count("        default: false") != 6:
        raise PolicyError("all six third-party channels must be default-off boolean inputs")
    for operation in (
        "validate-selected",
        "validate-all",
        "publish-selected",
        "publish-retry-selected",
        "publish-all",
    ):
        if f"          - {operation}" not in text:
            raise PolicyError(f"release operation choice is missing {operation!r}")

    contract = CONTRACT_PATH.read_text(encoding="utf-8")
    for fragment in (
        '"conan2",',
        '"apt",',
        '"rpm",',
        '"publish-selected": ("publish", False, False)',
        '"publish-retry-selected": ("publish", False, True)',
        '"publish-all": ("publish", True, False)',
        'value not in {"true", "false"}',
        'all-channel operation cannot be combined with individual channels',
        'confirmation must exactly bind operation:tag:channels:ledger_issue',
        '",".join(("github", *selected))',
        'release_kind == "rc" and channels',
        'expected_repository_id=os.environ["EXPECTED_REPOSITORY_ID"]',
    ):
        if fragment not in contract:
            raise PolicyError(f"dispatch validator is missing {fragment!r}")
    if "targets_input" in contract or "DISPATCH_TARGETS" in text:
        raise PolicyError("free-form release target strings are forbidden")
    if re.search(r'EXPECTED_(?:REPOSITORY|OWNER)_ID\s*=\s*"[0-9]+"', contract):
        raise PolicyError("numeric repository identities must come from repository variables")


def check_global_permissions_and_concurrency(lines: list[str]) -> None:
    permissions = [line.strip() for line in top_level_block(lines, "permissions") if line.strip()]
    if permissions != ["contents: read"]:
        raise PolicyError("top-level permissions must be exactly contents: read")
    concurrency = [line.strip() for line in top_level_block(lines, "concurrency") if line.strip()]
    if concurrency != ["group: release-publishing", "cancel-in-progress: false"]:
        raise PolicyError("release publication must be globally serialized with cancellation disabled")


def check_action_pins(lines: list[str]) -> None:
    uses_lines = [line.strip() for line in lines if line.strip().startswith("uses:")]
    if not uses_lines:
        raise PolicyError("release workflow contains no pinned actions")
    for uses_line in uses_lines:
        reference = uses_line.removeprefix("uses:").split("#", 1)[0].strip()
        if reference.startswith("./") or "@" not in reference:
            raise PolicyError(f"local or unversioned action is forbidden: {reference}")
        action, sha = reference.rsplit("@", 1)
        if not FULL_SHA.fullmatch(sha):
            raise PolicyError(f"action is not pinned to a full commit SHA: {reference}")
        if ACTION_ALLOWLIST.get(action) != sha:
            raise PolicyError(f"action is not in the literal allowlist: {reference}")


def check_permissions(blocks: dict[str, list[str]]) -> None:
    if set(blocks) != set(EXPECTED_JOB_PERMISSIONS):
        missing = sorted(set(EXPECTED_JOB_PERMISSIONS) - set(blocks))
        extra = sorted(set(blocks) - set(EXPECTED_JOB_PERMISSIONS))
        raise PolicyError(f"unexpected job inventory; missing={missing}, extra={extra}")
    writers: list[str] = []
    for job, expected in EXPECTED_JOB_PERMISSIONS.items():
        actual = mapping_from_block(blocks[job], "permissions", 4)
        if actual != expected:
            raise PolicyError(f"job {job!r} permissions {actual} do not equal {expected}")
        if actual.get("issues") == "write":
            writers.append(job)
    if writers:
        raise PolicyError(f"the default Actions token must never write issues: {writers}")


def check_environments(blocks: dict[str, list[str]]) -> None:
    for job, expected in EXPECTED_ENVIRONMENTS.items():
        actual = scalar_from_block(blocks[job], "environment", 4)
        if actual != expected:
            raise PolicyError(f"job {job!r} environment {actual!r} does not equal {expected!r}")
    for job in set(blocks) - set(EXPECTED_ENVIRONMENTS):
        if scalar_from_block(blocks[job], "environment", 4) is not None:
            raise PolicyError(f"non-publisher job {job!r} must not use an environment")


def check_privileged_jobs(blocks: dict[str, list[str]]) -> None:
    forbidden = (
        "actions/cache",
        "cmake ",
        "dpkg-buildpackage",
        "rpmbuild",
        "choco pack",
    )
    for job in PRIVILEGED_JOBS:
        text = "\n".join(blocks[job])
        for fragment in forbidden:
            if fragment in text:
                raise PolicyError(f"privileged job {job!r} executes forbidden source/tool fragment {fragment!r}")
        checkout_positions = [
            index for index, line in enumerate(blocks[job])
            if "uses: actions/checkout@" in line
        ]
        if not checkout_positions:
            raise PolicyError(f"privileged job {job!r} must checkout its exact tagged policy")
        for checkout_position in checkout_positions:
            step_start = checkout_position
            while step_start >= 0 and not re.match(r"^      - (?:name|uses):", blocks[job][step_start]):
                step_start -= 1
            step_end = checkout_position + 1
            while step_end < len(blocks[job]) and not re.match(
                r"^      - (?:name|uses):", blocks[job][step_end]
            ):
                step_end += 1
            checkout_step = blocks[job][step_start:step_end]
            checkout_text = "\n".join(checkout_step)
            if checkout_text.count("persist-credentials: false") != 1:
                raise PolicyError(
                    f"privileged job {job!r} must disable credentials in every checkout step"
                )
        secret_positions = [
            index for index, line in enumerate(blocks[job]) if "secrets." in line
        ]
        if checkout_positions and secret_positions and max(checkout_positions) > min(secret_positions):
            raise PolicyError(f"privileged job {job!r} must checkout before requesting secrets")
        for line in blocks[job]:
            command = line.strip()
            if re.search(r"\bpython3\s+scripts/", command) and not re.search(
                r"\bpython3 -I -S scripts/run_release_tool\.py\b", command
            ):
                raise PolicyError(
                    f"privileged job {job!r} executes an unapproved checked-out Python script"
                )
        if job not in {"immutability-gate", "github-release"} and "actions/download-artifact@" not in text:
            raise PolicyError(f"privileged publisher {job!r} must consume an artifact handoff")


def check_secret_scope(blocks: dict[str, list[str]]) -> None:
    found: set[str] = set()
    for job, block in blocks.items():
        references = set(re.findall(r"secrets\.[A-Z0-9_]+", "\n".join(block)))
        if references and job not in PRIVILEGED_JOBS:
            raise PolicyError(f"unprotected job {job!r} references a secret")
        found.update(references)
    if found != ALLOWED_SECRET_REFERENCES:
        raise PolicyError(f"secret reference inventory mismatch: {sorted(found)}")


def check_credential_rechecks(blocks: dict[str, list[str]]) -> None:
    """Require a fresh ledger binding immediately before each credential boundary."""

    markers = {
        "immutability-gate": "secrets.RELEASE_ADMIN_APP_PRIVATE_KEY",
        "preflight-cloudsmith": "uses: cloudsmith-io/cloudsmith-cli-action@",
        "preflight-aur": "secrets.AUR_SSH_PRIVATE_KEY_B64",
        "preflight-homebrew": "secrets.HOMEBREW_APP_PRIVATE_KEY",
        "preflight-chocolatey": "secrets.CHOCOLATEY_APP_PRIVATE_KEY",
        "preflight-conan-fork": "secrets.CONAN_RECIPE_APP_PRIVATE_KEY",
        "preflight-conan-broker": "secrets.CONAN_BROKER_APP_PRIVATE_KEY",
        "signing": "secrets.RELEASE_GPG_PRIVATE_KEY_B64",
        "attestation": "uses: actions/attest@",
        "github-release": "release.github_publication create-release",
        "publish-apt": "uses: cloudsmith-io/cloudsmith-cli-action@",
        "publish-rpm": "uses: cloudsmith-io/cloudsmith-cli-action@",
        "aur": "secrets.AUR_SSH_PRIVATE_KEY_B64",
        "homebrew": "secrets.HOMEBREW_APP_PRIVATE_KEY",
        "chocolatey": "secrets.CHOCOLATEY_APP_PRIVATE_KEY",
        "conan-recipe": "secrets.CONAN_RECIPE_APP_PRIVATE_KEY",
        "conan": "secrets.CONAN_BROKER_APP_PRIVATE_KEY",
        "ledger-coordinator": "secrets.RELEASE_LEDGER_APP_PRIVATE_KEY",
    }
    readiness_artifact = "release-ledger-readiness-${{ github.run_id }}-${{ github.run_attempt }}"
    for job, credential_marker in markers.items():
        block = "\n".join(blocks[job])
        if readiness_artifact not in block:
            raise PolicyError(f"credential-bearing job {job!r} lacks the exact ledger handoff")
        recheck = block.find("scripts/release/recheck_ledger.sh")
        credential = block.find(credential_marker)
        if recheck < 0 or credential < 0 or recheck > credential:
            raise PolicyError(
                f"credential-bearing job {job!r} does not recheck the ledger before credentials"
            )


def check_publication_invariants(blocks: dict[str, list[str]], text: str) -> None:
    gate = "\n".join(blocks["publication-gate"])
    if (
        "vars.RELEASE_PUBLISHING_ENABLED" not in text
        or "release.workflow_gate publishing" not in gate
        or '--enabled "${RELEASE_PUBLISHING_ENABLED}"' not in gate
    ):
        raise PolicyError("publication gate must enforce the exact RELEASE_PUBLISHING_ENABLED=true kill switch")
    ledger_readiness = "\n".join(blocks["ledger-readiness"])
    for fragment in (
        "scripts/release/prepare_ledger.sh", "SELECTED_CHANNELS", "RETRY_AUTHORIZED",
        "RELEASE_LEDGER_BOT_ID", "ledger_readiness_sha256", "prior_manifest_sha256",
    ):
        if fragment not in ledger_readiness:
            raise PolicyError(f"ledger/anchor readiness is missing {fragment!r}")
    immutability = "\n".join(blocks["immutability-gate"])
    for fragment in (
        "permission-administration: read",
        "secrets.RELEASE_ADMIN_APP_PRIVATE_KEY",
        "repos/${REPOSITORY}/immutable-releases",
    ):
        if fragment not in immutability:
            raise PolicyError(f"immutable Releases gate is missing {fragment!r}")
    github_release = "\n".join(blocks["github-release"])
    if "release.github_publication create-release" not in github_release:
        raise PolicyError("GitHub release creation is not delegated to its tested adapter")
    anchor = "\n".join(blocks["github-anchor"])
    for fragment in (
        "release.github_anchor download-and-verify",
        "PRIOR_MANIFEST_SHA256",
        "--prior-manifest-sha256",
        "--tag-object-sha",
    ):
        if fragment not in anchor:
            raise PolicyError(f"immutable anchor verification is missing {fragment!r}")
    github_workflow = "\n".join((github_release, anchor))
    if any(command in github_workflow for command in (
        "gh release create",
        "gh release download",
        "gh release view",
        "gh api --paginate",
    )):
        raise PolicyError("GitHub publication logic must not be embedded in workflow YAML")
    check_github_release_source(
        Path("release/github_publication.py").read_text(encoding="utf-8")
    )
    check_github_anchor_source(Path("release/github_anchor.py").read_text(encoding="utf-8"))
    for job, expected in PROVIDER_PREFLIGHT_CONDITIONS.items():
        if scalar_from_block(blocks[job], "if", 4) != expected:
            raise PolicyError(
                f"provider preflight {job!r} is not gated by its normalized channel checkbox"
            )
    for job in PUBLISHER_JOBS:
        block = "\n".join(blocks[job])
        if "github-anchor" not in block or "Verify fixed anchor handoff" not in block:
            raise PolicyError(f"publisher {job!r} is not bound to the verified GitHub anchor")
    selected_conditions = {
        "publish-apt": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_apt == 'true' }}",
        "publish-rpm": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_rpm == 'true' }}",
        "aur": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_aur == 'true' }}",
        "homebrew": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_homebrew == 'true' }}",
        "chocolatey": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_chocolatey == 'true' }}",
        "conan-recipe": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_conan2 == 'true' }}",
        "conan": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_conan2 == 'true' }}",
    }
    for job, expected in selected_conditions.items():
        if scalar_from_block(blocks[job], "if", 4) != expected:
            raise PolicyError(f"publisher {job!r} is not gated by its normalized channel checkbox")
    check_aur_target_binding(blocks)
    aur = "\n".join(blocks["aur"])
    for path in (
        "scripts/release/publish_aur.sh",
        "scripts/release/aur_ssh_agent.sh",
        "release/aur_ssh.py",
    ):
        aur += "\n" + Path(path).read_text(encoding="utf-8")
    check_aur_publisher_text(aur)
    check_provider_publication_invariants(blocks, text)


def check_aur_target_binding(
    blocks: dict[str, list[str]],
    preflight_source: str | None = None,
    publisher_source: str | None = None,
) -> None:
    """Bind both AUR credential boundaries to the signed package target."""

    if preflight_source is None:
        preflight_source = Path("scripts/release/preflight_aur.sh").read_text(
            encoding="utf-8"
        )
    if publisher_source is None:
        publisher_source = Path("scripts/release/publish_aur.sh").read_text(
            encoding="utf-8"
        )
    exact_environment = (
        "AUR_PACKAGE_BASE: mcp-cpp-sdk",
        "AUR_CLONE_URL: ssh://aur@aur.archlinux.org/mcp-cpp-sdk.git",
    )
    job_requirements = {
        "preflight-aur": (
            "--contract existing-anchor/release-publication-contract.json --name aur",
        ),
        "aur": (
            "--contract release-assets/release-publication-contract.json --name aur",
        ),
    }
    for job, required in job_requirements.items():
        block = "\n".join(blocks[job])
        for fragment in (*exact_environment, *required):
            if block.count(fragment) != 1:
                raise PolicyError(f"AUR job {job!r} is not bound to exact target {fragment!r}")
        if job == "preflight-aur" and block.count(
            "if: ${{ needs.ledger-readiness.outputs.anchor_exists == 'true' }}"
        ) != 2:
            raise PolicyError("AUR preflight historical target checks are not fail-closed")
        for fragment in (
            '--repository "${AUR_CLONE_URL}"',
            '--package-base "${AUR_PACKAGE_BASE}"',
            "--architecture x86_64",
        ):
            if block.count(fragment) != 1:
                raise PolicyError(f"AUR job {job!r} has an incomplete signed target binding")
        binding = block.find("release.publication_contract")
        credential = block.find("secrets.AUR_SSH_PRIVATE_KEY_B64")
        if binding < 0 or credential < 0 or binding > credential:
            raise PolicyError(f"AUR job {job!r} reads credentials before target binding")

    source_requirements = {
        "preflight": (
            preflight_source,
            (
                'test "${AUR_PACKAGE_BASE}" = "mcp-cpp-sdk"',
                'test "${AUR_CLONE_URL}" = "ssh://aur@aur.archlinux.org/mcp-cpp-sdk.git"',
                '--package "${AUR_PACKAGE_BASE}"',
            ),
        ),
        "publisher": (
            publisher_source,
            (
                'test "${AUR_PACKAGE_BASE}" = "mcp-cpp-sdk"',
                'test "${AUR_CLONE_URL}" = "ssh://aur@aur.archlinux.org/mcp-cpp-sdk.git"',
                'git clone --no-checkout "${AUR_CLONE_URL}" aur-repository',
            ),
        ),
    }
    for label, (source, fragments) in source_requirements.items():
        for fragment in fragments:
            if source.count(fragment) != 1:
                raise PolicyError(f"AUR {label} does not enforce exact target {fragment!r}")


def check_github_release_source(source: str) -> None:
    """Require the extracted GitHub write adapter to preserve every safety gate."""

    for fragment in (
        "def create_release(",
        "verify_live_github_tag(",
        "probe_existing_release(",
        "_release_assets(directory)",
        '"--verify-tag"',
        '"--notes-file"',
        '"--prerelease"',
        '"--latest=false"',
        'commands.add_parser("create-release")',
    ):
        if fragment not in source:
            raise PolicyError(f"GitHub release adapter is missing {fragment!r}")
    if "shell=True" in source:
        raise PolicyError("GitHub release adapter must not invoke a shell")


def check_github_anchor_source(source: str) -> None:
    """Require the extracted anchor adapter to perform complete public readback."""

    for fragment in (
        "def download_and_verify_anchor(",
        "verify_live_github_tag(",
        "_download_release(",
        "_release_metadata(",
        "verify_candidate(",
        "verify_anchor(",
        '"--paginate"',
        '"--slurp"',
        "assets?per_page=100",
        "candidate and anchor manifest digests disagree",
    ):
        if fragment not in source:
            raise PolicyError(f"GitHub anchor adapter is missing {fragment!r}")
    if "shell=True" in source:
        raise PolicyError("GitHub anchor adapter must not invoke a shell")


def check_aur_publisher_text(aur: str) -> None:
    """Reject weakened SSH handling across the AUR workflow and adapter."""
    for fragment in (
        "secrets.AUR_SSH_KEY_PASSPHRASE",
        "SSH_ASKPASS_REQUIRE=force",
        "ssh-add",
        "BatchMode=yes",
        "HostKeyAlgorithms=ssh-ed25519",
        "StrictHostKeyChecking=yes",
        "UserKnownHostsFile=${AUR_SSH_KNOWN_HOSTS_FILE}",
        "SHA256:RFzBCUItH9LZS0cKB5UE6ceAYhBD5C8GeOBip8Z11+4",
        "release.aur_ssh",
    ):
        if fragment not in aur:
            raise PolicyError(f"AUR publisher is missing passphrase-protected SSH handling {fragment!r}")
    if "ssh-keyscan" in aur or "StrictHostKeyChecking=no" in aur:
        raise PolicyError("AUR publisher weakens SSH host-key verification")


def check_provider_publication_invariants(blocks: dict[str, list[str]], text: str) -> None:
    """Check provider and ledger invariants after the AUR boundary."""
    check_cloudsmith_cli_integrity(blocks)
    cloudsmith = "\n".join(blocks["publish-apt"] + blocks["publish-rpm"])
    if "cloudsmith-cli-action@159f1619275d5d3147f059c3cc110938ec221d16" not in cloudsmith:
        raise PolicyError("Cloudsmith action pin is missing")
    if cloudsmith.count('cli-version: "1.19.0"') != 2:
        raise PolicyError("each Cloudsmith publisher must pin CLI version 1.19.0")
    for job in ("publish-apt", "publish-rpm"):
        publisher = "\n".join(blocks[job])
        if publisher.count(
            "CLOUDSMITH_PUBLISH_SERVICE_USERNAME: "
            "${{ vars.CLOUDSMITH_PUBLISH_SERVICE_USERNAME }}"
        ) != 1 or publisher.count(
            '--expected-username "${CLOUDSMITH_PUBLISH_SERVICE_USERNAME}"'
        ) != 1:
            raise PolicyError(
                f"Cloudsmith publisher {job!r} does not bind the exact OIDC service identity"
            )
    preflights = "\n".join(
        "\n".join(blocks[name])
        for name in (
            "preflight-cloudsmith", "preflight-aur", "preflight-homebrew",
            "preflight-chocolatey", "preflight-conan-fork", "preflight-conan-broker",
            "provider-preflight-gate",
        )
    )
    for fragment in (
        "release.cloudsmith_publish preflight",
        "scripts/release/preflight_aur.sh",
        "scripts/release/preflight_aur_auth.sh",
        "release.provider_preflight workflow",
        "release.provider_preflight conan-fork",
        "release.provider_preflight gate",
    ):
        if fragment not in preflights:
            raise PolicyError(f"selected provider preflights are missing {fragment!r}")
    if preflights.count('cli-version: "1.19.0"') != 1:
        raise PolicyError("Cloudsmith preflight must pin CLI version 1.19.0")
    for preflight in ("preflight-homebrew", "preflight-chocolatey", "preflight-conan-broker"):
        block = "\n".join(blocks[preflight])
        for fragment in (
            "control_tag: ${{ steps.control.outputs.control_tag }}",
            "control_sha: ${{ steps.control.outputs.control_sha }}",
            "--controls packaging/provider-controls.json",
            '--github-output "${GITHUB_OUTPUT}"',
        ):
            if fragment not in block:
                raise PolicyError(
                    f"provider preflight {preflight!r} does not export its reviewed control tag"
                )
    control_dependencies = {
        "homebrew": "preflight-homebrew",
        "chocolatey": "preflight-chocolatey",
        "conan": "preflight-conan-broker",
    }
    for job, preflight in control_dependencies.items():
        block = "\n".join(blocks[job])
        for fragment in (
            f"      - {preflight}",
            f"needs.{preflight}.outputs.control_tag",
            f"needs.{preflight}.outputs.control_sha",
            '--source-workflow-head-sha "${SOURCE_WORKFLOW_HEAD_SHA}"',
            '--provider-control-tag "${PROVIDER_CONTROL_TAG}"',
            '--provider-control-sha "${PROVIDER_CONTROL_SHA}"',
            '--github-output "${GITHUB_OUTPUT}"',
            "downstream_run_id: ${{ steps.dispatch.outputs.downstream_workflow_run_id }}",
        ):
            if fragment not in block:
                raise PolicyError(
                    f"provider dispatcher {job!r} is not bound to {preflight!r}"
                )
    provider_source = Path("release/github_provider.py").read_text(encoding="utf-8")
    for fragment in (
        '"provider_control_sha"',
        '"source_workflow_head_sha"',
        '{"ref": control_tag, "inputs": dict(inputs)}',
        'git/ref/tags/{control_tag}',
        'ref_object.get("sha") != expected_control_sha',
        'set(value) != {"workflow_run_id", "run_url", "html_url"}',
        'X-GitHub-Api-Version: 2026-03-10',
    ):
        if fragment not in provider_source:
            raise PolicyError(
                f"GitHub provider adapter does not preserve control binding {fragment!r}"
            )
    preparation = "\n".join(blocks["preparation-gate"])
    if "provider-preflight-gate" not in preparation or "publication-gate" not in preparation:
        raise PolicyError("release construction is not gated by provider and publication policy")
    ledger = "\n".join(blocks["ledger-coordinator"])
    for fragment in (
        "always()", "release-ledger", "permission-issues: write",
        "scripts/release/append_ledger.sh", "release.ledger_updates",
    ):
        if fragment not in ledger:
            raise PolicyError(f"ledger coordinator is missing {fragment!r}")
    if "gh issue edit" in ledger or re.search(r"(?m)^\s+issues: write\s*$", ledger):
        raise PolicyError("ledger must use only the distinct App token and append-only comments")


def check_cloudsmith_cli_integrity(
    blocks: dict[str, list[str]], verifier_source: str | None = None
) -> None:
    """Require checksum verification before any downloaded Cloudsmith CLI runs."""

    if verifier_source is None:
        verifier_source = CLOUDSMITH_VERIFY_PATH.read_text(encoding="utf-8")
    required_verifier_fragments = (
        'expected_sha256="c076e4b002ee07f26774c0f8a9134f52a73b16a3fb10adb31891475485e28038"',
        "sha256sum --check --status",
        'test ! -L "${cli_path}"',
        'test "$(readlink -f "${resolved_cli}")" = "$(readlink -f "${cli_path}")"',
        'CLI Package Version: 1.19.0',
        'API Package Version: 2.0.27',
    )
    for fragment in required_verifier_fragments:
        if fragment not in verifier_source:
            raise PolicyError(f"Cloudsmith CLI verifier is missing {fragment!r}")
    if verifier_source.find("sha256sum --check --status") > verifier_source.find(
        '"${cli_path}" --version'
    ):
        raise PolicyError("Cloudsmith CLI must be checksummed before it is executed")

    command_markers = {
        "preflight-cloudsmith": "release.cloudsmith_publish preflight",
        "publish-apt": "release.cloudsmith_publish publish",
        "publish-rpm": "release.cloudsmith_publish publish",
    }
    for job, command_marker in command_markers.items():
        block = "\n".join(blocks[job])
        action = block.find("uses: cloudsmith-io/cloudsmith-cli-action@")
        verifier = block.find("run: bash scripts/release/verify_cloudsmith_cli.sh")
        command = block.find(command_marker)
        if (
            action < 0
            or verifier < 0
            or command < 0
            or not action < verifier < command
            or block.count("run: bash scripts/release/verify_cloudsmith_cli.sh") != 1
        ):
            raise PolicyError(
                f"Cloudsmith job {job!r} does not verify the downloaded CLI before use"
            )


def check_fixed_outputs(blocks: dict[str, list[str]]) -> None:
    emitted: set[str] = set()
    for job in PRIVILEGED_JOBS:
        for value in re.findall(r"result=([A-Z_]+)", "\n".join(blocks[job])):
            if value not in FIXED_RESULTS:
                raise PolicyError(f"job {job!r} emits non-approved result {value!r}")
            emitted.add(value)
    gate_source = Path("release/workflow_gate.py").read_text(encoding="utf-8")
    emitted.update(value for value in FIXED_RESULTS if f'"{value}"' in gate_source)
    required = {
        "PUBLISHED", "SKIPPED_ALREADY_IDENTICAL",
        "DISPATCHED_PENDING_REVIEW", "DISPATCHED_PENDING_MODERATION",
    }
    if not required.issubset(emitted):
        raise PolicyError(f"publisher result coverage is incomplete: {sorted(emitted)}")
    for job in PUBLISHER_JOBS:
        if "result: ${{ steps." not in "\n".join(blocks[job]):
            raise PolicyError(f"publisher {job!r} lacks a sanitized result output")
    if re.search(r"echo\s+['\"]?result=LIVE", "\n".join(sum(blocks.values(), []))):
        raise PolicyError("source workflow must not emit the removed LIVE ledger result")


def check_repository_readiness_source(source: str) -> None:
    """Require fresh tags and historical anchors to use distinct trust rules."""

    for fragment in (
        "def validate_tag_position(",
        "if not anchor_exists and commit != dispatch_head_sha:",
        "registry = load_trusted_signers(args.trusted_signers)",
        "active = registry.active",
        "accepted_signers = registry.signers",
        "selected_signer = _verify_tag_signature(tag=args.tag, signers=accepted_signers)",
    ):
        if fragment not in source:
            raise PolicyError(
                f"repository readiness lacks fresh/historical trust policy {fragment!r}"
            )


def check_artifact_construction(blocks: dict[str, list[str]]) -> None:
    for job in (
        "construct-core", "build-apt", "build-rpm", "build-windows", "build-abi",
        "assemble-unsigned",
    ):
        block = "\n".join(blocks[job])
        if "actions/upload-artifact@" not in block:
            raise PolicyError(f"artifact construction job {job!r} does not upload its output")
        if job.startswith("build-"):
            expected = "${{ needs.contract.outputs.release_kind == 'stable' && needs.ledger-readiness.outputs.anchor_exists == 'false' }}"
            if scalar_from_block(blocks[job], "if", 4) != expected:
                raise PolicyError(f"artifact build job {job!r} must build every fresh stable anchor")
            if re.search(r"needs\.contract\.outputs\.target_(?:apt|rpm|chocolatey|aur|homebrew|conan2)", block):
                raise PolicyError(f"artifact build job {job!r} must not depend on publication selection")
    native_jobs = "\n".join(
        blocks["build-apt"] + blocks["build-rpm"] + blocks["build-windows"]
        + blocks["validate-aur-packages"]
    )
    if "self-hosted" in native_jobs:
        raise PolicyError("release construction must not run on persistent self-hosted machines")
    for job in ("build-apt", "build-rpm"):
        block = "\n".join(blocks[job])
        for fragment in (
            "runs-on: ${{ matrix.runner }}",
            "release.native_builder run",
            "--lock release/native-builders/lock.json",
            "matrix.builder_image",
        ):
            if fragment not in block:
                raise PolicyError(f"native job {job!r} is missing locked hosted builder policy {fragment!r}")
    windows = "\n".join(blocks["build-windows"])
    for fragment in (
        "runs-on: windows-2022",
        "actions/setup-python@",
        "scripts/release/install_windows_conan.ps1",
        "release/windows/conan-requirements.txt",
    ):
        if fragment not in windows:
            raise PolicyError(f"Windows release construction is missing {fragment!r}")
    if "windows-2025" in windows:
        raise PolicyError("Windows release construction must remain on windows-2022")
    aur_validation = "\n".join(blocks["validate-aur-packages"])
    aur_validation += "\n" + Path("scripts/release/validate_aur_packages.sh").read_text(encoding="utf-8")
    aur_validation += "\n" + Path("release/aur_validation.py").read_text(encoding="utf-8")
    aur_validation += "\n" + Path("release/aur_package_installer.py").read_text(encoding="utf-8")
    aur_validation += "\n" + Path("release/aur_package_installer.py").read_text(encoding="utf-8")
    expected_native_validation = "${{ needs.contract.outputs.release_kind == 'stable' && needs.ledger-readiness.outputs.anchor_exists == 'false' }}"
    if scalar_from_block(blocks["validate-aur-packages"], "if", 4) != expected_native_validation:
        raise PolicyError("AUR inputs must be validated for every fresh stable anchor")
    if "target_aur" in aur_validation:
        raise PolicyError("AUR construction validation must not depend on publication selection")
    for fragment in (
        "release.native_builder run", "aur-x86_64", "makepkg --printsrcinfo",
        "cmp --silent .SRCINFO", "makepkg --verifysource", "makepkg --cleanbuild",
        'os.execv("/usr/bin/pacman", ("pacman", "-U", "--noconfirm", "--", *packages))',
        "mcp::sdk_shared", "mcp::sdk_static",
    ):
        if fragment not in aur_validation:
            raise PolicyError(f"AUR package validation is missing {fragment!r}")
    aur_gate = "\n".join(blocks["aur-validation-gate"])
    if (
        "validate-aur-packages.result" not in aur_gate
        or "release.workflow_gate aur" not in aur_gate
    ):
        raise PolicyError("GitHub publication is not gated on stable-anchor AUR validation")
    assembly = "\n".join(blocks["assemble-unsigned"])
    if assembly.count("if: ${{ needs.contract.outputs.release_kind == 'stable' }}") != 4:
        raise PolicyError("every stable anchor must assemble APT, RPM, Windows, and ABI artifact parts")
    if any(name in assembly for name in ("target_apt", "target_rpm", "target_chocolatey")):
        raise PolicyError("immutable artifact assembly must not depend on publication selection")
    if "release.construct_core" not in "\n".join(blocks["construct-core"]):
        raise PolicyError("core release construction is not delegated to its tested module")
    native_construction = "\n".join(blocks["build-apt"] + blocks["build-rpm"])
    if native_construction.count("release.native_builder run") != 2:
        raise PolicyError("APT and RPM construction must use the tested locked-builder module")
    manifest_construction = "\n".join(blocks["assemble-unsigned"])
    if "release.assemble_unsigned" not in manifest_construction:
        raise PolicyError("release manifest does not bind the complete stable channel capability set")
    abi_construction = "\n".join(blocks["build-abi"])
    for fragment in (
        "release.abi_build container-build", "release.abi_policy select",
        "release.abi_policy verify-bundle", "release.abi_build container-compare",
        "release-abi-${{ github.run_id }}-${{ github.run_attempt }}",
    ):
        if fragment not in abi_construction:
            raise PolicyError(f"ABI construction is missing {fragment!r}")
    if "build-abi" not in assembly:
        raise PolicyError("unsigned assembly does not consume the canonical ABI artifact")
    readiness = "\n".join(blocks["repository-readiness"])
    for fragment in (
        "release.repository_readiness", "apt_matrix", "rpm_matrix",
        "probe-existing-release", "--anchor-exists", "anchor_exists",
        "fromJSON(needs.repository-readiness.outputs.apt_matrix)",
        "fromJSON(needs.repository-readiness.outputs.rpm_matrix)",
    ):
        if fragment not in "\n".join((readiness, "\n".join(blocks["build-apt"]), "\n".join(blocks["build-rpm"]))):
            raise PolicyError(f"native target matrix is missing {fragment!r}")
    check_repository_readiness_source(
        Path("release/repository_readiness.py").read_text(encoding="utf-8")
    )
    candidate = "\n".join(blocks["candidate-gate"])
    for fragment in (
        "release.verify_candidate",
        "release-signed-${{ github.run_id }}-${{ github.run_attempt }}",
        "release-candidate-verified-${{ github.run_id }}-${{ github.run_attempt }}",
        "--release-notes included",
    ):
        if fragment not in candidate:
            raise PolicyError(f"pre-publication candidate gate is missing {fragment!r}")
    for job in ("validate-aur-packages", "attestation", "github-release"):
        block = "\n".join(blocks[job])
        if "candidate-gate" not in block or "release-candidate-verified-" not in block:
            raise PolicyError(f"job {job!r} does not consume the verified candidate handoff")
    validation = "\n".join(blocks["validation-complete"])
    for fragment in (
        "release.workflow_gate validation", "candidate-gate", "build-abi",
        "aur-validation-gate", "package-validation-gate", "--package-gate-result",
    ):
        if fragment not in validation:
            raise PolicyError(f"read-only validation completion is missing {fragment!r}")
    if "preparation-gate" not in "\n".join(blocks["construct-core"]):
        raise PolicyError("candidate construction bypasses the mode-aware preparation gate")
    if "publication-gate" in "\n".join(
        "\n".join(blocks[name])
        for name in ("construct-core", "build-apt", "build-rpm", "build-windows", "build-abi")
    ):
        raise PolicyError("validation construction still depends directly on the publish-only gate")
    attestation = "\n".join(blocks["attestation"])
    for gate in ("aur-validation-gate", "package-validation-gate"):
        if gate not in attestation:
            raise PolicyError(f"attestation may run before {gate!r} completes")
    github_release = "\n".join(blocks["github-release"])
    if "package-validation-gate" not in github_release:
        raise PolicyError("GitHub publication may bypass exact package-manager validation")


def check_native_targets(path: Path = NATIVE_TARGETS_PATH) -> None:
    try:
        targets = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PolicyError(f"native target inventory is unreadable: {error}") from error
    if not isinstance(targets, list) or len(targets) != 18:
        raise PolicyError("native target inventory must contain exactly 18 targets")
    if sum(item.get("format") == "apt" for item in targets if isinstance(item, dict)) != 10:
        raise PolicyError("native target inventory must contain exactly 10 APT targets")
    if sum(item.get("format") == "rpm" for item in targets if isinstance(item, dict)) != 8:
        raise PolicyError("native target inventory must contain exactly 8 RPM targets")
    expected_runners = {
        "aarch64": "ubuntu-24.04-arm",
        "x86_64": "ubuntu-24.04",
    }
    for target in targets:
        if not isinstance(target, dict):
            raise PolicyError("native target inventory contains a non-object")
        machine = target.get("builder_uname_machine")
        if target.get("runner") != expected_runners.get(machine):
            raise PolicyError("native target does not use its exact GitHub-hosted architecture")
    try:
        builders = load_builder_lock(NATIVE_BUILDER_LOCK_PATH, require_resolved=False)
    except ValidationError as error:
        raise PolicyError(f"native builder bootstrap lock is invalid: {error}") from error
    if len(builders) != 20 or builders[-2]["id"] != "aur-x86_64" or builders[-1]["id"] != "conan-linux-x86_64":
        raise PolicyError("native builder bootstrap inventory is incomplete")


def check_windows_dependency_locks() -> None:
    requirements = Path("release/windows/conan-requirements.txt").read_text(encoding="utf-8")
    if requirements.count("conan==2.30.0 --hash=sha256:") != 1:
        raise PolicyError("Windows Conan client lock does not contain exact Conan 2.30.0")
    if any(
        not re.fullmatch(r"[A-Za-z0-9_-]+==[^ ]+ --hash=sha256:[0-9a-f]{64}", line)
        for line in requirements.splitlines()
    ):
        raise PolicyError("Windows Conan client graph is not fully hash locked")
    dependency_lock = Path("release/windows/conan.lock").read_text(encoding="utf-8")
    for reference in ("boost/1.86.0#", "nlohmann_json/3.12.0#", "openssl/3.6.3#", "gtest/1.17.0#"):
        if dependency_lock.count(reference) != 1:
            raise PolicyError(f"Windows Conan recipe lock is missing {reference!r}")
    windows_script = Path("scripts/release/build_windows.ps1").read_text(encoding="utf-8")
    for fragment in (
        "--lockfile=release/windows/conan.lock",
        "release/windows/conan-msvc-release.profile",
    ):
        if fragment not in windows_script:
            raise PolicyError(f"Windows build does not consume reviewed lock {fragment!r}")


def check_package_manager_validation(blocks: dict[str, list[str]]) -> None:
    """Require functional package-manager tests before a fresh stable anchor."""

    expected_condition = (
        "${{ needs.contract.outputs.release_kind == 'stable' && "
        "needs.ledger-readiness.outputs.anchor_exists == 'false' }}"
    )
    for job in (
        "validate-homebrew-package",
        "validate-conan-linux",
        "validate-conan-windows",
    ):
        block = "\n".join(blocks[job])
        if scalar_from_block(blocks[job], "if", 4) != expected_condition:
            raise PolicyError(f"package validator {job!r} must run for every fresh stable anchor")
        if "candidate-gate" not in block or "release-candidate-verified-" not in block:
            raise PolicyError(f"package validator {job!r} lacks the verified candidate handoff")
        if re.search(r"needs\.contract\.outputs\.target_", block):
            raise PolicyError(f"package validator {job!r} depends on publication selection")

    homebrew = "\n".join(blocks["validate-homebrew-package"])
    homebrew_script = Path("scripts/release/validate_homebrew_candidate.sh").read_text(
        encoding="utf-8"
    )
    for fragment in (
        "macos-15-intel", "bottle_tag: sequoia", "macos-15",
        "bottle_tag: arm64_sequoia", "ubuntu-24.04",
        "bottle_tag: x86_64_linux", "ubuntu-24.04-arm",
        "bottle_tag: arm64_linux", "validate_homebrew_candidate.sh",
        "brew audit --strict --formula", "brew --cache --build-from-source",
        "brew install --build-from-source", "release.package_validation homebrew-consumer",
    ):
        if fragment not in homebrew + "\n" + homebrew_script:
            raise PolicyError(f"Homebrew package validation is missing {fragment!r}")

    conan_linux = "\n".join(blocks["validate-conan-linux"])
    for fragment in (
        "conan_builder", "release.native_builder run", "--kind conan",
        "conan-linux-x86_64", "conan-validation-linux-", "actions/upload-artifact@",
    ):
        if fragment not in conan_linux:
            raise PolicyError(f"Linux ConanCenter validation is missing {fragment!r}")

    conan_windows = "\n".join(blocks["validate-conan-windows"])
    for fragment in (
        "runs-on: windows-2022", "actions/setup-python@", "ilammy/msvc-dev-cmd@",
        "scripts/release/install_windows_conan.ps1",
        "scripts/release/validate_conan_candidate.ps1",
        "conan-validation-windows-", "actions/upload-artifact@",
    ):
        if fragment not in conan_windows:
            raise PolicyError(f"Windows ConanCenter validation is missing {fragment!r}")

    conan_source = Path("release/conan_validation.py").read_text(encoding="utf-8")
    conan_script = Path("scripts/release/validate_conan_candidate.ps1").read_text(
        encoding="utf-8"
    )
    for fragment in (
        "for shared in (False, True):", "--lockfile=", "dependency_lock_sha256",
        "--lockfile $lockfile",
    ):
        if fragment not in conan_source + "\n" + conan_script:
            raise PolicyError(f"ConanCenter recipe validation is missing {fragment!r}")

    chocolatey = Path("scripts/release/build_windows.ps1").read_text(encoding="utf-8")
    for fragment in (
        "Invoke-ChocolateyCandidateValidation", "release.loopback_archive",
        "release.package_validation', 'chocolatey-adapt",
        "release.package_validation', 'compare-trees",
        "'install', 'mcp-cpp-sdk'", "'uninstall', 'mcp-cpp-sdk'",
        "[EnvironmentVariableTarget]::Machine",
    ):
        if fragment not in chocolatey:
            raise PolicyError(f"Chocolatey package validation is missing {fragment!r}")

    package_gate = "\n".join(blocks["package-validation-gate"])
    for fragment in (
        "always()", "validate-homebrew-package.result", "validate-conan-linux.result",
        "validate-conan-windows.result", "release.workflow_gate packages",
    ):
        if fragment not in package_gate:
            raise PolicyError(f"package-manager result gate is missing {fragment!r}")
    for job in ("validation-complete", "attestation", "github-release", "github-anchor"):
        if "package-validation-gate" not in "\n".join(blocks[job]):
            raise PolicyError(f"release job {job!r} can bypass package-manager validation")


def check_checkout_safety(lines: list[str], blocks: dict[str, list[str]]) -> None:
    text = "\n".join(lines)
    checkout_count = text.count("uses: actions/checkout@")
    if checkout_count < 7:
        raise PolicyError(f"expected protected-source checkouts in construction jobs, found {checkout_count}")
    if text.count("persist-credentials: false") != checkout_count:
        raise PolicyError("every checkout must disable persisted credentials")


def check_embedded_python(lines: list[str]) -> None:
    if any("<<'PY'" in line or "python3 - <<" in line for line in lines):
        raise PolicyError("release workflow must not contain embedded Python programs")
    text = "\n".join(lines)
    if "scripts/run_release_tool.py" not in text:
        raise PolicyError("release workflow must use the isolated tested-tool bootstrap")
    if re.search(r"\bpython3(?:\s+-I\s+-S)?\s+release/", text):
        raise PolicyError("release workflow must invoke release modules through the isolated bootstrap")


def check_required_actionlint() -> None:
    runner = ACTIONLINT_RUNNER_PATH.read_text(encoding="utf-8")
    required_runner_fragments = (
        'ACTIONLINT_VERSION="1.7.12"',
        'ACTIONLINT_SHA256="8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8"',
        "--proto '=https'",
        "sha256sum --check --status",
        "bootstrap",
        "*.yaml",
    )
    for fragment in required_runner_fragments:
        if fragment not in runner:
            raise PolicyError(f"required actionlint runner is missing {fragment!r}")
    for workflow_path in (WORKFLOW_PATH, CI_WORKFLOW_PATH):
        workflow = workflow_path.read_text(encoding="utf-8")
        if workflow.count("run: scripts/release/lint_workflows.sh") != 1:
            raise PolicyError(f"{workflow_path} must require the pinned all-workflow linter")
    if "when preinstalled" in runner or "command -v actionlint" in runner:
        raise PolicyError("actionlint may not be optional")


def check_windows_powershell_syntax_ci(
    ci_source: str | None = None, checker_source: str | None = None
) -> None:
    """Keep release PowerShell syntax validation mandatory on a Windows runner."""

    if ci_source is None:
        ci_source = CI_WORKFLOW_PATH.read_text(encoding="utf-8")
    if checker_source is None:
        checker_source = POWERSHELL_SYNTAX_PATH.read_text(encoding="utf-8")
    required_step = """    - name: Parse release PowerShell
      if: runner.os == 'Windows'
      shell: pwsh
      run: ./scripts/release/check_powershell_syntax.ps1"""
    if ci_source.count(required_step) != 1:
        raise PolicyError("Windows CI must parse every release PowerShell script")
    for fragment in (
        "Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File -Filter '*.ps1'",
        "System.Management.Automation.Language.Parser]::ParseFile(",
        "if ($errors.Count -ne 0)",
    ):
        if fragment not in checker_source:
            raise PolicyError(f"PowerShell syntax checker is missing {fragment!r}")


def main() -> int:
    if not WORKFLOW_PATH.is_file() or not CONTRACT_PATH.is_file():
        raise PolicyError("release workflow or dispatch contract is missing")
    try:
        validate_workflows(workflow_paths())
    except WorkflowYamlError as error:
        raise PolicyError(str(error)) from error
    text = WORKFLOW_PATH.read_text(encoding="utf-8")
    lines = text.splitlines()
    if "\t" in text:
        raise PolicyError("workflow contains tab characters")
    check_trigger(lines)
    check_dispatch_contract(lines)
    check_global_permissions_and_concurrency(lines)
    check_action_pins(lines)
    blocks = job_blocks(lines)
    check_permissions(blocks)
    check_environments(blocks)
    check_privileged_jobs(blocks)
    check_secret_scope(blocks)
    check_credential_rechecks(blocks)
    check_publication_invariants(blocks, text)
    check_fixed_outputs(blocks)
    check_artifact_construction(blocks)
    check_native_targets()
    check_windows_dependency_locks()
    check_package_manager_validation(blocks)
    check_checkout_safety(lines, blocks)
    check_embedded_python(lines)
    check_required_actionlint()
    check_windows_powershell_syntax_ci()
    print(f"release workflow policy passed: {WORKFLOW_PATH}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PolicyError as error:
        print(f"release workflow policy failed: {error}", file=sys.stderr)
        raise SystemExit(1)
