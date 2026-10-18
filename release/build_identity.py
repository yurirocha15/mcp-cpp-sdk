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
import hashlib
import json
from pathlib import Path
import re
import sys
from types import MappingProxyType
from typing import Any

from .model import ValidationError
from .native_targets import NativeTarget, load_native_targets, load_target_catalog


class BuildIdentityError(ValueError):
    """Raised when observed builder facts do not prove the requested target."""


WINDOWS_TARGET_ID = "windows-x64-v143-md"
WINDOWS_CACHE_EXPECTATIONS: Mapping[str, str] = MappingProxyType(
    {
        "CMAKE_GENERATOR": "Visual Studio 17 2022",
        "CMAKE_GENERATOR_PLATFORM": "x64",
        "CMAKE_GENERATOR_TOOLSET": "v143",
        "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreadedDLL",
    }
)

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


def _target(name: str, target_id: object, package_format: str) -> NativeTarget:
    target_name = _require_string(f"{name} target_id", target_id)
    try:
        targets = {
            target.id: target for target in load_native_targets(_TARGET_CATALOG_PATH)
        }
        target = targets[target_name]
    except ValidationError as error:
        raise BuildIdentityError(str(error)) from error
    except KeyError as error:
        raise BuildIdentityError(f"unknown {name} target: {target_name}") from error
    if target.format != package_format:
        raise BuildIdentityError(f"unknown {name} target: {target_name}")
    return target


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


def expected_native_facts(target: NativeTarget) -> dict[str, str]:
    """Return the exact platform facts encoded by one validated native target."""

    if target.format == "apt":
        return {
            "os_id": target.builder_os_id,
            "os_version_id": target.builder_os_version_id,
            "os_version_codename": target.builder_os_version_codename or "",
            "dpkg_architecture": target.builder_dpkg_architecture or "",
            "uname_machine": target.builder_uname_machine,
        }
    if target.format == "rpm":
        return {
            "os_id": target.builder_os_id,
            "os_version_id": target.builder_os_version_id,
            "rpm_fedora": target.builder_rpm_fedora or "",
            "rpm_rhel": target.builder_rpm_rhel or "",
            "rpm_dist": target.builder_rpm_dist or "",
            "rpm_architecture": target.builder_rpm_architecture or "",
            "uname_machine": target.builder_uname_machine,
        }
    raise BuildIdentityError(f"unsupported native target format: {target.format}")


def expected_aur_facts(architecture: str) -> dict[str, str]:
    """Return the exact Arch host facts for a catalogued AUR architecture."""

    try:
        document, _ = load_target_catalog(_TARGET_CATALOG_PATH)
    except ValidationError as error:
        raise BuildIdentityError(str(error)) from error
    aur = document["aur"]
    architectures = aur["architectures"] if isinstance(aur, Mapping) else ()
    if architecture not in architectures:
        raise BuildIdentityError(f"unknown AUR target: {architecture}")
    return {
        "os_id": "arch",
        "arch_release_present": "true",
        "uname_machine": architecture,
    }


_TARGET_CATALOG_PATH = Path(__file__).resolve().parent.parent / "packaging/targets.json"


def validate_apt_build_identity(target_id: str, facts: Mapping[str, object]) -> dict[str, object]:
    """Validate exact Ubuntu/Debian release and CPU facts for an APT target."""

    return validate_native_build_identity(_target("APT", target_id, "apt"), facts)


def validate_native_build_identity(
    target: NativeTarget, facts: Mapping[str, object]
) -> dict[str, object]:
    """Validate facts against the supplied current or signed historical target."""

    if target.format not in {"apt", "rpm"}:
        raise BuildIdentityError(f"unsupported native target format: {target.format}")
    label = target.format.upper()
    expected = expected_native_facts(target)
    if target.format == "rpm" and frozenset(expected) != _RPM_FACT_FIELDS:
        raise BuildIdentityError("internal RPM fact schema mismatch")
    normalized = _require_expected_facts(
        f"{label} facts", facts, expected
    )
    return _verified_result(target.format, target.id, normalized, normalized)


def validate_rpm_build_identity(
    target_id: str,
    facts: Mapping[str, object],
) -> dict[str, object]:
    """Validate exact Fedora or pinned AlmaLinux RPM build facts."""

    return validate_native_build_identity(_target("RPM", target_id, "rpm"), facts)


def validate_aur_build_identity(architecture: str, facts: Mapping[str, object]) -> dict[str, object]:
    """Validate that an AUR build runs on Arch Linux for the requested CPU."""

    normalized = _require_expected_facts(
        "AUR facts", facts, expected_aur_facts(architecture)
    )
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
        targets = load_native_targets(arguments.targets)
    except (BuildIdentityError, ValidationError) as error:
        print(f"target projection validation failed: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "apt_targets": sum(target.format == "apt" for target in targets),
                "rpm_targets": sum(target.format == "rpm" for target in targets),
                "status": "VALID",
            },
            sort_keys=True,
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
