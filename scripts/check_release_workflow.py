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
PROVIDER_PREFLIGHT_PATH = Path(".github/workflows/release-provider-preflight.yml")
PROVIDER_DISPATCH_PATH = Path(".github/workflows/release-provider-dispatch.yml")
CANDIDATE_PATH = Path(".github/workflows/release-candidate.yml")
PUBLICATION_PATH = Path(".github/workflows/release-publication.yml")
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
MAIN_JOB_PERMISSIONS = {
    "contract": READ,
    "policy": READ,
    "repository-readiness": READ,
    "immutability-gate": READ,
    "existing-anchor-policy": READ,
    "preflight-cloudsmith": READ,
    "preflight-aur": READ,
    "preflight-homebrew": READ,
    "preflight-chocolatey": READ,
    "preflight-conan-broker": READ,
    "preparation-gate": READ,
    "candidate": READ,
    "attestation": {"contents": "read", "id-token": "write", "attestations": "write"},
    "github-release": {"contents": "write"},
    "github-anchor": READ,
    "publication": READ,
    "release-summary": READ,
}

MAIN_ENVIRONMENTS = {
    "immutability-gate": "release-admin-read",
    "preflight-cloudsmith": "release-cloudsmith",
    "preflight-aur": "release-aur",
    "attestation": "release-github",
    "github-release": "release-github",
}

CANDIDATE_JOB_PERMISSIONS = {
    "construct-core": READ,
    "build-native": READ,
    "build-windows": READ,
    "assemble-unsigned": READ,
    "signing": READ,
    "candidate-gate": READ,
    "validate-homebrew-package": READ,
    "validate-conan-linux": READ,
    "validate-conan-windows": READ,
    "package-validation-gate": READ,
    "validate-aur-packages": READ,
}
CANDIDATE_ENVIRONMENTS = {"signing": "release-signing"}

PUBLICATION_JOB_PERMISSIONS = {
    "publish-cloudsmith": READ,
    "aur": READ,
    "homebrew": READ,
    "chocolatey": READ,
    "conan": READ,
    "publication-results": READ,
}
PUBLICATION_ENVIRONMENTS = {
    "publish-cloudsmith": "release-cloudsmith",
    "aur": "release-aur",
}

ALLOWED_SECRETS = {
    "secrets.RELEASE_GPG_PRIVATE_KEY_B64",
    "secrets.RELEASE_GPG_PASSPHRASE",
    "secrets.AUR_SSH_PRIVATE_KEY_B64",
    "secrets.AUR_SSH_KEY_PASSPHRASE",
    "secrets.CLOUDSMITH_API_KEY",
    "secrets.HOMEBREW_APP_PRIVATE_KEY",
    "secrets.CHOCOLATEY_APP_PRIVATE_KEY",
    "secrets.CONAN_BROKER_APP_PRIVATE_KEY",
    "secrets.RELEASE_ADMIN_APP_PRIVATE_KEY",
}

# A reusable workflow receives an environment secret only when its caller passes the secret by
# name, so these calls forward exactly these names; the called jobs hold the environments.
MAIN_FORWARDED_SECRETS = {
    "candidate": frozenset({"RELEASE_GPG_PRIVATE_KEY_B64", "RELEASE_GPG_PASSPHRASE"}),
    "publication": frozenset(
        {
            "AUR_SSH_PRIVATE_KEY_B64",
            "AUR_SSH_KEY_PASSPHRASE",
            "CLOUDSMITH_API_KEY",
            "HOMEBREW_APP_PRIVATE_KEY",
            "CHOCOLATEY_APP_PRIVATE_KEY",
            "CONAN_BROKER_APP_PRIVATE_KEY",
        }
    ),
}
PUBLICATION_FORWARDED_SECRETS = {
    "homebrew": frozenset({"HOMEBREW_APP_PRIVATE_KEY"}),
    "chocolatey": frozenset({"CHOCOLATEY_APP_PRIVATE_KEY"}),
    "conan": frozenset({"CONAN_BROKER_APP_PRIVATE_KEY"}),
}

PREFLIGHT_CONDITIONS = {
    "preflight-cloudsmith": (
        "${{ needs.contract.outputs.release_kind == 'stable' && "
        "(needs.repository-readiness.outputs.anchor_exists == 'false' || "
        "needs.contract.outputs.target_apt == 'true' || "
        "needs.contract.outputs.target_rpm == 'true') }}"
    ),
    "preflight-aur": (
        "${{ needs.contract.outputs.release_kind == 'stable' && "
        "(needs.repository-readiness.outputs.anchor_exists == 'false' || "
        "needs.contract.outputs.target_aur == 'true') }}"
    ),
    "preflight-homebrew": (
        "${{ needs.contract.outputs.release_kind == 'stable' && "
        "(needs.repository-readiness.outputs.anchor_exists == 'false' || "
        "needs.contract.outputs.target_homebrew == 'true') }}"
    ),
    "preflight-chocolatey": (
        "${{ needs.contract.outputs.release_kind == 'stable' && "
        "(needs.repository-readiness.outputs.anchor_exists == 'false' || "
        "needs.contract.outputs.target_chocolatey == 'true') }}"
    ),
    "preflight-conan-broker": (
        "${{ needs.contract.outputs.release_kind == 'stable' && "
        "(needs.repository-readiness.outputs.anchor_exists == 'false' || "
        "needs.contract.outputs.target_conan2 == 'true') }}"
    ),
}

PUBLISHER_CONDITIONS = {
    "publish-cloudsmith": (
        "${{ inputs.mode == 'publish' && "
        "(inputs.target_apt == 'true' || inputs.target_rpm == 'true') }}"
    ),
    "aur": "${{ inputs.mode == 'publish' && inputs.target_aur == 'true' }}",
    "homebrew": "${{ inputs.mode == 'publish' && inputs.target_homebrew == 'true' }}",
    "chocolatey": "${{ inputs.mode == 'publish' && inputs.target_chocolatey == 'true' }}",
    "conan": "${{ inputs.mode == 'publish' && inputs.target_conan2 == 'true' }}",
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


def expression(block: list[str], key: str) -> str | None:
    """Return a scalar or folded job-level expression with normalized whitespace."""

    value = scalar(block, key)
    if value not in {">", ">-", "|", "|-"}:
        return value
    marker = f"    {key}: {value}"
    start = block.index(marker)
    lines: list[str] = []
    for line in block[start + 1 :]:
        if line and not line.startswith("      "):
            break
        if line.strip():
            lines.append(line.strip())
    return " ".join(" ".join(lines).split())


def sequence(block: list[str], key: str) -> tuple[str, ...]:
    """Return a job-level scalar or block sequence."""

    value = scalar(block, key)
    if value is not None:
        return (value,)
    marker = f"    {key}:"
    if marker not in block:
        return ()
    start = block.index(marker)
    values: list[str] = []
    for line in block[start + 1 :]:
        match = re.fullmatch(r"      - ([a-z][a-z0-9-]*)", line)
        if match is None:
            break
        values.append(match.group(1))
    return tuple(values)


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
        "chocolatey",
    }
    if inputs != expected:
        raise PolicyError(f"workflow-dispatch input inventory mismatch: {sorted(inputs)}")
    if text.count("        type: boolean") != 6 or text.count("        default: false") != 6:
        raise PolicyError("all third-party channels must be default-off booleans")
    for operation in ("validate-selected", "validate-all", "publish-selected", "publish-all"):
        if f"          - {operation}" not in text:
            raise PolicyError(f"missing operation {operation!r}")

    contract_text = CONTRACT_PATH.read_text(encoding="utf-8")
    if "targets_input" in contract_text or "DISPATCH_TARGETS" in text:
        raise PolicyError("free-form release target strings are forbidden")


def check_reusable_trigger(lines: list[str], label: str) -> None:
    trigger = top_level_block(lines, "on")
    entries = [
        line.strip()
        for line in trigger
        if line.startswith("  ") and not line.startswith("    ")
    ]
    if entries != ["workflow_call:"]:
        raise PolicyError(f"{label} trigger must be workflow_call only")


def check_action_pins(lines: list[str]) -> None:
    for line in lines:
        match = re.search(r"uses: ([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)@([^\s#]+)", line)
        if match is None:
            continue
        action, revision = match.groups()
        if revision != ACTION_ALLOWLIST.get(action) or FULL_SHA.fullmatch(revision) is None:
            raise PolicyError(f"action {action!r} is not pinned to its reviewed commit")


def forwarded_secrets(block: list[str]) -> tuple[frozenset[str], set[int]]:
    """Return the secrets a reusable-workflow call forwards by name, and their line indexes."""

    if "    secrets:" not in block:
        return frozenset(), set()
    if not any(line.startswith("    uses: ./.github/workflows/") for line in block):
        raise PolicyError("only reusable-workflow calls may forward secrets")
    start = block.index("    secrets:")
    names: set[str] = set()
    indexes = {start}
    for index in range(start + 1, len(block)):
        line = block[index]
        if not line.startswith("      "):
            break
        match = re.fullmatch(r"      ([A-Z0-9_]+): \$\{\{ secrets\.([A-Z0-9_]+) \}\}", line)
        if match is None or match.group(1) != match.group(2):
            raise PolicyError("a reusable-workflow call may forward secrets only under their own names")
        names.add(match.group(1))
        indexes.add(index)
    return frozenset(names), indexes


def check_job_security(
    blocks: dict[str, list[str]],
    expected_permissions: dict[str, dict[str, str]] = MAIN_JOB_PERMISSIONS,
    expected_environments: dict[str, str] = MAIN_ENVIRONMENTS,
    expected_forwarded: dict[str, frozenset[str]] = MAIN_FORWARDED_SECRETS,
) -> None:
    if set(blocks) != set(expected_permissions):
        raise PolicyError("release job inventory differs from reviewed policy")
    for job, block in blocks.items():
        if mapping(block, "permissions") != expected_permissions[job]:
            raise PolicyError(f"job {job!r} permissions differ from reviewed policy")
        expected_environment = expected_environments.get(job)
        if scalar(block, "environment") != expected_environment:
            raise PolicyError(f"job {job!r} has the wrong protected environment")
        forwarded, forwarding_lines = forwarded_secrets(block)
        if forwarded != expected_forwarded.get(job, frozenset()):
            raise PolicyError(f"job {job!r} forwards secrets that differ from reviewed policy")
        if not {f"secrets.{name}" for name in forwarded} <= ALLOWED_SECRETS:
            raise PolicyError(f"job {job!r} forwards an unreviewed secret")
        block_text = "\n".join(line for index, line in enumerate(block) if index not in forwarding_lines)
        secrets = set(re.findall(r"secrets\.[A-Z0-9_]+", block_text))
        if not secrets <= ALLOWED_SECRETS:
            raise PolicyError(f"job {job!r} references an unreviewed secret")
        if secrets and expected_environment is None:
            raise PolicyError(f"unprotected job {job!r} references a secret")

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


def check_job_graph(
    blocks: dict[str, list[str]],
    text: str,
    candidate_blocks: dict[str, list[str]] | None = None,
    publication_blocks: dict[str, list[str]] | None = None,
) -> None:
    if candidate_blocks is None:
        candidate_blocks = job_blocks(CANDIDATE_PATH.read_text(encoding="utf-8").splitlines())
    if publication_blocks is None:
        publication_blocks = job_blocks(PUBLICATION_PATH.read_text(encoding="utf-8").splitlines())

    for job, expected in PREFLIGHT_CONDITIONS.items():
        if expression(blocks[job], "if") != expected:
            raise PolicyError(
                f"preflight {job!r} does not cover fresh anchors and selected resumes"
            )

    candidate_call = "\n".join(blocks["candidate"])
    if expression(blocks["candidate"], "if") != (
        "${{ always() && needs.preparation-gate.result == 'success' }}"
    ):
        raise PolicyError("candidate workflow call does not admit reviewed skipped preflights")
    if not {"preparation-gate", "existing-anchor-policy"} <= set(
        sequence(blocks["candidate"], "needs")
    ):
        raise PolicyError("candidate workflow call bypasses preparation policy")
    require_fragments(
        candidate_call,
        (
            "./.github/workflows/release-candidate.yml",
        ),
        "candidate workflow call",
    )
    publication_call = "\n".join(blocks["publication"])
    if "github-anchor" not in sequence(blocks["publication"], "needs"):
        raise PolicyError("publication workflow call bypasses the immutable anchor")
    require_fragments(
        publication_call,
        (
            "./.github/workflows/release-publication.yml",
        ),
        "publication workflow call",
    )

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
            "preflight-conan-broker",
        ),
        "preparation gate",
    )
    github_release = "\n".join(blocks["github-release"])
    if not {"candidate", "attestation"} <= set(
        sequence(blocks["github-release"], "needs")
    ):
        raise PolicyError("GitHub release job bypasses candidate validation or attestation")
    require_fragments(
        github_release,
        ("release.github_publication create-release",),
        "GitHub release job",
    )
    anchor = "\n".join(blocks["github-anchor"])
    if not {
        "existing-anchor-policy",
        "preparation-gate",
        "candidate",
        "github-release",
    } <= set(sequence(blocks["github-anchor"], "needs")):
        raise PolicyError("immutable anchor job bypasses release policy")
    require_fragments(
        anchor,
        (
            "release.github_anchor download-and-verify",
            "Download the already verified historical anchor",
        ),
        "immutable anchor job",
    )
    if any(
        value in "\n".join((github_release, anchor))
        for value in ("gh release create", "gh release download")
    ):
        raise PolicyError("GitHub release orchestration must remain in tested Python modules")

    fresh_stable = (
        "${{ inputs.release_kind == 'stable' && inputs.anchor_exists == 'false' }}"
    )
    for job in ("build-native", "build-windows"):
        if scalar(candidate_blocks[job], "if") != fresh_stable:
            raise PolicyError(f"build {job!r} is not required for every fresh stable anchor")
        if re.search(r"inputs\.target_", "\n".join(candidate_blocks[job])):
            raise PolicyError(f"build {job!r} incorrectly depends on publication selection")
    for job in (
        "validate-homebrew-package",
        "validate-conan-linux",
        "validate-conan-windows",
        "validate-aur-packages",
    ):
        block = "\n".join(candidate_blocks[job])
        if scalar(candidate_blocks[job], "if") != fresh_stable or "candidate-gate" not in block:
            raise PolicyError(f"package validator {job!r} can bypass a fresh stable anchor")
    package_gate = "\n".join(candidate_blocks["package-validation-gate"])
    require_fragments(
        package_gate,
        ("release.workflow_gate packages", "--aur-result", "validate-aur-packages.result"),
        "package validation gate",
    )
    require_fragments(
        "\n".join(candidate_blocks["build-native"]),
        ("fromJSON(inputs.native_matrix)", '--kind "${FORMAT}"'),
        "native build matrix",
    )

    for job, expected in PUBLISHER_CONDITIONS.items():
        block = "\n".join(publication_blocks[job])
        if expression(publication_blocks[job], "if") != expected:
            raise PolicyError(f"publisher {job!r} is not gated by its checkbox")
        if job in {"homebrew", "chocolatey", "conan"}:
            if scalar(publication_blocks[job], "uses") != (
                "./.github/workflows/release-provider-dispatch.yml"
            ):
                raise PolicyError(
                    f"publisher {job!r} bypasses the reviewed reusable dispatcher"
                )
        elif "release.github_publication verify-handoff" not in block:
            raise PolicyError(
                f"publisher {job!r} does not verify the immutable anchor handoff"
            )
    require_fragments(
        "\n".join(publication_blocks["publish-cloudsmith"]),
        ("fromJSON(inputs.cloudsmith_matrix)", '--format "${{ matrix.format }}"'),
        "Cloudsmith publication matrix",
    )
    require_fragments(
        "\n".join(publication_blocks["publication-results"]),
        ("publish-cloudsmith", "aur", "homebrew", "chocolatey", "conan"),
        "publication result collector",
    )

    forbidden_ledger = (
        "ledger-readiness",
        "ledger-coordinator",
        "release_ledger",
        "RELEASE_LEDGER_",
    )
    if any(value in text for value in forbidden_ledger):
        raise PolicyError("removed mutable release-ledger machinery reappeared")


def check_provider_workflows(
    main_blocks: dict[str, list[str]],
    publication_blocks: dict[str, list[str]],
    combined_text: str,
) -> None:
    if "secrets: inherit" in combined_text:
        raise PolicyError("release workflows must not inherit unrelated secrets")
    preflight_text = PROVIDER_PREFLIGHT_PATH.read_text(encoding="utf-8")
    dispatch_text = PROVIDER_DISPATCH_PATH.read_text(encoding="utf-8")
    preflight_blocks = job_blocks(preflight_text.splitlines())
    dispatch_blocks = job_blocks(dispatch_text.splitlines())
    if set(preflight_blocks) != {"preflight"}:
        raise PolicyError("provider preflight job inventory differs from reviewed policy")
    if set(dispatch_blocks) != {"dispatch"}:
        raise PolicyError("provider dispatch job inventory differs from reviewed policy")

    check_job_security(
        preflight_blocks,
        {"preflight": READ},
        {"preflight": "${{ inputs.environment }}"},
    )
    check_job_security(
        dispatch_blocks,
        {"dispatch": READ},
        {"dispatch": "${{ inputs.environment }}"},
    )
    for label, provider_blocks in (
        ("provider preflight", preflight_blocks),
        ("provider dispatch", dispatch_blocks),
    ):
        block = next(iter(provider_blocks.values()))
        require_fragments(
            "\n".join(block),
            ('case "${PROVIDER}:${PROVIDER_ENVIRONMENT}" in',),
            label,
        )
    require_fragments(
        preflight_text,
        (
            "homebrew:release-homebrew",
            "chocolatey:release-chocolatey",
            "conan:release-control-dispatch",
            "release.provider_preflight workflow",
        ),
        "provider preflight workflow",
    )
    require_fragments(
        dispatch_text,
        (
            "homebrew:release-homebrew",
            "chocolatey:release-chocolatey",
            "conan2:release-control-dispatch",
            "actions/create-github-app-token@",
            "release.github_publication verify-handoff",
            "release.github_provider",
        ),
        "provider dispatch workflow",
    )
    for job, provider, environment in (
        ("preflight-homebrew", "homebrew", "release-homebrew"),
        ("preflight-chocolatey", "chocolatey", "release-chocolatey"),
        ("preflight-conan-broker", "conan", "release-control-dispatch"),
    ):
        require_fragments(
            "\n".join(main_blocks[job]),
            (f"provider: {provider}", f"environment: {environment}"),
            f"provider caller {job!r}",
        )
    for job, provider, environment in (
        ("homebrew", "homebrew", "release-homebrew"),
        ("chocolatey", "chocolatey", "release-chocolatey"),
        ("conan", "conan2", "release-control-dispatch"),
    ):
        require_fragments(
            "\n".join(publication_blocks[job]),
            (f"provider: {provider}", f"environment: {environment}"),
            f"provider caller {job!r}",
        )
    secrets = set(
        re.findall(
            r"secrets\.[A-Z0-9_]+",
            "\n".join((combined_text, preflight_text, dispatch_text)),
        )
    )
    if secrets != ALLOWED_SECRETS:
        raise PolicyError(f"secret inventory differs from reviewed policy: {sorted(secrets)}")


def check_delegation_and_ci(blocks: dict[str, list[str]], text: str) -> None:
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
    required_paths = (
        WORKFLOW_PATH,
        PROVIDER_PREFLIGHT_PATH,
        PROVIDER_DISPATCH_PATH,
        CANDIDATE_PATH,
        PUBLICATION_PATH,
        CONTRACT_PATH,
    )
    if any(not path.is_file() for path in required_paths):
        raise PolicyError("release workflow, reusable provider workflow, or validator is missing")
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
    release_workflow_lines: list[str] = []
    for path in sorted(WORKFLOW_PATH.parent.glob("release*.yml")):
        release_workflow_lines.extend(path.read_text(encoding="utf-8").splitlines())
    check_action_pins(release_workflow_lines)
    candidate_text = CANDIDATE_PATH.read_text(encoding="utf-8")
    publication_text = PUBLICATION_PATH.read_text(encoding="utf-8")
    provider_preflight_text = PROVIDER_PREFLIGHT_PATH.read_text(encoding="utf-8")
    provider_dispatch_text = PROVIDER_DISPATCH_PATH.read_text(encoding="utf-8")
    candidate_blocks = job_blocks(candidate_text.splitlines())
    publication_blocks = job_blocks(publication_text.splitlines())
    for path, reusable_text in (
        (CANDIDATE_PATH, candidate_text),
        (PUBLICATION_PATH, publication_text),
        (PROVIDER_PREFLIGHT_PATH, provider_preflight_text),
        (PROVIDER_DISPATCH_PATH, provider_dispatch_text),
    ):
        check_reusable_trigger(reusable_text.splitlines(), str(path))
    combined_text = "\n".join(
        (
            text,
            candidate_text,
            publication_text,
            provider_preflight_text,
            provider_dispatch_text,
        )
    )

    blocks = job_blocks(lines)
    check_job_security(blocks)
    check_job_security(
        candidate_blocks,
        CANDIDATE_JOB_PERMISSIONS,
        CANDIDATE_ENVIRONMENTS,
        {},
    )
    check_job_security(
        publication_blocks,
        PUBLICATION_JOB_PERMISSIONS,
        PUBLICATION_ENVIRONMENTS,
        PUBLICATION_FORWARDED_SECRETS,
    )
    check_job_graph(blocks, combined_text, candidate_blocks, publication_blocks)
    check_provider_workflows(blocks, publication_blocks, combined_text)
    check_delegation_and_ci(blocks, combined_text)
    print(f"release workflow policy passed: {WORKFLOW_PATH}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PolicyError as error:
        print(f"release workflow policy failed: {error}", file=sys.stderr)
        raise SystemExit(1)
