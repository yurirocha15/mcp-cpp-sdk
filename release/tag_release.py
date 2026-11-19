#!/usr/bin/env python3
"""Sign the release tag for main's VERSION, check it, push it, and start the release.

Run it from a clone of the repository, with the release keyring and an
authenticated GitHub CLI:

    python3 -m release.tag_release --gnupghome ~/path/to/release-keyring

It signs ``vVERSION`` on the head of the remote's main with the active tag
subkey from ``release/trusted-release-signers.json`` (gpg asks for the
passphrase), verifies the signature, and runs the same readiness check as the
release workflow. A stable release also needs a successful dry run of that
commit, and publishes to the channels the RELEASE_CHANNELS repository variable
names. Pushing the tag and dispatching the release each ask for a yes first.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Callable, Mapping, Sequence

from .model import SemVer, ValidationError

SIGNERS = "release/trusted-release-signers.json"
# The channels of the release workflow's dispatch inputs (scripts/release_dispatch_contract.py).
CHANNELS = ("conan2", "apt", "rpm", "aur", "homebrew", "chocolatey")
REPOSITORY_FILES = (
    "--targets",
    "packaging/targets.json",
    "--native-builder-lock",
    "release/native-builders/lock.json",
    "--trusted-signers",
    SIGNERS,
)


class TagReleaseError(RuntimeError):
    """Raised when the release cannot be tagged as checked."""


def run(
    *arguments: str,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    capture: bool = True,
) -> str:
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        env=None if env is None else {**os.environ, **env},
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip() if capture else ""
        raise TagReleaseError(f"{' '.join(arguments[:3])} failed: {detail}".rstrip(": "))
    return completed.stdout if capture else ""


def active_signer(signers_json: str) -> dict[str, str]:
    """Return the fingerprints and key path of the active release signer."""

    document = json.loads(signers_json)
    active = document.get("active_signer_id")
    for signer in document.get("signers", []):
        if signer.get("id") == active:
            fields = (
                "public_key",
                "primary_fingerprint",
                "tag_subkey_fingerprint",
                "artifact_subkey_fingerprint",
            )
            if all(isinstance(signer.get(field), str) for field in fields):
                return {field: signer[field] for field in fields}
    raise TagReleaseError(f"{SIGNERS} has no complete active signer")


def signature_fingerprints(status: str) -> tuple[str, str] | None:
    """Return the (signing key, primary key) of a good signature in gpg status output."""

    for line in status.splitlines():
        fields = line.split()
        if len(fields) >= 12 and fields[:2] == ["[GNUPG:]", "VALIDSIG"]:
            return fields[2], fields[11]
    return None


def successful_dry_run(runs_json: str, tag: str, commit: str) -> str | None:
    """Return the URL of a successful dry run of ``tag`` on ``commit``, if there is one."""

    for entry in json.loads(runs_json):
        if (
            entry.get("displayTitle") == f"release {tag} (dry-run)"
            and entry.get("headSha") == commit
            and entry.get("conclusion") == "success"
        ):
            return entry.get("url")
    return None


def release_dispatch(version: SemVer, channels_variable: str | None) -> list[str]:
    """Return the release workflow inputs that publish ``version`` to the chosen channels.

    ``channels_variable`` is the RELEASE_CHANNELS repository variable: the channels a
    stable release publishes to, separated by spaces.
    """

    if version.is_prerelease:
        # A release candidate publishes only the GitHub Release.
        return ["-f", f"tag={version.tag}", "-f", "operation=publish-selected"]
    if channels_variable is None or not channels_variable.split():
        raise TagReleaseError(
            "set the RELEASE_CHANNELS repository variable to the channels to publish, such as: "
            f"gh variable set RELEASE_CHANNELS --body '{' '.join(CHANNELS)}'"
        )
    channels = channels_variable.split()
    unknown = sorted(set(channels) - set(CHANNELS))
    if unknown or len(set(channels)) != len(channels):
        raise TagReleaseError(
            f"RELEASE_CHANNELS names unknown or repeated channels {unknown or channels}; "
            f"the channels are {' '.join(CHANNELS)}"
        )
    inputs = ["-f", f"tag={version.tag}", "-f", "operation=publish-selected"]
    for channel in CHANNELS:
        if channel in channels:
            inputs += ["-f", f"{channel}=true"]
    return inputs


def confirm(question: str, answer: Callable[[str], str] = input) -> bool:
    return answer(f"{question} [y/N] ").strip().lower() in {"y", "yes"}


def _readiness(commit: str, tag: str, version: SemVer, signer: Mapping[str, str]) -> None:
    worktree = Path(tempfile.mkdtemp(prefix="mcp-release-readiness-"))
    try:
        run("git", "worktree", "add", "--detach", "--force", str(worktree / "tree"), commit)
        tree = worktree / "tree"
        output = worktree / "readiness-output.txt"
        run(
            sys.executable,
            "-I",
            "-S",
            "scripts/run_release_tool.py",
            "release.repository_readiness",
            "--tag",
            tag,
            "--version",
            str(version),
            "--dispatch-head-sha",
            commit,
            "--anchor-exists",
            "false",
            "--dry-run",
            "false",
            "--primary-fingerprint",
            signer["primary_fingerprint"],
            "--tag-fingerprint",
            signer["tag_subkey_fingerprint"],
            "--artifact-fingerprint",
            signer["artifact_subkey_fingerprint"],
            "--public-key",
            signer["public_key"],
            *REPOSITORY_FILES,
            "--github-output",
            str(output),
            cwd=tree,
        )
    finally:
        subprocess.run(
            ("git", "worktree", "remove", "--force", str(worktree / "tree")),
            check=False,
            capture_output=True,
        )
        shutil.rmtree(worktree, ignore_errors=True)


def _repository_variable(name: str) -> str | None:
    completed = subprocess.run(
        ("gh", "variable", "get", name), check=False, capture_output=True, text=True
    )
    return completed.stdout.strip() if completed.returncode == 0 else None


def tag_release(args: argparse.Namespace, answer: Callable[[str], str] = input) -> int:
    remote = args.remote
    gpg_env = {"GNUPGHOME": str(args.gnupghome.expanduser())} if args.gnupghome else {}

    run("git", "fetch", "--quiet", remote, "main")
    commit = run("git", "rev-parse", "FETCH_HEAD").strip()
    try:
        version = SemVer.parse(run("git", "show", f"{commit}:VERSION").strip())
    except ValidationError as error:
        raise TagReleaseError(f"VERSION on {remote}/main is not a release version: {error}") from error
    tag = version.tag
    print(f"{remote}/main is {commit}; VERSION is {version}")

    if run("git", "ls-remote", "--tags", remote, f"refs/tags/{tag}").strip():
        raise TagReleaseError(f"{tag} already exists on {remote}")
    # A dry run validates every channel, which a release candidate does not publish to.
    if not version.is_prerelease and not args.without_dry_run:
        runs = run(
            "gh", "run", "list", "--workflow", "release.yml", "--branch", "main",
            "--event", "workflow_dispatch", "--limit", "100",
            "--json", "displayTitle,headSha,conclusion,url",
        )
        dry_run = successful_dry_run(runs, tag, commit)
        if dry_run is None:
            raise TagReleaseError(
                f"no successful dry run of {tag} on {commit}; approve the one that started "
                "when VERSION changed, or start one with: gh workflow run release.yml --ref main "
                f"-f tag={tag} -f operation=dry-run"
            )
        print(f"dry run passed: {dry_run}")

    signer = active_signer(run("git", "show", f"{commit}:{SIGNERS}"))
    tag_key = signer["tag_subkey_fingerprint"]
    if run("git", "tag", "--list", tag).strip():
        if run("git", "rev-parse", f"{tag}^{{commit}}").strip() != commit:
            raise TagReleaseError(f"the local {tag} points elsewhere; delete it with: git tag -d {tag}")
        print(f"using the existing local {tag}")
    else:
        print(f"signing {tag} with {tag_key}; gpg asks for the release key passphrase")
        run(
            "git", "tag", "-s", "-u", f"{tag_key}!", "-m", f"mcp-cpp-sdk {version}", tag, commit,
            env=gpg_env, capture=False,
        )
    status = subprocess.run(
        ("git", "verify-tag", "--raw", tag),
        env={**os.environ, **gpg_env},
        check=False,
        capture_output=True,
        text=True,
    )
    if signature_fingerprints(status.stderr) != (tag_key, signer["primary_fingerprint"]):
        raise TagReleaseError(f"{tag} does not carry a good signature by {tag_key}")
    print(f"{tag} is signed by {tag_key}")

    _readiness(commit, tag, version, signer)
    print("release readiness passed")
    # Checked before the push, so a missing or wrong variable stops before the tag is public.
    dispatch = release_dispatch(version, _repository_variable("RELEASE_CHANNELS"))
    channels = [value.removesuffix("=true") for value in dispatch[5::2]] or ["the GitHub Release only"]

    if not confirm(f"Push {tag} to {remote}? It cannot be moved or deleted there.", answer):
        print(f"not pushed; push later with: git push {remote} refs/tags/{tag}")
        return 0
    run("git", "fetch", "--quiet", remote, "main")
    if run("git", "rev-parse", "FETCH_HEAD").strip() != commit:
        raise TagReleaseError(f"{remote}/main moved; the tag must point at its head, so nothing was pushed")
    run("git", "push", remote, f"refs/tags/{tag}", capture=False)

    if not confirm(f"Start the release of {tag} to {', '.join(channels)}?", answer):
        print(f"not started; start later with: gh workflow run release.yml --ref main {' '.join(dispatch)}")
        return 0
    run("gh", "workflow", "run", "release.yml", "--ref", "main", *dispatch, capture=False)
    print("approve the run, then the publication, at https://github.com/" + re.sub(
        r"^.*github\.com[:/]|\.git$", "", run("git", "remote", "get-url", remote).strip()
    ) + "/actions/workflows/release.yml")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--gnupghome",
        type=Path,
        default=os.environ.get("MCP_RELEASE_GNUPGHOME"),
        help="the keyring holding the tag subkey (default: $MCP_RELEASE_GNUPGHOME, else gpg's own)",
    )
    parser.add_argument("--remote", default="origin")
    parser.add_argument(
        "--without-dry-run",
        action="store_true",
        help="tag a stable release that has no successful dry run on the commit",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.gnupghome is not None:
        args.gnupghome = Path(args.gnupghome)
    try:
        return tag_release(args)
    except (OSError, ValueError, TagReleaseError) as error:
        raise SystemExit(f"tag-release: {error}") from error


if __name__ == "__main__":
    raise SystemExit(main())
