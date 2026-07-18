#!/usr/bin/env python3
"""Offline structural security checks for the source release workflow."""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path


WORKFLOW_PATH = Path(".github/workflows/release.yml")
CONTRACT_PATH = Path("scripts/release_dispatch_contract.py")
FULL_SHA = re.compile(r"^[0-9a-f]{40}$")
ACTION_ALLOWLIST = {
    "actions/checkout": "9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0",
    "actions/upload-artifact": "043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    "actions/download-artifact": "3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
    "actions/attest": "f7c74d28b9d84cb8768d0b8ca14a4bac6ef463e6",
    "actions/create-github-app-token": "bcd2ba49218906704ab6c1aa796996da409d3eb1",
    "cloudsmith-io/cloudsmith-cli-action": "159f1619275d5d3147f059c3cc110938ec221d16",
}
EXPECTED_JOB_PERMISSIONS = {
    "contract": {"contents": "read"},
    "policy": {"contents": "read"},
    "repository-readiness": {"contents": "read"},
    "ledger-readiness": {"contents": "read", "issues": "read"},
    "immutability-gate": {"contents": "read"},
    "validation-complete": {"contents": "read"},
    "publication-gate": {"contents": "read"},
    "construct-core": {"contents": "read"},
    "build-apt": {"contents": "read"},
    "build-rpm": {"contents": "read"},
    "build-windows": {"contents": "read"},
    "assemble-unsigned": {"contents": "read"},
    "signing": {"contents": "read"},
    "validate-aur-packages": {"contents": "read"},
    "aur-validation-gate": {"contents": "read"},
    "attestation": {"contents": "read", "id-token": "write", "attestations": "write"},
    "github-release": {"contents": "write"},
    "github-anchor": {"contents": "read"},
    "publish-apt": {"contents": "read", "id-token": "write"},
    "publish-rpm": {"contents": "read", "id-token": "write"},
    "aur": {"contents": "read"},
    "homebrew": {"contents": "read"},
    "chocolatey": {"contents": "read"},
    "conan-recipe": {"contents": "read"},
    "conan": {"contents": "read"},
    "ledger-coordinator": {"contents": "read", "issues": "write"},
}
EXPECTED_ENVIRONMENTS = {
    "immutability-gate": "release-admin-read",
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
    "ledger-coordinator": "release-github",
}
PRIVILEGED_JOBS = frozenset(EXPECTED_ENVIRONMENTS)
PUBLISHER_JOBS = ("publish-apt", "publish-rpm", "aur", "homebrew", "chocolatey", "conan")
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
}
FIXED_RESULTS = {
    "SIGNED",
    "ATTESTED",
    "PUBLISHED",
    "SKIPPED_ALREADY_IDENTICAL",
    "SUBMITTED_PENDING_REVIEW",
    "SUBMITTED_PENDING_MODERATION",
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
        "Exact mode:tag:targets:ledger_issue confirmation",
        "vars.RELEASE_EXPECTED_REPOSITORY_ID",
        "vars.RELEASE_EXPECTED_OWNER_ID",
        "run: python3 -I -S scripts/release_dispatch_contract.py",
    ):
        if fragment not in text:
            raise PolicyError(f"dispatch contract is missing {fragment!r}")
    contract = CONTRACT_PATH.read_text(encoding="utf-8")
    for fragment in (
        '"conan2",',
        '"apt",',
        '"rpm",',
        'confirmation must exactly bind mode:tag:targets:ledger_issue',
        'release_kind == "rc" and targets != ("github",)',
        'targets_input != "all"',
        'expected_repository_id=os.environ["EXPECTED_REPOSITORY_ID"]',
    ):
        if fragment not in contract:
            raise PolicyError(f"dispatch validator is missing {fragment!r}")
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
    if writers != ["ledger-coordinator"]:
        raise PolicyError(f"ledger coordinator must be the sole issues writer: {writers}")


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
        "actions/checkout",
        "actions/cache",
        "scripts/",
        "release/verify",
        "release.cli",
        "cmake ",
        "dpkg-buildpackage",
        "rpmbuild",
        "python scripts",
        "choco pack",
    )
    for job in PRIVILEGED_JOBS:
        text = "\n".join(blocks[job])
        for fragment in forbidden:
            if fragment in text:
                raise PolicyError(f"privileged job {job!r} executes forbidden source/tool fragment {fragment!r}")
        if job not in {"immutability-gate", "ledger-coordinator", "github-release"} and "actions/download-artifact@" not in text:
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


def check_publication_invariants(blocks: dict[str, list[str]], text: str) -> None:
    gate = "\n".join(blocks["publication-gate"])
    if 'vars.RELEASE_PUBLISHING_ENABLED' not in text or '!= true' not in gate or "exit 1" not in gate:
        raise PolicyError("publication gate must enforce the exact RELEASE_PUBLISHING_ENABLED=true kill switch")
    ledger_readiness = "\n".join(blocks["ledger-readiness"])
    for fragment in ("state=PREPARING", "release-ledger", "IS_RETRY", "anchor_exists"):
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
    anchor = "\n".join(blocks["github-anchor"])
    for fragment in ("isImmutable", "server-side SHA-256", "release-manifest.json", "ANCHOR.json"):
        if fragment not in anchor:
            raise PolicyError(f"immutable anchor verification is missing {fragment!r}")
    for job in PUBLISHER_JOBS:
        block = "\n".join(blocks[job])
        if "github-anchor" not in block or "Verify fixed anchor handoff" not in block:
            raise PolicyError(f"publisher {job!r} is not bound to the verified GitHub anchor")
    aur = "\n".join(blocks["aur"])
    for fragment in (
        "secrets.AUR_SSH_KEY_PASSPHRASE",
        "SSH_ASKPASS_REQUIRE=force",
        "ssh-add",
        "BatchMode=yes",
        "StrictHostKeyChecking=yes",
        "UserKnownHostsFile=${HOME}/.ssh/known_hosts",
    ):
        if fragment not in aur:
            raise PolicyError(f"AUR publisher is missing passphrase-protected SSH handling {fragment!r}")
    if "ssh-keyscan" in aur or "StrictHostKeyChecking=no" in aur:
        raise PolicyError("AUR publisher weakens SSH host-key verification")
    cloudsmith = "\n".join(blocks["publish-apt"] + blocks["publish-rpm"])
    if "cloudsmith-cli-action@159f1619275d5d3147f059c3cc110938ec221d16" not in cloudsmith:
        raise PolicyError("Cloudsmith action pin is missing")
    if cloudsmith.count('cli-version: "1.19.0"') != 2:
        raise PolicyError("each Cloudsmith adapter must pin CLI version 1.19.0")
    ledger = "\n".join(blocks["ledger-coordinator"])
    if "always()" not in ledger or "release-ledger-json" not in ledger:
        raise PolicyError("ledger coordinator must always produce a complete sanitized snapshot")


def check_fixed_outputs(blocks: dict[str, list[str]]) -> None:
    emitted: set[str] = set()
    for job in PRIVILEGED_JOBS:
        for value in re.findall(r"result=([A-Z_]+)", "\n".join(blocks[job])):
            if value not in FIXED_RESULTS:
                raise PolicyError(f"job {job!r} emits non-approved result {value!r}")
            emitted.add(value)
    required = {"PUBLISHED", "SKIPPED_ALREADY_IDENTICAL", "SUBMITTED_PENDING_REVIEW", "SUBMITTED_PENDING_MODERATION"}
    if not required.issubset(emitted):
        raise PolicyError(f"publisher result coverage is incomplete: {sorted(emitted)}")
    for job in PUBLISHER_JOBS:
        if "result: ${{ steps." not in "\n".join(blocks[job]):
            raise PolicyError(f"publisher {job!r} lacks a sanitized result output")
    if re.search(r"echo\s+['\"]?result=LIVE", "\n".join(sum(blocks.values(), []))):
        raise PolicyError("source workflow must not claim LIVE without provider installation tests")


def check_artifact_construction(blocks: dict[str, list[str]]) -> None:
    for job in ("construct-core", "build-apt", "build-rpm", "build-windows", "assemble-unsigned"):
        block = "\n".join(blocks[job])
        if "actions/upload-artifact@" not in block:
            raise PolicyError(f"artifact construction job {job!r} does not upload its output")
        if job.startswith("build-") and "anchor_exists == 'false'" not in block:
            raise PolicyError(f"artifact build job {job!r} must be skipped for immutable-anchor retries")
    if "self-hosted, release" not in "\n".join(blocks["build-apt"] + blocks["build-rpm"] + blocks["build-windows"]):
        raise PolicyError("native packages must use explicitly labeled release builders")
    aur_validation = "\n".join(blocks["validate-aur-packages"])
    for fragment in ("self-hosted, release", "makepkg --printsrcinfo", "cmp --silent .SRCINFO", "makepkg --verifysource", "makepkg --cleanbuild", "pacman -U", "mcp::sdk_shared", "mcp::sdk_static"):
        if fragment not in aur_validation:
            raise PolicyError(f"AUR package validation is missing {fragment!r}")
    if "validate-aur-packages.result" not in "\n".join(blocks["aur-validation-gate"]):
        raise PolicyError("GitHub publication is not gated on selected AUR validation")
    aur_construction = "\n".join(blocks["construct-core"])
    for fragment in ("\\toptions = !debug", "\\toptions = staticlibs"):
        if fragment not in aur_construction:
            raise PolicyError(f"AUR metadata is missing {fragment!r}")
    apt_construction = "\n".join(blocks["build-apt"])
    for fragment in ('source.rglob("*.in")', "output.parent.mkdir(parents=True, exist_ok=True)"):
        if fragment not in apt_construction:
            raise PolicyError(f"APT package construction is missing {fragment!r}")
    manifest_construction = "\n".join(blocks["assemble-unsigned"])
    for dependency in ("boost", "nlohmann_json", "openssl", "zlib"):
        if f'{{"name": "{dependency}", "minimum":' not in manifest_construction:
            raise PolicyError(f"release manifest omits dependency {dependency!r}")


def check_checkout_safety(lines: list[str], blocks: dict[str, list[str]]) -> None:
    text = "\n".join(lines)
    checkout_count = text.count("uses: actions/checkout@")
    if checkout_count < 7:
        raise PolicyError(f"expected protected-source checkouts in construction jobs, found {checkout_count}")
    if text.count("persist-credentials: false") != checkout_count:
        raise PolicyError("every checkout must disable persisted credentials")
    for job in PRIVILEGED_JOBS:
        if "actions/checkout@" in "\n".join(blocks[job]):
            raise PolicyError(f"privileged job {job!r} must not checkout source")


def check_embedded_python(lines: list[str]) -> None:
    scripts = 0
    index = 0
    while index < len(lines):
        if "<<'PY'" not in lines[index]:
            index += 1
            continue
        indentation = len(lines[index]) - len(lines[index].lstrip(" "))
        terminator = " " * indentation + "PY"
        start = index + 1
        index = start
        while index < len(lines) and lines[index] != terminator:
            index += 1
        if index == len(lines):
            raise PolicyError(f"unterminated Python heredoc at line {start}")
        source = "\n".join(line[indentation:] for line in lines[start:index]) + "\n"
        try:
            ast.parse(source, filename=f"{WORKFLOW_PATH}:{start + 1}")
        except SyntaxError as error:
            raise PolicyError(f"invalid embedded Python at line {start + 1}: {error}") from error
        scripts += 1
        index += 1
    if scripts == 0:
        raise PolicyError("release workflow has no embedded Python to validate")


def main() -> int:
    if not WORKFLOW_PATH.is_file() or not CONTRACT_PATH.is_file():
        raise PolicyError("release workflow or dispatch contract is missing")
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
    check_publication_invariants(blocks, text)
    check_fixed_outputs(blocks)
    check_artifact_construction(blocks)
    check_checkout_safety(lines, blocks)
    check_embedded_python(lines)
    print(f"release workflow policy passed: {WORKFLOW_PATH}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PolicyError as error:
        print(f"release workflow policy failed: {error}", file=sys.stderr)
        raise SystemExit(1)
