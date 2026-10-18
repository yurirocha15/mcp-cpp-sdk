"""Single strict parser for native package targets and publication routes."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
from typing import Any

from .model import ValidationError
from .strict_json import load_strict


_CATALOG_FIELDS = frozenset(
    {"schema_version", "apt", "rpm", "aur", "homebrew", "chocolatey"}
)
_APT_GROUP_FIELDS = frozenset(
    {
        "architectures",
        "builder_os_id",
        "builder_os_version_codename",
        "builder_os_version_id",
        "distribution",
        "release",
    }
)
_APT_ARCH_FIELDS = frozenset(
    {"architecture", "builder_dpkg_architecture", "builder_uname_machine"}
)
_RPM_GROUP_FIELDS = frozenset(
    {
        "architectures",
        "builder_os_id",
        "builder_os_version_id",
        "builder_rpm_dist",
        "builder_rpm_fedora",
        "builder_rpm_rhel",
        "distribution",
        "release",
    }
)
_RPM_ARCH_FIELDS = frozenset(
    {"architecture", "builder_rpm_architecture", "builder_uname_machine"}
)
_CHANNEL_FIELDS = {
    "aur": frozenset({"package_base", "architectures"}),
    "homebrew": frozenset({"tap", "bottle_tags"}),
    "chocolatey": frozenset(
        {"package_id", "architecture", "toolset", "runtime"}
    ),
}
_BASE_FIELDS = frozenset(
    {"id", "format", "distribution", "release", "architecture", "runner"}
)
_APT_FIELDS = _BASE_FIELDS | frozenset(
    {
        "builder_os_id",
        "builder_os_version_id",
        "builder_os_version_codename",
        "builder_dpkg_architecture",
        "builder_uname_machine",
    }
)
_RPM_FIELDS = _BASE_FIELDS | frozenset(
    {
        "builder_os_id",
        "builder_os_version_id",
        "builder_rpm_fedora",
        "builder_rpm_rhel",
        "builder_rpm_dist",
        "builder_rpm_architecture",
        "builder_uname_machine",
    }
)
_ROUTE_FIELDS = frozenset(
    {
        "asset",
        "format",
        "route_id",
        "distribution",
        "release",
        "target_architecture",
        "package_name",
        "package_version",
        "package_architecture",
        "build_tuple",
        "identity_asset",
    }
)


def _mapping(name: str, value: object, fields: frozenset[str]) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or set(value) != fields:
        raise ValidationError(f"{name} schema is not exact")
    if any(not isinstance(field, str) for field in value):
        raise ValidationError(f"{name} field names must be strings")
    return value


def _string(name: str, value: object, *, allow_empty: bool = False) -> str:
    if (
        not isinstance(value, str)
        or (not value and not allow_empty)
        or len(value) > 160
        or value != value.strip()
        or any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)
    ):
        raise ValidationError(f"{name} is not a canonical bounded string")
    return value


def _list(name: str, value: object) -> list[Any]:
    if not isinstance(value, list) or not value:
        raise ValidationError(f"{name} must be a non-empty array")
    return value


@dataclass(frozen=True)
class NativeTarget:
    id: str
    format: str
    distribution: str
    release: str
    architecture: str
    runner: str
    builder_os_id: str
    builder_os_version_id: str
    builder_uname_machine: str
    builder_os_version_codename: str | None = None
    builder_dpkg_architecture: str | None = None
    builder_rpm_fedora: str | None = None
    builder_rpm_rhel: str | None = None
    builder_rpm_dist: str | None = None
    builder_rpm_architecture: str | None = None

    @classmethod
    def from_mapping(cls, value: Any) -> "NativeTarget":
        if not isinstance(value, Mapping) or value.get("format") not in {"apt", "rpm"}:
            raise ValidationError("native target schema is not exact")
        fields = _APT_FIELDS if value["format"] == "apt" else _RPM_FIELDS
        record = _mapping("native target", value, fields)
        empty_allowed = {"builder_rpm_fedora", "builder_rpm_rhel"}
        normalized = {
            field: _string(
                f"native target.{field}",
                record[field],
                allow_empty=field in empty_allowed,
            )
            for field in fields
        }
        target = cls(
            **{field: normalized[field] for field in _BASE_FIELDS},
            builder_os_id=normalized["builder_os_id"],
            builder_os_version_id=normalized["builder_os_version_id"],
            builder_uname_machine=normalized["builder_uname_machine"],
            builder_os_version_codename=normalized.get("builder_os_version_codename"),
            builder_dpkg_architecture=normalized.get("builder_dpkg_architecture"),
            builder_rpm_fedora=normalized.get("builder_rpm_fedora"),
            builder_rpm_rhel=normalized.get("builder_rpm_rhel"),
            builder_rpm_dist=normalized.get("builder_rpm_dist"),
            builder_rpm_architecture=normalized.get("builder_rpm_architecture"),
        )
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", target.id):
            raise ValidationError("native target ID is unsafe")
        if target.id != f"{target.distribution}-{target.release}-{target.architecture}":
            raise ValidationError("native target ID does not match its route")
        expected_runner = (
            "ubuntu-24.04-arm"
            if target.builder_uname_machine == "aarch64"
            else "ubuntu-24.04"
        )
        if target.runner != expected_runner:
            raise ValidationError("native target runner does not match its builder CPU")
        architectures = {"apt": {"amd64", "arm64"}, "rpm": {"x86_64", "aarch64"}}
        if target.architecture not in architectures[target.format]:
            raise ValidationError("native target architecture does not match its format")
        return target

    def to_mapping(self) -> dict[str, str]:
        value = {
            "id": self.id,
            "format": self.format,
            "distribution": self.distribution,
            "release": self.release,
            "architecture": self.architecture,
            "runner": self.runner,
            "builder_os_id": self.builder_os_id,
            "builder_os_version_id": self.builder_os_version_id,
            "builder_uname_machine": self.builder_uname_machine,
        }
        if self.format == "apt":
            value.update(
                {
                    "builder_os_version_codename": self.builder_os_version_codename or "",
                    "builder_dpkg_architecture": self.builder_dpkg_architecture or "",
                }
            )
        else:
            value.update(
                {
                    "builder_rpm_fedora": self.builder_rpm_fedora or "",
                    "builder_rpm_rhel": self.builder_rpm_rhel or "",
                    "builder_rpm_dist": self.builder_rpm_dist or "",
                    "builder_rpm_architecture": self.builder_rpm_architecture or "",
                }
            )
        return value


@dataclass(frozen=True)
class NativeRoute:
    asset: str
    format: str
    route_id: str
    distribution: str
    release: str
    target_architecture: str
    package_name: str
    package_version: str
    package_architecture: str
    build_tuple: str
    identity_asset: str

    @classmethod
    def from_mapping(cls, value: Any) -> "NativeRoute":
        record = _mapping("native route", value, _ROUTE_FIELDS)
        route = cls(
            **{
                field: _string(f"native route.{field}", record[field])
                for field in _ROUTE_FIELDS
            }
        )
        if PurePosixPath(route.asset).name != route.asset or route.asset.count("--") != 1:
            raise ValidationError("native route asset name is unsafe")
        if not route.asset.startswith(f"{route.route_id}--"):
            raise ValidationError("native route asset is not bound to its target")
        extension = ".deb" if route.format == "apt" else ".rpm"
        if route.format not in {"apt", "rpm"} or not route.asset.endswith(extension):
            raise ValidationError("native route asset extension does not match its format")
        if route.build_tuple != f"{route.format}-{route.route_id}":
            raise ValidationError("native route build tuple does not match its target")
        if route.identity_asset != f"build-identity-{route.route_id}.json":
            raise ValidationError("native route identity asset does not match its target")
        return route

    def to_mapping(self) -> dict[str, str]:
        return {field: getattr(self, field) for field in _ROUTE_FIELDS}


def load_target_catalog(path: Path) -> tuple[Mapping[str, Any], tuple[NativeTarget, ...]]:
    document = _mapping(
        "target catalog",
        load_strict(path, label="target catalog"),
        _CATALOG_FIELDS,
    )
    if type(document["schema_version"]) is not int or document["schema_version"] != 2:
        raise ValidationError("target catalog schema_version must be exactly 2")
    for channel, fields in _CHANNEL_FIELDS.items():
        value = _mapping(f"target catalog {channel}", document[channel], fields)
        for field, item in value.items():
            if isinstance(item, list):
                normalized = [
                    _string(f"target catalog {channel}.{field}", entry)
                    for entry in _list(f"target catalog {channel}.{field}", item)
                ]
                if len(normalized) != len(set(normalized)):
                    raise ValidationError(
                        f"target catalog {channel}.{field} contains duplicates"
                    )
            else:
                _string(f"target catalog {channel}.{field}", item)

    targets: list[NativeTarget] = []
    for package_format, group_fields, arch_fields in (
        ("apt", _APT_GROUP_FIELDS, _APT_ARCH_FIELDS),
        ("rpm", _RPM_GROUP_FIELDS, _RPM_ARCH_FIELDS),
    ):
        for index, raw_group in enumerate(_list(f"target catalog {package_format}", document[package_format])):
            name = f"target catalog {package_format}[{index}]"
            group = _mapping(name, raw_group, group_fields)
            scalar = {
                field: _string(
                    f"{name}.{field}",
                    group[field],
                    allow_empty=field in {"builder_rpm_fedora", "builder_rpm_rhel"},
                )
                for field in group_fields - {"architectures"}
            }
            for arch_index, raw_arch in enumerate(_list(f"{name}.architectures", group["architectures"])):
                arch_name = f"{name}.architectures[{arch_index}]"
                arch = _mapping(arch_name, raw_arch, arch_fields)
                architecture = {
                    field: _string(f"{arch_name}.{field}", arch[field])
                    for field in arch_fields
                }
                value = {
                    **scalar,
                    **architecture,
                    "format": package_format,
                    "id": f"{scalar['distribution']}-{scalar['release']}-{architecture['architecture']}",
                    "runner": (
                        "ubuntu-24.04-arm"
                        if architecture["builder_uname_machine"] == "aarch64"
                        else "ubuntu-24.04"
                    ),
                }
                targets.append(NativeTarget.from_mapping(value))
    identifiers = [target.id for target in targets]
    if (
        len(targets) != 18
        or sum(target.format == "apt" for target in targets) != 10
        or sum(target.format == "rpm" for target in targets) != 8
        or len(identifiers) != len(set(identifiers))
    ):
        raise ValidationError("target catalog must contain 10 unique APT and 8 unique RPM targets")
    return document, tuple(targets)


def load_native_targets(path: Path) -> tuple[NativeTarget, ...]:
    return load_target_catalog(path)[1]
