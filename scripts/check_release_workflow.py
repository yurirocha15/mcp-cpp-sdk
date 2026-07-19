#!/usr/bin/env python3
"""Fail-closed structural policy for the release workflow.

Behavior belongs in tested release modules.  This checker is intentionally
limited to properties that only the Actions document can express: triggers,
permissions, environments, action pins, dispatch inputs, and job edges.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

from workflow_yaml import WorkflowYamlError, validate_workflows, workflow_paths


WORKFLOW_PATH = Path(".github/workflows/release.yml")
CONTRACT_PATH = Path("scripts/release_dispatch_contract.py")
FULL_SHA = re.compile(r"[0-9a-f]{40}")

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

READ = {"contents": "read"}
EXPECTED_JOB_PERMISSIONS = {
    "contract": READ,
    "policy": READ,
    "repository-readiness": READ,
    "immutability-gate": READ,
    "existing-anchor-policy": READ,
    "fresh-provider-controls": READ,
    "preflight-cloudsmith": {"contents": "read", "id-token": "write"},
    "preflight-aur": READ,
    "preflight-homebrew": READ,
    "preflight-chocolatey": READ,
    "preflight-conan-fork": READ,
    "preflight-conan-broker": READ,
    "preparation-gate": READ,
    "construct-core": READ,
    "build-native": READ,
    "build-windows": READ,
    "build-abi": READ,
    "assemble-unsigned": READ,
    "signing": READ,
    "candidate-gate": READ,
    "validate-homebrew-package": READ,
    "validate-conan-linux": READ,
    "validate-conan-windows": READ,
    "package-validation-gate": READ,
    "validate-aur-packages": READ,
    "validation-complete": READ,
    "attestation": {"contents": "read", "id-token": "write", "attestations": "write"},
    "github-release": {"contents": "write"},
    "github-anchor": READ,
    "publish-cloudsmith": {"contents": "read", "id-token": "write"},
    "aur": READ,
    "homebrew": READ,
    "chocolatey": READ,
    "conan-recipe": READ,
    "conan": READ,
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
    "publish-cloudsmith": "release-cloudsmith",
    "aur": "release-aur",
    "homebrew": "release-homebrew",
    "chocolatey": "release-chocolatey",
    "conan-recipe": "release-conan-recipe",
    "conan": "release-control-dispatch",
}

ALLOWED_SECRETS = {
    "secrets.RELEASE_GPG_PRIVATE_KEY_B64",
    "secrets.RELEASE_GPG_PASSPHRASE",
    "secrets.AUR_SSH_PRIVATE_KEY_B64",
    "secrets.AUR_SSH_KEY_PASSPHRASE",
    "secrets.HOMEBREW_APP_PRIVATE_KEY",
    "secrets.CHOCOLATEY_APP_PRIVATE_KEY",
    "secrets.CONAN_BROKER_APP_PRIVATE_KEY",
    "secrets.CONAN_RECIPE_APP_PRIVATE_KEY",
    "secrets.RELEASE_ADMIN_APP_PRIVATE_KEY",
}

PREFLIGHT_CONDITIONS = {
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

PUBLISHER_CONDITIONS = {
    "publish-cloudsmith": (
        "${{ needs.contract.outputs.mode == 'publish' && "
        "(needs.contract.outputs.target_apt == 'true' || "
        "needs.contract.outputs.target_rpm == 'true') }}"
    ),
    "aur": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_aur == 'true' }}",
    "homebrew": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_homebrew == 'true' }}",
    "chocolatey": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_chocolatey == 'true' }}",
    "conan-recipe": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_conan2 == 'true' }}",
    "conan": "${{ needs.contract.outputs.mode == 'publish' && needs.contract.outputs.target_conan2 == 'true' }}",
}


class PolicyError(RuntimeError):
    """Raised when the Actions document weakens release policy."""


def top_level_block(lines: list[str], key: str) -> list[str]:
    start = next((index for index, line in enumerate(lines) if line == f"{key}:"), None)
    if start is None:
        raise PolicyError(f"missing top-level {key!r} block")
    end = next(
        (index for index in range(start + 1, len(lines)) if lines[index] and not lines[index].startswith(" ")),
        len(lines),
    )
    return lines[start + 1 : end]


def job_blocks(lines: list[str]) -> dict[str, list[str]]:
    jobs = top_level_block(lines, "jobs")
    starts = [
        (index, match.group(1))
        for index, line in enumerate(jobs)
        if (match := re.fullmatch(r"  ([a-z][a-z0-9-]*):", line))
    ]
    blocks: dict[str, list[str]] = {}
    for position, (start, name) in enumerate(starts):
        end = starts[position + 1][0] if position + 1 < len(starts) else len(jobs)
        blocks[name] = jobs[start + 1 : end]
    return blocks


def mapping(block: list[str], key: str) -> dict[str, str]:
    start = next((index for index, line in enumerate(block) if line == f"    {key}:"), None)
    if start is None:
        return {}
    values: dict[str, str] = {}
    for line in block[start + 1 :]:
        if not line.startswith("      ") or line.startswith("        "):
            break
        match = re.fullmatch(r"      ([a-z-]+): ([a-z]+)", line)
        if match is None:
            raise PolicyError(f"job {key!r} mapping contains a non-scalar entry")
        values[match.group(1)] = match.group(2)
    return values


def scalar(block: list[str], key: str) -> str | None:
    prefix = f"    {key}: "
    values = [line[len(prefix) :] for line in block if line.startswith(prefix)]
    if len(values) > 1:
        raise PolicyError(f"job contains multiple {key!r} values")
    return values[0] if values else None


def require_fragments(text: str, fragments: tuple[str, ...], label: str) -> None:
    missing = [fragment for fragment in fragments if fragment not in text]
    if missing:
        raise PolicyError(f"{label} is missing {missing!r}")


def check_trigger_and_dispatch(lines: list[str], text: str) -> None:
    trigger = top_level_block(lines, "on")
    if [line.strip() for line in trigger if line.startswith("  ") and not line.startswith("    ")] != [
        "workflow_dispatch:"
    ]:
        raise PolicyError("release trigger must be workflow_dispatch only")
    forbidden = ("pull_request_target", "repository_dispatch", "workflow_run", "schedule:")
    if any(value in "\n".join(trigger) for value in forbidden):
        raise PolicyError("release workflow contains a forbidden trigger")

    inputs = {
        match.group(1)
        for line in trigger
        if (match := re.fullmatch(r"      ([a-z0-9_]+):", line))
    }
    expected = {
        "tag", "operation", "conan2", "apt", "rpm", "aur", "homebrew",
        "chocolatey", "confirmation",
    }
    if inputs != expected:
        raise PolicyError(f"workflow-dispatch input inventory mismatch: {sorted(inputs)}")
    if text.count("        type: boolean") != 6 or text.count("        default: false") != 6:
        raise PolicyError("all third-party channels must be default-off booleans")
    for operation in ("validate-selected", "validate-all", "publish-selected", "publish-all"):
        if f"          - {operation}" not in text:
            raise PolicyError(f"missing operation {operation!r}")

    contract = CONTRACT_PATH.read_text(encoding="utf-8")
    require_fragments(
        contract,
        (
            '"publish-selected": ("publish", False)',
            '"publish-all": ("publish", True)',
            'value not in {"true", "false"}',
            'confirmation must exactly bind operation:tag:channels',
            'expected_repository_id=os.environ["EXPECTED_REPOSITORY_ID"]',
        ),
        "dispatch validator",
    )
    if "targets_input" in contract or "DISPATCH_TARGETS" in text:
        raise PolicyError("free-form release target strings are forbidden")


def check_action_pins(lines: list[str]) -> None:
    for line in lines:
        match = re.search(r"uses: ([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)@([^\s#]+)", line)
        if match is None:
            continue
        action, revision = match.groups()
        if revision != ACTION_ALLOWLIST.get(action) or FULL_SHA.fullmatch(revision) is None:
            raise PolicyError(f"action {action!r} is not pinned to its reviewed commit")


def check_job_security(blocks: dict[str, list[str]]) -> None:
    if set(blocks) != set(EXPECTED_JOB_PERMISSIONS):
        raise PolicyError("release job inventory differs from reviewed policy")
    found_secrets: set[str] = set()
    for job, block in blocks.items():
        if mapping(block, "permissions") != EXPECTED_JOB_PERMISSIONS[job]:
            raise PolicyError(f"job {job!r} permissions differ from reviewed policy")
        expected_environment = EXPECTED_ENVIRONMENTS.get(job)
        if scalar(block, "environment") != expected_environment:
            raise PolicyError(f"job {job!r} has the wrong protected environment")
        block_text = "\n".join(block)
        secrets = set(re.findall(r"secrets\.[A-Z0-9_]+", block_text))
        if secrets and expected_environment is None:
            raise PolicyError(f"unprotected job {job!r} references a secret")
        found_secrets.update(secrets)

        checkout_indexes = [index for index, line in enumerate(block) if "uses: actions/checkout@" in line]
        for index in checkout_indexes:
            end = next(
                (cursor for cursor in range(index + 1, len(block)) if re.match(r"^      - (?:name|uses):", block[cursor])),
                len(block),
            )
            step = "\n".join(block[index:end])
            if "persist-credentials: false" not in step:
                raise PolicyError(f"job {job!r} persists checkout credentials")
            if expected_environment is not None and "ref: ${{ github.sha }}" not in step:
                raise PolicyError(f"privileged job {job!r} does not pin checkout to github.sha")
        secret_indexes = [index for index, line in enumerate(block) if "secrets." in line]
        if checkout_indexes and secret_indexes and max(checkout_indexes) > min(secret_indexes):
            raise PolicyError(f"privileged job {job!r} checks out code after reading secrets")

    if found_secrets != ALLOWED_SECRETS:
        raise PolicyError(f"secret inventory differs from reviewed policy: {sorted(found_secrets)}")


def check_job_graph(blocks: dict[str, list[str]], text: str) -> None:
    for job, expected in PREFLIGHT_CONDITIONS.items():
        if scalar(blocks[job], "if") != expected:
            raise PolicyError(f"preflight {job!r} is not gated by its checkbox")
    for job, expected in PUBLISHER_CONDITIONS.items():
        block = "\n".join(blocks[job])
        if scalar(blocks[job], "if") != expected:
            raise PolicyError(f"publisher {job!r} is not gated by its checkbox")
        if "      - github-anchor" not in block or "Verify fixed anchor handoff" not in block:
            raise PolicyError(f"publisher {job!r} bypasses the immutable anchor")

    preparation = "\n".join(blocks["preparation-gate"])
    require_fragments(
        preparation,
        (
            "release.provider_preflight gate",
            "release.workflow_gate publishing",
            "vars.RELEASE_PUBLISHING_ENABLED",
            "preflight-cloudsmith",
            "preflight-aur",
            "preflight-homebrew",
            "preflight-chocolatey",
            "preflight-conan-fork",
            "preflight-conan-broker",
        ),
        "preparation gate",
    )
    github_release = "\n".join(blocks["github-release"])
    require_fragments(
        github_release,
        ("candidate-gate", "attestation", "package-validation-gate", "release.github_publication create-release"),
        "GitHub release job",
    )
    anchor = "\n".join(blocks["github-anchor"])
    require_fragments(
        anchor,
        ("preparation-gate", "package-validation-gate", "release.github_anchor download-and-verify"),
        "immutable anchor job",
    )
    if any(value in "\n".join((github_release, anchor)) for value in ("gh release create", "gh release download")):
        raise PolicyError("GitHub release orchestration must remain in tested Python modules")

    fresh_stable = (
        "${{ needs.contract.outputs.release_kind == 'stable' && "
        "needs.repository-readiness.outputs.anchor_exists == 'false' }}"
    )
    for job in ("build-native", "build-windows", "build-abi"):
        if scalar(blocks[job], "if") != fresh_stable:
            raise PolicyError(f"build {job!r} is not required for every fresh stable anchor")
        if re.search(r"needs\.contract\.outputs\.target_", "\n".join(blocks[job])):
            raise PolicyError(f"build {job!r} incorrectly depends on publication selection")
    for job in ("validate-homebrew-package", "validate-conan-linux", "validate-conan-windows", "validate-aur-packages"):
        block = "\n".join(blocks[job])
        if scalar(blocks[job], "if") != fresh_stable or "candidate-gate" not in block:
            raise PolicyError(f"package validator {job!r} can bypass a fresh stable anchor")
    package_gate = "\n".join(blocks["package-validation-gate"])
    require_fragments(
        package_gate,
        ("release.workflow_gate packages", "--aur-result", "validate-aur-packages.result"),
        "package validation gate",
    )
    require_fragments(
        "\n".join(blocks["build-native"]),
        ("fromJSON(needs.repository-readiness.outputs.native_matrix)", '--kind "${FORMAT}"'),
        "native build matrix",
    )
    require_fragments(
        "\n".join(blocks["publish-cloudsmith"]),
        ("fromJSON(needs.contract.outputs.cloudsmith_matrix)", '--format "${{ matrix.format }}"'),
        "Cloudsmith publication matrix",
    )

    forbidden_ledger = ("ledger-readiness", "ledger-coordinator", "release_ledger", "RELEASE_LEDGER_")
    if any(value in text for value in forbidden_ledger):
        raise PolicyError("removed mutable release-ledger machinery reappeared")


def check_delegation_and_ci(blocks: dict[str, list[str]], text: str) -> None:
    required_modules = (
        "release.repository_readiness",
        "release.repository_immutability",
        "release.construct_core",
        "release.assemble_unsigned",
        "scripts/release/sign_release.sh",
        "release.verify_candidate",
        "release.github_publication",
        "release.github_anchor",
        "release.cloudsmith_publish",
        "release.conan_fork prepare",
        "release.conan_fork publish",
        "scripts/release/publish_aur.sh",
        "release.github_provider",
    )
    require_fragments(text, required_modules, "release workflow delegation")
    if re.search(r"python3\s+(?!-I -S scripts/run_release_tool\.py)scripts/", text):
        offenders = [line.strip() for line in text.splitlines() if re.search(r"python3\s+scripts/", line)]
        if offenders != ["python3 scripts/check_release_workflow.py"]:
            raise PolicyError("release workflow directly executes an unreviewed Python module")
    if "python3 - <<" in text or "python3 -c" in text or "@'" in text:
        raise PolicyError("release workflow embeds Python code")
    policy = "\n".join(blocks["policy"])
    require_fragments(
        policy,
        ("scripts/check_release_workflow.py", "unittest discover -s test/release", "lint_workflows.sh"),
        "offline policy job",
    )


def main() -> int:
    if not WORKFLOW_PATH.is_file() or not CONTRACT_PATH.is_file():
        raise PolicyError("release workflow or dispatch validator is missing")
    try:
        validate_workflows(workflow_paths())
    except WorkflowYamlError as error:
        raise PolicyError(str(error)) from error
    text = WORKFLOW_PATH.read_text(encoding="utf-8")
    lines = text.splitlines()
    if "\t" in text:
        raise PolicyError("workflow contains tab characters")
    if [line.strip() for line in top_level_block(lines, "permissions") if line.strip()] != ["contents: read"]:
        raise PolicyError("top-level permissions must be contents: read")
    if [line.strip() for line in top_level_block(lines, "concurrency") if line.strip()] != [
        "group: release-publishing", "cancel-in-progress: false"
    ]:
        raise PolicyError("release runs must be serialized without cancellation")

    check_trigger_and_dispatch(lines, text)
    check_action_pins(lines)
    blocks = job_blocks(lines)
    check_job_security(blocks)
    check_job_graph(blocks, text)
    check_delegation_and_ci(blocks, text)
    print(f"release workflow policy passed: {WORKFLOW_PATH}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PolicyError as error:
        print(f"release workflow policy failed: {error}", file=sys.stderr)
        raise SystemExit(1)
