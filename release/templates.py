"""Strict rendering of reviewed package templates."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Mapping
from urllib.parse import urlsplit
from xml.sax.saxutils import escape as xml_escape

from .model import ValidationError


_TOKEN_RE = re.compile(r"@([A-Z][A-Z0-9_]*)@")
_SEMVER_RE = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_FINGERPRINT_RE = re.compile(r"[0-9A-F]{40}|[0-9A-F]{64}")
_PACKAGE_WORD_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9 .,+_()/&:;'#-]*")
_DEBIAN_MAINTAINER_RE = re.compile(
    r"[A-Za-z0-9][A-Za-z0-9 .,'()+&_-]* <[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+>"
)
_SAFE_URL_CHARACTERS = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?&=%+"
)
_XML_VALUE_NAMES = frozenset(
    {"AUTHORS", "OWNERS", "SUMMARY", "DESCRIPTION", "COPYRIGHT", "TAGS"}
)


def _validate_url(name: str, value: str) -> None:
    if any(character not in _SAFE_URL_CHARACTERS for character in value):
        raise ValidationError(f"template value {name} contains an unsafe URL character")
    parsed = urlsplit(value)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
        raise ValidationError(f"template value {name} must be an absolute credential-free HTTPS URL")
    if parsed.fragment:
        raise ValidationError(f"template value {name} must not contain a URL fragment")


def _validate_known_value(name: str, value: str) -> None:
    if name == "VERSION" and _SEMVER_RE.fullmatch(value) is None:
        raise ValidationError("VERSION must be a stable canonical SemVer")
    if name == "DEBIAN_VERSION" and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+-[1-9][0-9]*", value) is None:
        raise ValidationError("DEBIAN_VERSION is not canonical")
    if name == "DEBIAN_DISTRIBUTION" and re.fullmatch(r"[a-z][a-z0-9-]{0,31}", value) is None:
        raise ValidationError("DEBIAN_DISTRIBUTION is not canonical")
    if name == "RPM_VERSION" and _SEMVER_RE.fullmatch(value) is None:
        raise ValidationError("RPM_VERSION is not canonical")
    if name == "RPM_RELEASE" and re.fullmatch(r"[1-9][0-9]*", value) is None:
        raise ValidationError("RPM_RELEASE is not canonical")
    if name == "PKGREL" and re.fullmatch(r"[1-9][0-9]*", value) is None:
        raise ValidationError("PKGREL is not canonical")
    if name.endswith("SHA256") and _SHA256_RE.fullmatch(value) is None:
        raise ValidationError(f"template value {name} must be lowercase SHA-256")
    if name.endswith("FINGERPRINT") and _FINGERPRINT_RE.fullmatch(value) is None:
        raise ValidationError(f"template value {name} must be a full uppercase fingerprint")
    if name.endswith("_URL"):
        _validate_url(name, value)
    if name == "MAINTAINER" and _DEBIAN_MAINTAINER_RE.fullmatch(value) is None:
        raise ValidationError("MAINTAINER must be a safe RFC-style name and email")
    if name == "RFC2822_DATE" and re.fullmatch(
        r"(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} "
        r"(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) "
        r"[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} \+0000",
        value,
    ) is None:
        raise ValidationError("RFC2822_DATE must be a canonical UTC timestamp")
    if name in _XML_VALUE_NAMES and _PACKAGE_WORD_RE.fullmatch(value) is None:
        raise ValidationError(f"template value {name} contains unsupported metadata characters")


def render_template(template: str, values: Mapping[str, str]) -> str:
    tokens = set(_TOKEN_RE.findall(template))
    missing = sorted(tokens - set(values))
    extra = sorted(set(values) - tokens)
    if missing or extra:
        raise ValidationError(f"template values mismatch; missing={missing}, extra={extra}")
    for name, value in values.items():
        if not isinstance(value, str):
            raise ValidationError(f"template value {name} must be a string")
        if any(character in "\r\n" or ord(character) < 0x20 for character in value):
            raise ValidationError(f"template value {name} contains a control character")
        _validate_known_value(name, value)
    rendered = _TOKEN_RE.sub(lambda match: values[match.group(1)], template)
    if _TOKEN_RE.search(rendered):
        raise ValidationError("template contains unresolved values")
    return rendered


def render_file(template_path: Path, output_path: Path, values: Mapping[str, str]) -> None:
    render_values = dict(values)
    if template_path.name.endswith(".nuspec.in"):
        render_values.update(
            (name, xml_escape(value, {'"': "&quot;", "'": "&apos;"}))
            for name, value in values.items()
            if name in _XML_VALUE_NAMES
        )
    rendered = render_template(template_path.read_text(encoding="utf-8"), render_values)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8", newline="\n")
    output_path.chmod(0o755 if rendered.startswith("#!") else 0o644)
