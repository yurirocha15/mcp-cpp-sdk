"""Verify the live GitHub tag and repository immutability controls."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
from typing import Any, Mapping, Sequence

from .model import ValidationError


_SHA_RE = re.compile(r"[0-9a-f]{40}")


def _object(path: Path) -> Mapping[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValidationError(f"{path} must contain a JSON object")
    return value


def verify_immutable_releases(value: Mapping[str, Any]) -> None:
    if set(value) != {"enabled", "enforced_by_owner"}:
        raise ValidationError("immutable Releases response schema is unexpected")
    if value["enabled"] is not True or not isinstance(value["enforced_by_owner"], bool):
        raise ValidationError("immutable Releases are not enabled")


def verify_tag_ruleset(
    value: Mapping[str, Any], *, ruleset_id: int, updated_at: str, repository: str
) -> None:
    if (
        value.get("id") != ruleset_id
        or value.get("target") != "tag"
        or value.get("enforcement") != "active"
        or value.get("source_type") != "Repository"
        or value.get("source") != repository
        or value.get("updated_at") != updated_at
    ):
        raise ValidationError("release-tag ruleset identity or enforcement is unsafe")
    if value.get("bypass_actors") != []:
        raise ValidationError("release-tag ruleset has bypass actors")
    conditions = value.get("conditions")
    if not isinstance(conditions, dict) or set(conditions) != {"ref_name"}:
        raise ValidationError("release-tag ruleset conditions are not exact")
    if conditions["ref_name"] != {"include": ["refs/tags/v*"], "exclude": []}:
        raise ValidationError("release-tag ruleset does not exactly cover v* tags")
    rules = value.get("rules")
    if not isinstance(rules, list) or any(not isinstance(rule, dict) for rule in rules):
        raise ValidationError("release-tag ruleset rules are malformed")
    if sorted(rules, key=lambda rule: str(rule.get("type"))) != [
        {"type": "deletion"},
        {"type": "update"},
    ]:
        raise ValidationError("release-tag ruleset must contain only deletion and update protection")


def verify_live_tag(
    ref: Mapping[str, Any],
    tag_object: Mapping[str, Any],
    *,
    tag: str,
    tag_object_sha: str,
    commit: str,
) -> None:
    if _SHA_RE.fullmatch(tag_object_sha) is None or _SHA_RE.fullmatch(commit) is None:
        raise ValidationError("expected tag or commit identity is malformed")
    target = tag_object.get("object")
    if (
        ref.get("ref") != f"refs/tags/{tag}"
        or ref.get("object") != {"sha": tag_object_sha, "type": "tag"}
        or tag_object.get("sha") != tag_object_sha
        or tag_object.get("tag") != tag
        or not isinstance(target, dict)
        or target.get("type") != "commit"
        or target.get("sha") != commit
    ):
        raise ValidationError("live annotated tag differs from the verified release identity")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--immutable-releases", type=Path, required=True)
    parser.add_argument("--ruleset", type=Path, required=True)
    parser.add_argument("--tag-ref", type=Path, required=True)
    parser.add_argument("--tag-object", type=Path, required=True)
    parser.add_argument("--ruleset-id", type=int, required=True)
    parser.add_argument("--ruleset-updated-at", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--tag-object-sha", required=True)
    parser.add_argument("--commit", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        verify_immutable_releases(_object(args.immutable_releases))
        verify_tag_ruleset(
            _object(args.ruleset),
            ruleset_id=args.ruleset_id,
            updated_at=args.ruleset_updated_at,
            repository=args.repository,
        )
        verify_live_tag(
            _object(args.tag_ref),
            _object(args.tag_object),
            tag=args.tag,
            tag_object_sha=args.tag_object_sha,
            commit=args.commit,
        )
    except (OSError, json.JSONDecodeError, ValidationError) as error:
        raise SystemExit(f"repository-immutability: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
