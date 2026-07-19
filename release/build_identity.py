"""Fail-closed validation of release-builder host and toolchain identity.

The functions in this module are deliberately pure: workflow steps collect the
facts with platform-native commands, extract only the documented fields, and
pass those values here.  A runner label is never treated as evidence.

Assumptions which must remain explicit in workflow integration:

* ``/etc/os-release`` values are parsed without executing the file and are
  passed here after quote removal, while command output is stripped once.
* Ubuntu Resolute is the 26.04 release; Debian Bookworm and Trixie are versions
  12 and 13 respectively.
* Enterprise Linux packages are built on the reviewed AlmaLinux 9.8 and 10.2
  images.  A route alone (for example ``el-9``) is insufficient evidence.
* RPM macro facts are the stripped outputs of ``rpm --eval`` for the named
  macros.  An undefined conditional macro is represented by the empty string.
* Windows compile flags are tokens from an actual Release compilation of the
  shared SDK target, and dumpbin evidence is from the staged SDK DLL itself.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
from types import MappingProxyType
from typing import Any


class BuildIdentityError(ValueError):
    """Raised when observed builder facts do not prove the requested target."""


@dataclass(frozen=True)
class AptTarget:
    """Exact operating-system and architecture identity for an APT route."""

    distribution: str
    version_id: str
    codename: str
    dpkg_architecture: str
    uname_machine: str

    def expected_facts(self) -> dict[str, str]:
        return {
            "os_id": self.distribution,
            "os_version_id": self.version_id,
            "os_version_codename": self.codename,
            "dpkg_architecture": self.dpkg_architecture,
            "uname_machine": self.uname_machine,
        }


@dataclass(frozen=True)
class RpmTarget:
    """Exact RPM route identity, including distribution macro results."""

    distribution: str
    release: str
    architecture: str
    uname_machine: str
    builder_os_id: str
    builder_os_version_id: str
    rpm_fedora: str
    rpm_rhel: str
    rpm_dist: str


@dataclass(frozen=True)
class AurTarget:
    """Exact Arch Linux host identity for an AUR validation route."""

    architecture: str
    uname_machine: str

    def expected_facts(self) -> dict[str, str]:
        return {
            "os_id": "arch",
            "arch_release_present": "true",
            "uname_machine": self.uname_machine,
        }


WINDOWS_TARGET_ID = "windows-x64-v143-md"
WINDOWS_CACHE_EXPECTATIONS: Mapping[str, str] = MappingProxyType(
    {
        "CMAKE_GENERATOR": "Visual Studio 17 2022",
        "CMAKE_GENERATOR_PLATFORM": "x64",
        "CMAKE_GENERATOR_TOOLSET": "v143",
        "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreadedDLL",
    }
)

_TARGET_CATALOG_FIELDS = frozenset(
    {"schema_version", "apt", "rpm", "aur", "homebrew", "chocolatey"}
)
_TARGET_APT_FIELDS = frozenset(
    {
        "architectures",
        "builder_os_id",
        "builder_os_version_codename",
        "builder_os_version_id",
        "distribution",
        "release",
    }
)
_TARGET_APT_ARCH_FIELDS = frozenset(
    {"architecture", "builder_dpkg_architecture", "builder_uname_machine"}
)
_TARGET_RPM_FIELDS = frozenset(
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
_TARGET_RPM_ARCH_FIELDS = frozenset(
    {"architecture", "builder_rpm_architecture", "builder_uname_machine"}
)
_PROVIDER_FIELDS = {
    "aur": frozenset({"package_base", "architectures"}),
    "homebrew": frozenset({"tap", "bottle_tags"}),
    "chocolatey": frozenset({"package_id", "architecture", "toolset", "runtime"}),
}
_EMPTY_RPM_FIELDS = frozenset({"builder_rpm_fedora", "builder_rpm_rhel"})
_WINDOWS_FACT_FIELDS = frozenset(
    {
        "os_architecture",
        "process_architecture",
        "compiler_id",
        "msc_ver",
        "pointer_bits",
        "build_configuration",
        "cache",
        "shared_compile_flags",
        "dumpbin_dependents",
    }
)
_RPM_FACT_FIELDS = frozenset(
    {
        "os_id",
        "os_version_id",
        "rpm_fedora",
        "rpm_rhel",
        "rpm_dist",
        "rpm_architecture",
        "uname_machine",
    }
)
_ABI_VERSION_RE = re.compile(
    r"(?:0\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)|[1-9][0-9]*)"
)
_SDK_DLL_RE = re.compile(
    rf"mcp-cpp-sdk-(?P<abi>{_ABI_VERSION_RE.pattern})\.dll",
    re.IGNORECASE,
)
_DYNAMIC_VCRUNTIME_RE = re.compile(r"\bVCRUNTIME140(?:_[0-9]+)?\.DLL\b", re.IGNORECASE)
_DYNAMIC_MSVCXX_RE = re.compile(r"\bMSVCP140(?:_[0-9]+)?\.DLL\b", re.IGNORECASE)
_DEBUG_CRT_RE = re.compile(
    r"\b(?:VCRUNTIME140D(?:_[0-9]+)?|MSVCP140D(?:_[0-9]+)?|UCRTBASED)\.DLL\b",
    re.IGNORECASE,
)


def _require_exact_fields(name: str, value: object, expected: frozenset[str]) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise BuildIdentityError(f"{name} must be a mapping")
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(repr(field) for field in actual - expected)
        raise BuildIdentityError(f"{name} fields mismatch; missing={missing}, extra={extra}")
    if any(not isinstance(field, str) for field in actual):
        raise BuildIdentityError(f"{name} field names must be strings")
    return value


def _require_string(
    name: str,
    value: object,
    *,
    allow_empty: bool = False,
    maximum: int = 128,
) -> str:
    if not isinstance(value, str) or (not value and not allow_empty) or len(value) > maximum:
        qualifier = "possibly empty" if allow_empty else "non-empty"
        raise BuildIdentityError(f"{name} must be a {qualifier} bounded string")
    if value != value.strip() or any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise BuildIdentityError(f"{name} must be stripped and contain no control characters")
    return value


def _require_expected_facts(name: str, value: object, expected: Mapping[str, str]) -> dict[str, str]:
    facts = _require_exact_fields(name, value, frozenset(expected))
    normalized: dict[str, str] = {}
    for field, expected_value in expected.items():
        observed = _require_string(
            f"{name}.{field}",
            facts[field],
            allow_empty=expected_value == "",
        )
        if observed != expected_value:
            raise BuildIdentityError(
                f"{name}.{field} is {observed!r}; expected {expected_value!r}"
            )
        normalized[field] = observed
    return normalized


def _target(name: str, target_id: object, targets: Mapping[str, Any]) -> Any:
    target_name = _require_string(f"{name} target_id", target_id)
    try:
        return targets[target_name]
    except KeyError as error:
        raise BuildIdentityError(f"unknown {name} target: {target_name}") from error


def _verified_result(kind: str, target_id: str, facts: Mapping[str, str], evidence: object) -> dict[str, object]:
    canonical_evidence = json.dumps(
        evidence,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    return {
        "schema_version": 1,
        "kind": kind,
        "target_id": target_id,
        "facts": {field: facts[field] for field in sorted(facts)},
        "evidence": evidence,
        "evidence_sha256": hashlib.sha256(canonical_evidence).hexdigest(),
    }


def _require_list(name: str, value: object) -> list[Any]:
    if not isinstance(value, list):
        raise BuildIdentityError(f"{name} must be a JSON array")
    return value


def _normalized_record(
    name: str,
    value: object,
    fields: frozenset[str],
    *,
    allow_empty: frozenset[str] = frozenset(),
) -> dict[str, str]:
    record = _require_exact_fields(name, value, fields)
    return {
        field: _require_string(
            f"{name}.{field}",
            record[field],
            allow_empty=field in allow_empty,
            maximum=160,
        )
        for field in fields
    }


def _catalog_projection(document: object) -> tuple[dict[str, str], ...]:
    catalog = _require_exact_fields("target catalog", document, _TARGET_CATALOG_FIELDS)
    if type(catalog["schema_version"]) is not int or catalog["schema_version"] != 2:
        raise BuildIdentityError("target catalog schema_version must be exactly 2")
    for provider, fields in _PROVIDER_FIELDS.items():
        value = _require_exact_fields(f"target catalog {provider}", catalog[provider], fields)
        for field, item in value.items():
            if isinstance(item, list):
                normalized = [_require_string(f"target catalog {provider}.{field}", entry) for entry in item]
                if not normalized or len(normalized) != len(set(normalized)):
                    raise BuildIdentityError(
                        f"target catalog {provider}.{field} must be a non-empty unique list"
                    )
            else:
                _require_string(f"target catalog {provider}.{field}", item)

    projected: list[dict[str, str]] = []
    for index, raw_group in enumerate(_require_list("target catalog apt", catalog["apt"])):
        name = f"target catalog apt[{index}]"
        group = _require_exact_fields(name, raw_group, _TARGET_APT_FIELDS)
        scalar = {
            field: _require_string(f"{name}.{field}", group[field])
            for field in _TARGET_APT_FIELDS - {"architectures"}
        }
        architectures = _require_list(f"{name}.architectures", group["architectures"])
        if not architectures:
            raise BuildIdentityError(f"{name}.architectures must not be empty")
        for arch_index, raw_architecture in enumerate(architectures):
            architecture = _normalized_record(
                f"{name}.architectures[{arch_index}]",
                raw_architecture,
                _TARGET_APT_ARCH_FIELDS,
            )
            target_id = f"{scalar['distribution']}-{scalar['release']}-{architecture['architecture']}"
            projected.append(
                {
                    "architecture": architecture["architecture"],
                    "builder_dpkg_architecture": architecture["builder_dpkg_architecture"],
                    "builder_os_id": scalar["builder_os_id"],
                    "builder_os_version_codename": scalar["builder_os_version_codename"],
                    "builder_os_version_id": scalar["builder_os_version_id"],
                    "builder_uname_machine": architecture["builder_uname_machine"],
                    "distribution": scalar["distribution"],
                    "format": "apt",
                    "id": target_id,
                    "release": scalar["release"],
                    "runner": (
                        "ubuntu-24.04-arm"
                        if architecture["builder_uname_machine"] == "aarch64"
                        else "ubuntu-24.04"
                    ),
                }
            )

    for index, raw_group in enumerate(_require_list("target catalog rpm", catalog["rpm"])):
        name = f"target catalog rpm[{index}]"
        group = _require_exact_fields(name, raw_group, _TARGET_RPM_FIELDS)
        scalar = {
            field: _require_string(
                f"{name}.{field}",
                group[field],
                allow_empty=field in _EMPTY_RPM_FIELDS,
            )
            for field in _TARGET_RPM_FIELDS - {"architectures"}
        }
        architectures = _require_list(f"{name}.architectures", group["architectures"])
        if not architectures:
            raise BuildIdentityError(f"{name}.architectures must not be empty")
        for arch_index, raw_architecture in enumerate(architectures):
            architecture = _normalized_record(
                f"{name}.architectures[{arch_index}]",
                raw_architecture,
                _TARGET_RPM_ARCH_FIELDS,
            )
            target_id = f"{scalar['distribution']}-{scalar['release']}-{architecture['architecture']}"
            projected.append(
                {
                    "architecture": architecture["architecture"],
                    "builder_os_id": scalar["builder_os_id"],
                    "builder_os_version_id": scalar["builder_os_version_id"],
                    "builder_rpm_architecture": architecture["builder_rpm_architecture"],
                    "builder_rpm_dist": scalar["builder_rpm_dist"],
                    "builder_rpm_fedora": scalar["builder_rpm_fedora"],
                    "builder_rpm_rhel": scalar["builder_rpm_rhel"],
                    "builder_uname_machine": architecture["builder_uname_machine"],
                    "distribution": scalar["distribution"],
                    "format": "rpm",
                    "id": target_id,
                    "release": scalar["release"],
                    "runner": (
                        "ubuntu-24.04-arm"
                        if architecture["builder_uname_machine"] == "aarch64"
                        else "ubuntu-24.04"
                    ),
                }
            )
    identifiers = [target["id"] for target in projected]
    if (
        len(projected) != 18
        or sum(target["format"] == "apt" for target in projected) != 10
        or sum(target["format"] == "rpm" for target in projected) != 8
        or len(identifiers) != len(set(identifiers))
    ):
        raise BuildIdentityError("target catalog must contain 10 unique APT and 8 unique RPM targets")
    return tuple(projected)


def _reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise BuildIdentityError(f"JSON contains duplicate key: {key}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> None:
    raise BuildIdentityError(f"JSON contains non-finite number: {value}")


def _load_strict_json(path: Path) -> object:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise BuildIdentityError(f"cannot read {path}: {error}") from error
    if not data or len(data) > 1024 * 1024:
        raise BuildIdentityError(f"{path} must be a non-empty JSON file no larger than 1 MiB")
    try:
        return json.loads(
            data.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_json_keys,
            parse_constant=_reject_json_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise BuildIdentityError(f"cannot parse strict JSON from {path}: {error}") from error


def load_and_validate_target_projection(
    target_catalog_path: Path,
) -> tuple[dict[str, str], ...]:
    """Load the single checked-in package target catalog strictly."""

    return _catalog_projection(_load_strict_json(target_catalog_path))


def _target_maps(
    document: object,
    projection: Sequence[Mapping[str, str]],
) -> tuple[Mapping[str, AptTarget], Mapping[str, RpmTarget], Mapping[str, AurTarget]]:
    catalog = _require_exact_fields("target catalog", document, _TARGET_CATALOG_FIELDS)
    apt: dict[str, AptTarget] = {}
    rpm: dict[str, RpmTarget] = {}
    for target in projection:
        if target["format"] == "apt":
            apt[target["id"]] = AptTarget(
                distribution=target["builder_os_id"],
                version_id=target["builder_os_version_id"],
                codename=target["builder_os_version_codename"],
                dpkg_architecture=target["builder_dpkg_architecture"],
                uname_machine=target["builder_uname_machine"],
            )
        else:
            rpm[target["id"]] = RpmTarget(
                distribution=target["distribution"],
                release=target["release"],
                architecture=target["builder_rpm_architecture"],
                uname_machine=target["builder_uname_machine"],
                builder_os_id=target["builder_os_id"],
                builder_os_version_id=target["builder_os_version_id"],
                rpm_fedora=target["builder_rpm_fedora"],
                rpm_rhel=target["builder_rpm_rhel"],
                rpm_dist=target["builder_rpm_dist"],
            )
    aur_value = _require_exact_fields(
        "target catalog aur", catalog["aur"], _PROVIDER_FIELDS["aur"]
    )
    architectures = _require_list("target catalog aur.architectures", aur_value["architectures"])
    aur = {
        architecture: AurTarget(architecture, architecture)
        for architecture in architectures
        if isinstance(architecture, str)
    }
    return MappingProxyType(apt), MappingProxyType(rpm), MappingProxyType(aur)


_TARGET_CATALOG_PATH = Path(__file__).resolve().parent.parent / "packaging/targets.json"
_TARGET_CATALOG_DOCUMENT = _load_strict_json(_TARGET_CATALOG_PATH)
_TARGET_PROJECTION = _catalog_projection(_TARGET_CATALOG_DOCUMENT)
CURRENT_NATIVE_TARGET_IDS = tuple(target["id"] for target in _TARGET_PROJECTION)
APT_TARGETS, RPM_TARGETS, AUR_TARGETS = _target_maps(
    _TARGET_CATALOG_DOCUMENT, _TARGET_PROJECTION
)


def validate_apt_build_identity(target_id: str, facts: Mapping[str, object]) -> dict[str, object]:
    """Validate exact Ubuntu/Debian release and CPU facts for an APT target."""

    target = _target("APT", target_id, APT_TARGETS)
    normalized = _require_expected_facts("APT facts", facts, target.expected_facts())
    return _verified_result("apt", target_id, normalized, normalized)


def validate_rpm_build_identity(
    target_id: str,
    facts: Mapping[str, object],
) -> dict[str, object]:
    """Validate exact Fedora or pinned AlmaLinux RPM build facts."""

    target = _target("RPM", target_id, RPM_TARGETS)
    expected = {
        "os_id": target.builder_os_id,
        "os_version_id": target.builder_os_version_id,
        "rpm_fedora": target.rpm_fedora,
        "rpm_rhel": target.rpm_rhel,
        "rpm_dist": target.rpm_dist,
        "rpm_architecture": target.architecture,
        "uname_machine": target.uname_machine,
    }
    if frozenset(expected) != _RPM_FACT_FIELDS:  # Defensive against accidental schema drift.
        raise BuildIdentityError("internal RPM fact schema mismatch")
    normalized = _require_expected_facts("RPM facts", facts, expected)
    return _verified_result("rpm", target_id, normalized, normalized)


def validate_aur_build_identity(architecture: str, facts: Mapping[str, object]) -> dict[str, object]:
    """Validate that an AUR build runs on Arch Linux for the requested CPU."""

    target = _target("AUR", architecture, AUR_TARGETS)
    normalized = _require_expected_facts("AUR facts", facts, target.expected_facts())
    return _verified_result("aur", f"aur-{architecture}", normalized, normalized)


def _windows_cache(value: object) -> dict[str, str]:
    return _require_expected_facts("Windows CMake cache", value, WINDOWS_CACHE_EXPECTATIONS)


def _windows_compile_flags(value: object) -> list[str]:
    if isinstance(value, (str, bytes)) or not isinstance(value, Sequence):
        raise BuildIdentityError("Windows shared_compile_flags must be a sequence of tokens")
    flags = [
        _require_string(f"Windows shared_compile_flags[{index}]", flag, maximum=512)
        for index, flag in enumerate(value)
    ]
    if not flags:
        raise BuildIdentityError("Windows shared_compile_flags must not be empty")
    folded = {flag.upper() for flag in flags}
    for required in ("/MD", "/O2", "/DNDEBUG"):
        if required not in folded:
            raise BuildIdentityError(f"Windows shared compilation did not prove {required}")
    forbidden = folded & {"/MT", "/MTD", "/MDD", "/OD", "/D_DEBUG", "/DDEBUG"}
    forbidden.update(
        flag
        for flag in folded
        if flag.replace('"', "").startswith("/D_DEBUG") or flag.replace('"', "") == "_DEBUG"
    )
    if forbidden:
        raise BuildIdentityError(f"Windows shared compilation contains forbidden flags: {sorted(forbidden)}")
    return sorted(folded)


def _windows_dumpbin(value: object, *, expected_sdk_dll: str) -> dict[str, str]:
    if not isinstance(value, Mapping) or not value:
        raise BuildIdentityError("Windows dumpbin_dependents must be a non-empty mapping")
    normalized: dict[str, str] = {}
    for raw_name, raw_output in value.items():
        name = _require_string("Windows dumpbin filename", raw_name, maximum=160)
        if not re.fullmatch(r"[A-Za-z0-9_.-]+\.(?:dll|exe)", name, re.IGNORECASE):
            raise BuildIdentityError(f"unsafe or unsupported dumpbin filename: {name}")
        if not isinstance(raw_output, str) or not raw_output or len(raw_output) > 256 * 1024:
            raise BuildIdentityError(f"dumpbin output for {name} must be non-empty and bounded")
        output = raw_output.replace("\r\n", "\n").strip()
        if not output or "\x00" in output:
            raise BuildIdentityError(f"dumpbin output for {name} is malformed")
        if name.casefold() not in output.casefold():
            raise BuildIdentityError(f"dumpbin output is not bound to {name}")
        marker = "Image has the following dependencies:"
        _, separator, dependency_section = output.partition(marker)
        if not separator:
            raise BuildIdentityError(f"dumpbin output for {name} has no dependency section")
        if _DEBUG_CRT_RE.search(dependency_section):
            raise BuildIdentityError(f"dumpbin output for {name} contains a debug CRT dependency")
        if (
            _DYNAMIC_VCRUNTIME_RE.search(dependency_section) is None
            or _DYNAMIC_MSVCXX_RE.search(dependency_section) is None
        ):
            raise BuildIdentityError(
                f"dumpbin output for {name} does not prove the dynamic Release MSVC CRT"
            )
        dependencies = sorted(
            {match.upper() for match in re.findall(r"\b[A-Za-z0-9_.-]+\.dll\b", dependency_section, re.IGNORECASE)}
        )
        normalized[name] = (
            f"Dump of file {name}\nFile Type: DLL\n\n"
            "Image has the following dependencies:\n\n"
            + "".join(f"    {dependency}\n" for dependency in dependencies)
        ).rstrip()
    sdk_dlls = [name for name in normalized if _SDK_DLL_RE.fullmatch(name)]
    if len(sdk_dlls) != 1:
        raise BuildIdentityError("dumpbin evidence must contain exactly one versioned mcp-cpp-sdk DLL")
    if sdk_dlls[0] != expected_sdk_dll:
        raise BuildIdentityError(
            f"dumpbin SDK DLL is {sdk_dlls[0]!r}; expected {expected_sdk_dll!r}"
        )
    return normalized


def validate_windows_build_identity(
    facts: Mapping[str, object],
    *,
    expected_abi_version: str,
) -> dict[str, object]:
    """Purely validate x64 VS 2022/v143 dynamic Release build evidence."""

    abi_version = _require_string(
        "Windows expected ABI version",
        expected_abi_version,
        maximum=32,
    )
    if _ABI_VERSION_RE.fullmatch(abi_version) is None:
        raise BuildIdentityError(
            "Windows expected ABI version must be full 0.minor.patch for 0.x or major for 1.x+"
        )
    expected_sdk_dll = f"mcp-cpp-sdk-{abi_version}.dll"
    values = _require_exact_fields("Windows facts", facts, _WINDOWS_FACT_FIELDS)
    expected_strings = {
        "os_architecture": "64-bit",
        "process_architecture": "AMD64",
        "compiler_id": "MSVC",
        "build_configuration": "Release",
    }
    normalized_strings = {
        field: _require_string(f"Windows facts.{field}", values[field])
        for field in expected_strings
    }
    for field, expected in expected_strings.items():
        if normalized_strings[field] != expected:
            raise BuildIdentityError(
                f"Windows facts.{field} is {normalized_strings[field]!r}; expected {expected!r}"
            )

    msc_ver = values["msc_ver"]
    pointer_bits = values["pointer_bits"]
    if type(msc_ver) is not int or not 1930 <= msc_ver <= 1949:
        raise BuildIdentityError("Windows msc_ver must be an integer in the v143 193x/194x range")
    if type(pointer_bits) is not int or pointer_bits != 64:
        raise BuildIdentityError("Windows compile probe must report exactly 64 pointer bits")

    cache = _windows_cache(values["cache"])
    flags = _windows_compile_flags(values["shared_compile_flags"])
    dependents = _windows_dumpbin(
        values["dumpbin_dependents"],
        expected_sdk_dll=expected_sdk_dll,
    )

    evidence = {
        **normalized_strings,
        "abi_version": abi_version,
        "msc_ver": msc_ver,
        "pointer_bits": pointer_bits,
        "cache": cache,
        "shared_compile_flags": flags,
        "dumpbin_dependents": dependents,
    }
    summary = {
        **normalized_strings,
        "architecture": "x64",
        "generator": cache["CMAKE_GENERATOR"],
        "toolset": cache["CMAKE_GENERATOR_TOOLSET"],
        "runtime": cache["CMAKE_MSVC_RUNTIME_LIBRARY"],
        "msc_ver": str(msc_ver),
        "abi_version": abi_version,
        "sdk_dll": expected_sdk_dll,
    }
    return _verified_result("windows", WINDOWS_TARGET_ID, summary, evidence)


def main(argv: Sequence[str] | None = None) -> int:
    """Validate checked-in target files for workflow and local preflight use."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", required=True, type=Path)
    arguments = parser.parse_args(argv)
    try:
        targets = load_and_validate_target_projection(arguments.targets)
    except BuildIdentityError as error:
        print(f"target projection validation failed: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "apt_targets": sum(target["format"] == "apt" for target in targets),
                "rpm_targets": sum(target["format"] == "rpm" for target in targets),
                "status": "VALID",
            },
            sort_keys=True,
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
