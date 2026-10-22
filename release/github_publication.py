#!/usr/bin/env python3
"""Fail-closed checks for GitHub's release publication boundary."""

from __future__ import annotations

import argparse
from datetime import datetime
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Mapping, Sequence

from .model import SemVer, ValidationError
from .strict_json import load_strict, loads_strict


SHA1 = re.compile(r"[0-9a-f]{40}")
TAG = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-rc\.[1-9][0-9]*)?")
ASSET_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}")
_REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
_GH_RESPONSE_LIMIT = 65536
_API_VERSION = "X-GitHub-Api-Version: 2026-03-10"


class PublicationPolicyError(ValueError):
    """Raised when GitHub state does not prove a safe publication boundary."""


# GraphQL's Release type has no immutability field, so an existing release is read again
# over REST by its ID for that.
_EXISTING_RELEASE_QUERY = """
query($owner:String!,$name:String!,$tag:String!){
  repository(owner:$owner,name:$name){
    databaseId
    nameWithOwner
    release(tagName:$tag){databaseId isDraft isPrerelease tagName}
  }
}
""".strip()


def _mapping(value: object, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise PublicationPolicyError(f"{label} must be a JSON object")
    return value


def _instant(value: object, label: str) -> datetime:
    """Parse one offset-qualified timestamp; GitHub formats it in the caller's zone."""

    if not isinstance(value, str):
        raise PublicationPolicyError(f"{label} must be a timestamp")
    text = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        instant = datetime.fromisoformat(text)
    except ValueError as error:
        raise PublicationPolicyError(f"{label} is malformed") from error
    if instant.utcoffset() is None:
        raise PublicationPolicyError(f"{label} has no UTC offset")
    return instant


def verify_tag_ruleset(
    value: object, *, ruleset_id: int, repository: str, expected_updated_at: str
) -> None:
    """Require one active, non-bypassable ruleset protecting every release tag.

    GitHub returns ``bypass_actors`` only to callers that can write the ruleset, and the
    release reads it with a read-only token. An absent list is accepted because the pinned
    update time changes with any edit, including a new bypass actor, and is recorded only
    after the ruleset was reviewed with write access. A returned list must be empty.
    """

    ruleset = _mapping(value, "tag ruleset")
    rules = ruleset.get("rules")
    expected_instant = _instant(expected_updated_at, "expected ruleset update time")
    if (
        ruleset.get("id") != ruleset_id
        or ruleset.get("target") != "tag"
        or ruleset.get("enforcement") != "active"
        or ruleset.get("source_type") != "Repository"
        or ruleset.get("source") != repository
        or _instant(ruleset.get("updated_at"), "ruleset update time") != expected_instant
        or ruleset.get("bypass_actors", []) != []
        or ruleset.get("conditions")
        != {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}}
        or not isinstance(rules, list)
        or any(not isinstance(rule, Mapping) for rule in rules)
        or sorted(rules, key=lambda rule: str(rule.get("type")))
        != [{"type": "deletion"}, {"type": "update"}]
    ):
        raise PublicationPolicyError("release-tag ruleset is not exact and non-bypassable")


def verify_immutable_releases(value: object) -> None:
    settings = _mapping(value, "immutable Releases settings")
    if (
        set(settings) != {"enabled", "enforced_by_owner"}
        or settings.get("enabled") is not True
        or not isinstance(settings.get("enforced_by_owner"), bool)
    ):
        raise PublicationPolicyError("immutable Releases are not enabled")


def verify_tag_objects(
    ref_value: object,
    tag_value: object,
    *,
    tag: str,
    tag_object_sha: str,
    commit_sha: str,
) -> None:
    """Bind the live GitHub tag ref to the locally verified annotated tag object."""

    if TAG.fullmatch(tag) is None:
        raise PublicationPolicyError("release tag is not canonical")
    ref = _mapping(ref_value, "tag ref")
    ref_target = _mapping(ref.get("object"), "tag ref target")
    tag_object = _mapping(tag_value, "annotated tag object")
    target = _mapping(tag_object.get("object"), "annotated tag target")
    if (
        SHA1.fullmatch(tag_object_sha) is None
        or SHA1.fullmatch(commit_sha) is None
        or ref.get("ref") != f"refs/tags/{tag}"
        or ref_target.get("sha") != tag_object_sha
        or ref_target.get("type") != "tag"
        or tag_object.get("sha") != tag_object_sha
        or tag_object.get("tag") != tag
        or target.get("type") != "commit"
        or target.get("sha") != commit_sha
    ):
        raise PublicationPolicyError(
            "live annotated tag differs from the verified release identity"
        )


def verify_release_identity(
    graphql_value: object,
    rest_value: object,
    *,
    tag: str,
    prerelease: bool,
) -> int:
    """Require the GraphQL/CLI and REST views to identify one immutable release."""

    graphql = _mapping(graphql_value, "GraphQL release")
    rest = _mapping(rest_value, "REST release")
    database_id = graphql.get("databaseId")
    rest_id = rest.get("id")
    if type(database_id) is not int or database_id < 1 or rest_id != database_id:
        raise PublicationPolicyError("GraphQL and REST release IDs disagree")
    expected_graphql = {
        "databaseId": database_id,
        "isDraft": False,
        "isImmutable": True,
        "isPrerelease": prerelease,
        "tagName": tag,
    }
    if dict(graphql) != expected_graphql:
        raise PublicationPolicyError("GraphQL release identity is not exact and immutable")
    if (
        rest.get("tag_name") != tag
        or rest.get("draft") is not False
        or rest.get("prerelease") is not prerelease
        or rest.get("immutable") is not True
    ):
        raise PublicationPolicyError("REST release identity is not exact and immutable")
    return database_id


def classify_existing_release(
    value: object,
    *,
    repository: str,
    repository_id: int,
    tag: str,
    rest_release: object = None,
) -> bool:
    """Return whether an exact immutable anchor exists, rejecting draft residue.

    ``rest_release`` is the REST view of the release that ``value`` names, if it names one.
    """

    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
        raise PublicationPolicyError("repository name is not canonical")
    if type(repository_id) is not int or repository_id < 1 or TAG.fullmatch(tag) is None:
        raise PublicationPolicyError("repository ID or release tag is malformed")
    root = _mapping(value, "GraphQL response")
    if set(root) != {"data"}:
        raise PublicationPolicyError("GraphQL response shape is not exact")
    data = _mapping(root["data"], "GraphQL data")
    if set(data) != {"repository"}:
        raise PublicationPolicyError("GraphQL data shape is not exact")
    repo = _mapping(data["repository"], "GraphQL repository")
    if set(repo) != {"databaseId", "nameWithOwner", "release"}:
        raise PublicationPolicyError("GraphQL repository shape is not exact")
    if repo["databaseId"] != repository_id or repo["nameWithOwner"] != repository:
        raise PublicationPolicyError("GraphQL repository identity differs from dispatch")
    release = repo["release"]
    if release is None:
        return False
    release = _mapping(release, "GraphQL release")
    expected_keys = {"databaseId", "isDraft", "isPrerelease", "tagName"}
    if set(release) != expected_keys:
        raise PublicationPolicyError("existing GraphQL release shape is not exact")
    database_id = release["databaseId"]
    flags = (release["isDraft"], release["isPrerelease"])
    if type(database_id) is not int or database_id < 1 or any(type(flag) is not bool for flag in flags):
        raise PublicationPolicyError("existing GraphQL release fields are malformed")
    if release["tagName"] != tag:
        raise PublicationPolicyError("existing GraphQL release tag differs from dispatch")
    expected_prerelease = "-rc." in tag
    if release["isPrerelease"] is not expected_prerelease:
        raise PublicationPolicyError("existing GitHub release has the wrong prerelease state")
    if release["isDraft"]:
        raise PublicationPolicyError(
            f"BLOCKED_MANUAL_ACTION: draft GitHub release {database_id} exists for {tag}"
        )
    rest = _mapping(rest_release, "existing REST release")
    if (
        rest.get("id") != database_id
        or rest.get("tag_name") != tag
        or rest.get("draft") is not False
        or rest.get("prerelease") is not expected_prerelease
    ):
        raise PublicationPolicyError("existing REST release differs from the GraphQL release")
    if rest.get("immutable") is not True:
        raise PublicationPolicyError("existing GitHub release is published but not immutable")
    return True


def _published_release_id(value: object) -> int | None:
    """Return the ID of the published release a GraphQL response names, without judging it."""

    release: object = value
    for key in ("data", "repository", "release"):
        if not isinstance(release, Mapping):
            return None
        release = release.get(key)
    if not isinstance(release, Mapping) or release.get("isDraft") is not False:
        return None
    database_id = release.get("databaseId")
    if type(database_id) is not int or database_id < 1:
        return None
    return database_id


def probe_existing_release(*, repository: str, repository_id: int, tag: str) -> bool:
    """Read one fixed GraphQL release projection, and the REST release it names."""

    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
        raise PublicationPolicyError("repository name is not canonical")
    owner, name = repository.split("/", 1)
    payload = _gh_json(
        [
            "api", "graphql", "-f", f"query={_EXISTING_RELEASE_QUERY}",
            "-F", f"owner={owner}", "-F", f"name={name}", "-F", f"tag={tag}",
        ],
        label="release-state",
    )
    release_id = _published_release_id(payload)
    rest_release = None
    if release_id is not None:
        rest_release = _gh_json(
            ["api", "-H", _API_VERSION, f"repos/{repository}/releases/{release_id}"],
            label="release-rest",
        )
    return classify_existing_release(
        payload,
        repository=repository,
        repository_id=repository_id,
        tag=tag,
        rest_release=rest_release,
    )


def _gh_json(arguments: Sequence[str], *, label: str, limit: int = _GH_RESPONSE_LIMIT) -> object:
    """Run one fixed GitHub CLI read and parse its size-bounded JSON response."""

    completed = subprocess.run(
        ["gh", *arguments],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=60,
    )
    if completed.returncode != 0:
        raise PublicationPolicyError(f"GitHub {label} query failed")
    if len(completed.stdout.encode("utf-8")) > limit:
        raise PublicationPolicyError(f"GitHub {label} response exceeds the fixed limit")
    try:
        return loads_strict(completed.stdout, label=f"GitHub {label} response")
    except ValidationError as error:
        raise PublicationPolicyError(str(error)) from error


def verify_live_github_tag(
    *, repository: str, tag: str, tag_object_sha: str, commit: str
) -> None:
    """Read back and bind the public annotated tag before a release operation."""

    if _REPOSITORY.fullmatch(repository) is None or TAG.fullmatch(tag) is None:
        raise PublicationPolicyError("repository or release tag is not canonical")
    if SHA1.fullmatch(tag_object_sha) is None or SHA1.fullmatch(commit) is None:
        raise PublicationPolicyError("tag-object or commit identity is malformed")
    ref = _gh_json(
        ["api", f"repos/{repository}/git/ref/tags/{tag}"],
        label="tag-ref",
    )
    tag_object = _gh_json(
        ["api", f"repos/{repository}/git/tags/{tag_object_sha}"],
        label="annotated-tag",
    )
    verify_tag_objects(
        ref,
        tag_object,
        tag=tag,
        tag_object_sha=tag_object_sha,
        commit_sha=commit,
    )


def verify_live_repository_policy(
    *,
    repository: str,
    ruleset_id: int,
    ruleset_updated_at: str,
    tag: str,
    tag_object_sha: str,
    commit: str,
    dry_run: bool = False,
) -> None:
    """Read and verify the live repository release policy, and the tag unless dry-running."""

    if type(ruleset_id) is not int or ruleset_id < 1:
        raise PublicationPolicyError("release-tag ruleset ID is malformed")
    verify_immutable_releases(
        _gh_json(
            ["api", "-H", _API_VERSION, f"repos/{repository}/immutable-releases"],
            label="immutable-releases",
        )
    )
    verify_tag_ruleset(
        _gh_json(
            ["api", "-H", _API_VERSION, f"repos/{repository}/rulesets/{ruleset_id}"],
            label="tag-ruleset",
        ),
        ruleset_id=ruleset_id,
        repository=repository,
        expected_updated_at=ruleset_updated_at,
    )
    if dry_run:
        return
    verify_live_github_tag(
        repository=repository,
        tag=tag,
        tag_object_sha=tag_object_sha,
        commit=commit,
    )


def _release_assets(directory: Path) -> tuple[Path, tuple[Path, ...]]:
    """Return the release notes and the exact regular-file upload inventory."""

    if directory.is_symlink() or not directory.is_dir():
        raise PublicationPolicyError("signed release bundle must be a real directory")
    files = tuple(sorted(directory.iterdir(), key=lambda path: path.name))
    if not files:
        raise PublicationPolicyError("signed release bundle is empty")
    for path in files:
        if path.is_symlink() or not path.is_file() or ASSET_NAME.fullmatch(path.name) is None:
            raise PublicationPolicyError("signed release bundle contains an unsafe entry")
    notes = directory / "RELEASE_NOTES.md"
    if notes not in files:
        raise PublicationPolicyError("signed release bundle lacks RELEASE_NOTES.md")
    try:
        notes_text = notes.read_text(encoding="utf-8")
    except UnicodeError as error:
        raise PublicationPolicyError("release notes are not UTF-8") from error
    if not notes_text.strip() or notes.stat().st_size > 1024 * 1024:
        raise PublicationPolicyError("release notes are empty or exceed the fixed size limit")
    assets = tuple(path for path in files if path != notes)
    if not assets:
        raise PublicationPolicyError("signed release bundle contains no uploadable assets")
    return notes, assets


def create_release(
    *,
    directory: Path,
    repository: str,
    repository_id: int,
    tag: str,
    release_kind: str,
    tag_object_sha: str,
    commit: str,
) -> None:
    """Create a GitHub Release only after exact tag and absence readbacks."""

    try:
        version = SemVer.from_tag(tag)
    except ValidationError as error:
        raise PublicationPolicyError(str(error)) from error
    expected_kind = "rc" if version.is_prerelease else "stable"
    if release_kind != expected_kind:
        raise PublicationPolicyError("release kind differs from the canonical tag")
    if type(repository_id) is not int or repository_id < 1:
        raise PublicationPolicyError("repository ID is malformed")
    notes, assets = _release_assets(directory)
    verify_live_github_tag(
        repository=repository,
        tag=tag,
        tag_object_sha=tag_object_sha,
        commit=commit,
    )
    if probe_existing_release(repository=repository, repository_id=repository_id, tag=tag):
        raise PublicationPolicyError("an immutable release already exists for this tag")

    arguments = [
        "gh",
        "release",
        "create",
        tag,
        *(str(path) for path in assets),
        "--repo",
        repository,
        "--verify-tag",
        "--title",
        f"mcp-cpp-sdk {tag}",
        "--notes-file",
        str(notes),
    ]
    if version.is_prerelease:
        arguments.extend(("--prerelease", "--latest=false"))
    completed = subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=600,
    )
    if completed.returncode != 0:
        raise PublicationPolicyError("GitHub release creation failed")


def verify_anchor_handoff(value: object, *, tag: str, manifest_sha256: str) -> None:
    anchor = _mapping(value, "verified anchor handoff")
    if TAG.fullmatch(tag) is None or re.fullmatch(r"[0-9a-f]{64}", manifest_sha256) is None:
        raise PublicationPolicyError("expected anchor tag or manifest digest is malformed")
    if (
        set(anchor) != {"schema_version", "repository", "release_id", "tag", "commit", "manifest_sha256"}
        or anchor.get("schema_version") != 1
        or not isinstance(anchor.get("repository"), str)
        or re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", anchor["repository"]) is None
        or not isinstance(anchor.get("release_id"), str)
        or re.fullmatch(r"[1-9][0-9]*", anchor["release_id"]) is None
        or not isinstance(anchor.get("commit"), str)
        or SHA1.fullmatch(anchor["commit"]) is None
        or anchor.get("tag") != tag
        or anchor.get("manifest_sha256") != manifest_sha256
    ):
        raise PublicationPolicyError("verified artifact handoff differs from the immutable anchor")


def _read(path: Path) -> object:
    return load_strict(path, label=path.name)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    state = commands.add_parser("verify-state")
    state.add_argument("--ruleset-id", type=int, required=True)
    state.add_argument("--repository", required=True)
    state.add_argument("--ruleset-updated-at", required=True)
    state.add_argument("--tag", required=True)
    state.add_argument("--tag-object-sha", required=True)
    state.add_argument("--commit", required=True)
    state.add_argument("--dry-run", choices=("true", "false"), required=True)

    handoff = commands.add_parser("verify-handoff")
    handoff.add_argument("--anchor", type=Path, required=True)
    handoff.add_argument("--tag", required=True)
    handoff.add_argument("--manifest-sha256", required=True)

    existing = commands.add_parser("probe-existing-release")
    existing.add_argument("--repository", required=True)
    existing.add_argument("--repository-id", type=int, required=True)
    existing.add_argument("--tag", required=True)

    create = commands.add_parser("create-release")
    create.add_argument("--directory", type=Path, required=True)
    create.add_argument("--repository", required=True)
    create.add_argument("--repository-id", type=int, required=True)
    create.add_argument("--tag", required=True)
    create.add_argument("--release-kind", choices=("stable", "rc"), required=True)
    create.add_argument("--tag-object-sha", required=True)
    create.add_argument("--commit", required=True)

    args = parser.parse_args(argv)
    try:
        if args.command == "verify-state":
            verify_live_repository_policy(
                repository=args.repository,
                ruleset_id=args.ruleset_id,
                ruleset_updated_at=args.ruleset_updated_at,
                tag=args.tag,
                tag_object_sha=args.tag_object_sha,
                commit=args.commit,
                dry_run=args.dry_run == "true",
            )
        elif args.command == "verify-handoff":
            verify_anchor_handoff(
                _read(args.anchor), tag=args.tag, manifest_sha256=args.manifest_sha256
            )
        elif args.command == "probe-existing-release":
            print(
                "true" if probe_existing_release(
                    repository=args.repository,
                    repository_id=args.repository_id,
                    tag=args.tag,
                ) else "false"
            )
        else:
            create_release(
                directory=args.directory,
                repository=args.repository,
                repository_id=args.repository_id,
                tag=args.tag,
                release_kind=args.release_kind,
                tag_object_sha=args.tag_object_sha,
                commit=args.commit,
            )
    except (
        OSError,
        UnicodeError,
        subprocess.SubprocessError,
        PublicationPolicyError,
        ValidationError,
    ) as error:
        print(f"github-publication: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
