#!/usr/bin/env python3
"""Build a canonical public-ABI corpus and bind it to its exact toolchain.

The release workflow calls this module instead of embedding build logic in
YAML.  Validation and identity construction are pure functions; the ``build``
subcommand is the deliberately small process boundary around CMake, GCC and
libabigail.
"""

from __future__ import annotations

import argparse
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
from typing import Any, Mapping, Sequence
import xml.etree.ElementTree as ET

from .abi_baseline import AbiPolicyError, BaselineSelection, DIGEST, Version
from .artifacts import ABI_BUILD_IDENTITY_NAME as ABI_IDENTITY_NAME, ABI_BUILD_TUPLE


EXPECTED_PLATFORM = {
    "os_id": "ubuntu",
    "os_version_id": "24.04",
    "os_version_codename": "noble",
    "dpkg_architecture": "amd64",
    "uname_machine": "x86_64",
}
EXPECTED_COMPONENTS = (
    "boost",
    "cmake",
    "gcc",
    "libabigail",
    "libstdcxx",
    "nlohmann_json",
    "openssl",
)
EXPECTED_COMPONENT_ARTIFACTS = {
    "boost": ("boost-version.hpp",),
    "cmake": ("cmake",),
    "gcc": ("cc1plus", "g++-13"),
    "libabigail": ("abidiff", "abidw"),
    "libstdcxx": ("libstdc++.so.6",),
    "nlohmann_json": ("nlohmann-json-version.hpp",),
    "openssl": ("libcrypto.so.3", "opensslv.h"),
}
EXPECTED_COMPONENT_TARGETS = {
    "boost": "headers",
    "cmake": "x86_64-linux-gnu",
    "gcc": "x86_64-linux-gnu",
    "libabigail": "x86_64-linux-gnu",
    "libstdcxx": "x86_64-linux-gnu",
    "nlohmann_json": "headers",
    "openssl": "x86_64-linux-gnu",
}
ABI_RECIPE = {
    "generator": "Ninja",
    "build_type": "RelWithDebInfo",
    "cxx_standard": "20",
    "linkage": "shared",
    "compiler": "g++-13",
    "compile_options": ["-DNDEBUG", "-O2", "-g", "-std=gnu++20"],
    "source_prefix_map": "/usr/src/mcp-cpp-sdk",
    "build_prefix_map": "/usr/src/mcp-cpp-sdk-build",
    "abidw_options": [
        "--drop-private-types",
        "--headers-dir1",
        "--no-comp-dir-path",
        "--no-corpus-path",
    ],
    "abidiff_options": ["--no-default-suppression"],
}

_IDENTITY_FIELDS = frozenset(
    {
        "schema_version",
        "kind",
        "build_tuple",
        "builder_image",
        "source",
        "platform",
        "components",
        "recipe",
        "environment_sha256",
        "outputs",
    }
)
_SOURCE_FIELDS = frozenset({"tag", "commit"})
_COMPONENT_FIELDS = frozenset({"name", "upstream_version", "target", "packages", "artifacts"})
_PACKAGE_FIELDS = frozenset({"name", "version", "architecture", "content_sha256"})
_ARTIFACT_FIELDS = frozenset({"name", "sha256"})
_OUTPUT_FIELDS = frozenset(
    {"library_name", "library_sha256", "corpus_name", "corpus_sha256", "needed"}
)
_SAFE_TEXT = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+~:/= -]{0,159}")
_SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,127}")
_PACKAGE_NAME = re.compile(r"[a-z0-9][a-z0-9+.-]{0,127}")
_COMMIT = re.compile(r"[0-9a-f]{40}")
_UPSTREAM_VERSION = re.compile(r"[0-9][A-Za-z0-9.+:~_-]{0,79}")
_BUILDER_IMAGE = re.compile(
    r"[a-z0-9][a-z0-9.-]*(?:/[a-z0-9][a-z0-9._-]*)+@sha256:[0-9a-f]{64}"
)


class AbiBuildError(ValueError):
    """Raised when the ABI build environment or evidence is not exact."""


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode(
        "ascii"
    )


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _exact_mapping(value: object, fields: frozenset[str], label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or set(value) != fields:
        raise AbiBuildError(f"{label} fields are not exact")
    return value


def _safe_text(value: object, label: str, *, pattern: re.Pattern[str] = _SAFE_TEXT) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise AbiBuildError(f"{label} is malformed")
    return value


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or DIGEST.fullmatch(value) is None:
        raise AbiBuildError(f"{label} is not a lowercase SHA-256 digest")
    return value


def _environment(identity: Mapping[str, Any]) -> dict[str, object]:
    return {
        "build_tuple": identity["build_tuple"],
        "builder_image": identity["builder_image"],
        "platform": identity["platform"],
        "components": identity["components"],
        "recipe": identity["recipe"],
    }


def environment_sha256(identity: Mapping[str, Any]) -> str:
    """Return the comparable toolchain/dependency identity for one corpus."""

    return _sha256_bytes(_canonical_json(_environment(identity)))


def validate_build_identity(value: object) -> dict[str, object]:
    """Validate and normalize the signed ABI build identity."""

    identity = _exact_mapping(value, _IDENTITY_FIELDS, "ABI build identity")
    if identity["schema_version"] != 1 or identity["kind"] != "abi-build-identity":
        raise AbiBuildError("ABI build identity schema or kind is unsupported")
    if identity["build_tuple"] != ABI_BUILD_TUPLE:
        raise AbiBuildError("ABI build tuple is not canonical")
    builder_image = identity["builder_image"]
    if not isinstance(builder_image, str) or _BUILDER_IMAGE.fullmatch(builder_image) is None:
        raise AbiBuildError("ABI builder image is not pinned by an immutable OCI digest")

    source = _exact_mapping(identity["source"], _SOURCE_FIELDS, "ABI source identity")
    try:
        version = Version.from_tag(source["tag"], stable_only=True)
    except AbiPolicyError as error:
        raise AbiBuildError(f"ABI source tag is invalid: {error}") from error
    if not isinstance(source["commit"], str) or _COMMIT.fullmatch(source["commit"]) is None:
        raise AbiBuildError("ABI source commit is malformed")

    platform = _exact_mapping(
        identity["platform"], frozenset(EXPECTED_PLATFORM), "ABI build platform"
    )
    if dict(platform) != EXPECTED_PLATFORM:
        raise AbiBuildError("ABI corpus was not built on canonical Ubuntu Noble amd64")

    components = identity["components"]
    if not isinstance(components, list):
        raise AbiBuildError("ABI component identity must be a list")
    normalized_components: list[dict[str, object]] = []
    for raw_component in components:
        component = _exact_mapping(raw_component, _COMPONENT_FIELDS, "ABI component")
        name = _safe_text(component["name"], "ABI component name", pattern=_SAFE_NAME)
        upstream_version = _safe_text(
            component["upstream_version"], "ABI component version", pattern=_UPSTREAM_VERSION
        )
        target = _safe_text(component["target"], "ABI component target")
        packages = component["packages"]
        artifacts = component["artifacts"]
        if not isinstance(packages, list) or not packages:
            raise AbiBuildError("ABI component package evidence is missing")
        if not isinstance(artifacts, list) or not artifacts:
            raise AbiBuildError("ABI component artifact evidence is missing")
        normalized_packages: list[dict[str, str]] = []
        for raw_package in packages:
            package = _exact_mapping(raw_package, _PACKAGE_FIELDS, "ABI package")
            package_name = _safe_text(package["name"], "ABI package name", pattern=_PACKAGE_NAME)
            package_version = _safe_text(package["version"], "ABI package version")
            architecture = _safe_text(package["architecture"], "ABI package architecture")
            if architecture not in {"all", "amd64"}:
                raise AbiBuildError("ABI package architecture is not compatible with amd64")
            normalized_packages.append(
                {
                    "name": package_name,
                    "version": package_version,
                    "architecture": architecture,
                    "content_sha256": _digest(
                        package["content_sha256"], "ABI package content digest"
                    ),
                }
            )
        normalized_artifacts: list[dict[str, str]] = []
        for raw_artifact in artifacts:
            artifact = _exact_mapping(raw_artifact, _ARTIFACT_FIELDS, "ABI component artifact")
            normalized_artifacts.append(
                {
                    "name": _safe_text(
                        artifact["name"], "ABI component artifact name", pattern=_SAFE_NAME
                    ),
                    "sha256": _digest(artifact["sha256"], "ABI component artifact digest"),
                }
            )
        if normalized_packages != sorted(normalized_packages, key=lambda item: item["name"]):
            raise AbiBuildError("ABI component packages are not canonically ordered")
        if len({item["name"] for item in normalized_packages}) != len(normalized_packages):
            raise AbiBuildError("ABI component package names are duplicated")
        if normalized_artifacts != sorted(normalized_artifacts, key=lambda item: item["name"]):
            raise AbiBuildError("ABI component artifacts are not canonically ordered")
        if len({item["name"] for item in normalized_artifacts}) != len(normalized_artifacts):
            raise AbiBuildError("ABI component artifact names are duplicated")
        normalized_components.append(
            {
                "name": name,
                "upstream_version": upstream_version,
                "target": target,
                "packages": normalized_packages,
                "artifacts": normalized_artifacts,
            }
        )
    names = [component["name"] for component in normalized_components]
    if names != list(EXPECTED_COMPONENTS):
        raise AbiBuildError("ABI component inventory is incomplete or not canonically ordered")
    by_name = {component["name"]: component for component in normalized_components}
    for name in EXPECTED_COMPONENTS:
        if by_name[name]["target"] != EXPECTED_COMPONENT_TARGETS[name]:
            raise AbiBuildError(f"ABI component target is not canonical: {name}")
        artifact_names = tuple(artifact["name"] for artifact in by_name[name]["artifacts"])
        if artifact_names != EXPECTED_COMPONENT_ARTIFACTS[name]:
            raise AbiBuildError(f"ABI component artifact evidence is incomplete: {name}")
    if not str(by_name["gcc"]["upstream_version"]).startswith("13."):
        raise AbiBuildError("ABI compiler is not GCC 13")
    if by_name["libabigail"]["upstream_version"] != "2.4":
        raise AbiBuildError("ABI corpus requires libabigail 2.4")

    if identity["recipe"] != ABI_RECIPE:
        raise AbiBuildError("ABI build recipe is not canonical")
    expected_environment = environment_sha256(identity)
    if identity["environment_sha256"] != expected_environment:
        raise AbiBuildError("ABI environment digest does not match its evidence")

    outputs = _exact_mapping(identity["outputs"], _OUTPUT_FIELDS, "ABI outputs")
    expected_library_name = f"libmcp-cpp-sdk.so.{version.loader_identity}"
    expected_corpus_name = f"mcp-cpp-sdk-{version.core}-{ABI_BUILD_TUPLE}.abi.xml"
    if outputs["library_name"] != expected_library_name:
        raise AbiBuildError("ABI library SONAME does not match the source loader identity")
    if outputs["corpus_name"] != expected_corpus_name:
        raise AbiBuildError("ABI corpus name does not match the source version")
    needed = outputs["needed"]
    if (
        not isinstance(needed, list)
        or needed != sorted(needed)
        or len(needed) != len(set(needed))
        or any(not isinstance(name, str) or _SAFE_NAME.fullmatch(name) is None for name in needed)
    ):
        raise AbiBuildError("ABI ELF dependency inventory is malformed")
    for required in ("libc.so.6", "libcrypto.so.3", "libgcc_s.so.1", "libstdc++.so.6"):
        if required not in needed:
            raise AbiBuildError(f"ABI shared library does not depend on {required}")
    normalized_outputs = {
        "library_name": outputs["library_name"],
        "library_sha256": _digest(outputs["library_sha256"], "ABI library digest"),
        "corpus_name": outputs["corpus_name"],
        "corpus_sha256": _digest(outputs["corpus_sha256"], "ABI corpus digest"),
        "needed": list(needed),
    }
    return {
        "schema_version": 1,
        "kind": "abi-build-identity",
        "build_tuple": ABI_BUILD_TUPLE,
        "builder_image": builder_image,
        "source": {"tag": version.tag, "commit": source["commit"]},
        "platform": dict(platform),
        "components": normalized_components,
        "recipe": dict(ABI_RECIPE),
        "environment_sha256": expected_environment,
        "outputs": normalized_outputs,
    }


def require_compatible_environments(baseline: object, candidate: object) -> tuple[dict[str, object], dict[str, object]]:
    """Require byte-independent toolchain/dependency equivalence before abidiff."""

    baseline_identity = validate_build_identity(baseline)
    candidate_identity = validate_build_identity(candidate)
    if baseline_identity["environment_sha256"] != candidate_identity["environment_sha256"]:
        raise AbiBuildError(
            "baseline and candidate ABI corpora use different compiler, runtime, dependency, or tool identities"
        )
    try:
        baseline_version = Version.from_tag(baseline_identity["source"]["tag"], stable_only=True)
        candidate_version = Version.from_tag(candidate_identity["source"]["tag"], stable_only=True)
    except AbiPolicyError as error:
        raise AbiBuildError(f"ABI source version is invalid: {error}") from error
    if baseline_version.comparison_series != candidate_version.comparison_series:
        raise AbiBuildError("baseline and candidate identities are in different comparison series")
    return baseline_identity, candidate_identity


def validate_candidate_pair(
    identity_path: Path,
    corpus_path: Path,
    *,
    tag: str,
    commit: str,
) -> dict[str, object]:
    """Require one canonical identity to bind one candidate corpus and source."""

    try:
        raw_identity = json.loads(identity_path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AbiBuildError("candidate ABI build identity is not ASCII JSON") from error
    identity = validate_build_identity(raw_identity)
    if _canonical_json(identity) != identity_path.read_bytes():
        raise AbiBuildError("candidate ABI build identity JSON is not canonical")
    if identity["source"] != {"tag": tag, "commit": commit}:
        raise AbiBuildError("candidate ABI build identity does not bind the source tag and commit")
    if identity["outputs"]["corpus_name"] != corpus_path.name:
        raise AbiBuildError("candidate ABI build identity names a different corpus")
    if not corpus_path.is_file() or corpus_path.is_symlink():
        raise AbiBuildError("candidate ABI corpus is missing or unsafe")
    if identity["outputs"]["corpus_sha256"] != sha256_file(corpus_path):
        raise AbiBuildError("candidate ABI build identity does not bind the corpus digest")
    validate_corpus(corpus_path, forbidden_paths=())
    return identity


def validate_corpus(path: Path, *, forbidden_paths: Sequence[Path]) -> None:
    """Reject malformed or host-path-dependent abidw output."""

    if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
        raise AbiBuildError("abidw did not produce a regular non-empty corpus")
    data = path.read_bytes()
    for forbidden in forbidden_paths:
        resolved = str(forbidden.resolve()).encode()
        if resolved and resolved in data:
            raise AbiBuildError("ABI corpus contains a host-specific source or build path")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise AbiBuildError("ABI corpus is not well-formed XML") from error
    if root.tag.rsplit("}", 1)[-1] != "abi-corpus":
        raise AbiBuildError("abidw output is not an ABI corpus")


def _run(
    arguments: Sequence[str],
    *,
    cwd: Path | None = None,
    environment: Mapping[str, str] | None = None,
) -> str:
    return subprocess.run(
        list(arguments),
        cwd=cwd,
        env=dict(environment) if environment is not None else None,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=1800,
    ).stdout.strip()


def _os_release(path: Path = Path("/etc/os-release")) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if not raw_line or raw_line.startswith("#") or "=" not in raw_line:
            continue
        name, raw_value = raw_line.split("=", 1)
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
            raise AbiBuildError("/etc/os-release contains a malformed key")
        parsed = shlex.split(raw_value, posix=True)
        if len(parsed) != 1:
            raise AbiBuildError("/etc/os-release contains a malformed value")
        values[name] = parsed[0]
    return values


def collect_platform() -> dict[str, str]:
    os_release = _os_release()
    platform = {
        "os_id": os_release.get("ID", ""),
        "os_version_id": os_release.get("VERSION_ID", ""),
        "os_version_codename": os_release.get("VERSION_CODENAME", ""),
        "dpkg_architecture": _run(["dpkg", "--print-architecture"]),
        "uname_machine": _run(["uname", "-m"]),
    }
    if platform != EXPECTED_PLATFORM:
        raise AbiBuildError("ABI runner is not exact Ubuntu Noble amd64")
    return platform


def _package_owner(path: Path) -> str:
    output = _run(["dpkg-query", "--search", str(path.resolve())])
    owners = {
        line.split(": ", 1)[0].removesuffix(":amd64")
        for line in output.splitlines()
        if ": " in line
    }
    if len(owners) != 1:
        raise AbiBuildError(f"ABI input does not have one unambiguous package owner: {path.name}")
    return owners.pop()


@lru_cache(maxsize=None)
def _package_tree_sha256(package: str) -> str:
    paths = sorted(set(_run(["dpkg-query", "--listfiles", package]).splitlines()))
    digest = hashlib.sha256()
    members = 0
    for name in paths:
        path = Path(name)
        if path.is_symlink():
            record = f"L\0{name}\0{os.readlink(path)}\n".encode("utf-8")
        elif path.is_file():
            record = f"F\0{name}\0{sha256_file(path)}\n".encode("utf-8")
        else:
            continue
        digest.update(record)
        members += 1
    if members == 0:
        raise AbiBuildError(f"ABI package contains no hashable files: {package}")
    return digest.hexdigest()


@lru_cache(maxsize=None)
def _package_identity(package: str) -> dict[str, str]:
    fields = _run(
        [
            "dpkg-query",
            "--show",
            "--showformat=${binary:Package}\t${Version}\t${Architecture}",
            package,
        ]
    ).split("\t")
    if len(fields) != 3:
        raise AbiBuildError(f"ABI package metadata is incomplete: {package}")
    name = fields[0].removesuffix(":amd64")
    return {
        "name": name,
        "version": fields[1],
        "architecture": fields[2],
        "content_sha256": _package_tree_sha256(name),
    }


def _component(
    name: str,
    upstream_version: str,
    target: str,
    artifacts: Mapping[str, Path],
) -> dict[str, object]:
    owners = sorted({_package_owner(path) for path in artifacts.values()})
    return {
        "name": name,
        "upstream_version": upstream_version,
        "target": target,
        "packages": [_package_identity(owner) for owner in owners],
        "artifacts": [
            {"name": artifact_name, "sha256": sha256_file(path.resolve())}
            for artifact_name, path in sorted(artifacts.items())
        ],
    }


def _header_version(path: Path, names: Sequence[str]) -> str:
    text = path.read_text(encoding="utf-8")
    values: list[str] = []
    for name in names:
        match = re.search(rf"(?m)^\s*#\s*define\s+{re.escape(name)}\s+\"?([0-9_]+)\"?\s*$", text)
        if match is None:
            raise AbiBuildError(f"ABI dependency header does not define {name}")
        values.append(match.group(1))
    if len(values) == 1:
        parts = values[0].split("_")
        if len(parts) == 2:
            parts.append("0")
        return ".".join(str(int(part)) for part in parts)
    return ".".join(str(int(value)) for value in values)


def _abigail_version() -> str:
    output = _run(["abidw", "--version"])
    match = re.search(r"(?<![0-9])([0-9]+\.[0-9]+)(?![0-9])", output)
    if match is None:
        raise AbiBuildError("could not determine libabigail version")
    return match.group(1)


def collect_components(*, openssl_library: Path) -> list[dict[str, object]]:
    gxx = Path(shutil.which("g++-13") or "")
    cmake = Path(shutil.which("cmake") or "")
    abidw = Path(shutil.which("abidw") or "")
    abidiff = Path(shutil.which("abidiff") or "")
    if any(not path.is_file() for path in (gxx, cmake, abidw, abidiff)):
        raise AbiBuildError("canonical GCC, CMake, or libabigail executable is missing")
    gcc_version = _run([str(gxx), "-dumpfullversion", "-dumpversion"])
    gcc_target = _run([str(gxx), "-dumpmachine"])
    cc1plus = Path(_run([str(gxx), "-print-prog-name=cc1plus"]))
    libstdcxx = Path(_run([str(gxx), "-print-file-name=libstdc++.so.6"])).resolve()
    boost = Path("/usr/include/boost/version.hpp")
    nlohmann = Path("/usr/include/nlohmann/detail/abi_macros.hpp")
    if not nlohmann.is_file():
        nlohmann = Path("/usr/include/nlohmann/json.hpp")
    openssl = Path("/usr/include/openssl/opensslv.h")
    openssl_library = openssl_library.resolve()
    if any(
        not path.is_file()
        for path in (cc1plus, libstdcxx, boost, nlohmann, openssl, openssl_library)
    ):
        raise AbiBuildError("canonical ABI compiler, runtime, or dependency input is missing")
    openssl_version = _run(["pkg-config", "--modversion", "openssl"])
    cmake_version_line = _run([str(cmake), "--version"]).splitlines()[0]
    cmake_match = re.fullmatch(r"cmake version ([0-9][A-Za-z0-9.+~-]*)", cmake_version_line)
    if cmake_match is None:
        raise AbiBuildError("could not determine CMake version")
    components = [
        _component(
            "boost",
            _header_version(boost, ["BOOST_LIB_VERSION"]),
            "headers",
            {"boost-version.hpp": boost},
        ),
        _component("cmake", cmake_match.group(1), "x86_64-linux-gnu", {"cmake": cmake}),
        _component(
            "gcc",
            gcc_version,
            gcc_target,
            {"cc1plus": cc1plus, "g++-13": gxx},
        ),
        _component(
            "libabigail",
            _abigail_version(),
            "x86_64-linux-gnu",
            {"abidiff": abidiff, "abidw": abidw},
        ),
        _component(
            "libstdcxx",
            _package_identity(_package_owner(libstdcxx))["version"],
            "x86_64-linux-gnu",
            {"libstdc++.so.6": libstdcxx},
        ),
        _component(
            "nlohmann_json",
            _header_version(
                nlohmann,
                [
                    "NLOHMANN_JSON_VERSION_MAJOR",
                    "NLOHMANN_JSON_VERSION_MINOR",
                    "NLOHMANN_JSON_VERSION_PATCH",
                ],
            ),
            "headers",
            {"nlohmann-json-version.hpp": nlohmann},
        ),
        _component(
            "openssl",
            openssl_version,
            "x86_64-linux-gnu",
            {"libcrypto.so.3": openssl_library, "opensslv.h": openssl},
        ),
    ]
    return sorted(components, key=lambda item: str(item["name"]))


def _compile_commands_are_canonical(path: Path, *, source: Path, build: Path) -> None:
    try:
        commands = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AbiBuildError("CMake compile command evidence is unavailable") from error
    if not isinstance(commands, list) or not commands:
        raise AbiBuildError("CMake compile command evidence is empty")
    shared_commands = []
    source_map = f"-fdebug-prefix-map={source.resolve()}={ABI_RECIPE['source_prefix_map']}"
    build_map = f"-fdebug-prefix-map={build.resolve()}={ABI_RECIPE['build_prefix_map']}"
    source_file_map = f"-ffile-prefix-map={source.resolve()}={ABI_RECIPE['source_prefix_map']}"
    build_file_map = f"-ffile-prefix-map={build.resolve()}={ABI_RECIPE['build_prefix_map']}"
    for command in commands:
        if not isinstance(command, Mapping):
            raise AbiBuildError("compile_commands.json contains a malformed entry")
        if isinstance(command.get("arguments"), list):
            tokens = command["arguments"]
        elif isinstance(command.get("command"), str):
            tokens = shlex.split(command["command"])
        else:
            raise AbiBuildError("compile_commands.json entry has no command")
        if "-DMCP_BUILD_DLL" not in tokens:
            continue
        shared_commands.append(tokens)
        required = {*ABI_RECIPE["compile_options"], source_map, build_map, source_file_map, build_file_map}
        if not required <= set(tokens):
            raise AbiBuildError("shared-library compilation did not use the canonical ABI flags")
        compiler = Path(tokens[0]).resolve()
        expected_compiler = Path(shutil.which("g++-13") or "").resolve()
        if compiler != expected_compiler:
            raise AbiBuildError("shared-library compilation did not use g++-13")
    if not shared_commands:
        raise AbiBuildError("no shared SDK compilation was found in compile_commands.json")


def _resolved_openssl_library(cache_path: Path) -> Path:
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        declaration, value = line.split("=", 1)
        name, _kind = declaration.split(":", 1)
        if name in values:
            raise AbiBuildError("CMake cache contains a duplicate dependency field")
        values[name] = value
    for name in ("Boost_INCLUDE_DIR", "OPENSSL_INCLUDE_DIR"):
        if Path(values.get(name, "")).resolve() != Path("/usr/include"):
            raise AbiBuildError(f"CMake did not resolve {name} from the canonical system prefix")
    nlohmann_directory = Path(values.get("nlohmann_json_DIR", "")).resolve()
    try:
        nlohmann_directory.relative_to("/usr")
    except ValueError as error:
        raise AbiBuildError("CMake did not resolve nlohmann_json from the canonical system prefix") from error
    openssl_library = Path(values.get("OPENSSL_CRYPTO_LIBRARY", "")).resolve()
    try:
        openssl_library.relative_to("/usr/lib")
    except ValueError as error:
        raise AbiBuildError("CMake did not resolve libcrypto from the canonical system prefix") from error
    if openssl_library.name != "libcrypto.so.3" or not openssl_library.is_file():
        raise AbiBuildError("CMake did not resolve the canonical OpenSSL 3 shared library")
    return openssl_library


def _elf_identity(library: Path) -> tuple[str, list[str]]:
    dynamic = _run(["readelf", "--dynamic", "--wide", str(library)])
    sonames = re.findall(r"\(SONAME\).*\[([^\]]+)\]", dynamic)
    needed = sorted(set(re.findall(r"\(NEEDED\).*\[([^\]]+)\]", dynamic)))
    if len(sonames) != 1 or not needed:
        raise AbiBuildError("shared SDK ELF identity is incomplete")
    return sonames[0], needed


def build_corpus(
    *,
    source: Path,
    work: Path,
    output: Path,
    tag: str,
    commit: str,
    source_date_epoch: int,
    builder_image: str,
) -> tuple[Path, Path]:
    """Perform the canonical shared build and return corpus/identity paths."""

    try:
        version = Version.from_tag(tag, stable_only=True)
    except AbiPolicyError as error:
        raise AbiBuildError(f"ABI build tag is invalid: {error}") from error
    if _COMMIT.fullmatch(commit) is None:
        raise AbiBuildError("ABI candidate commit is malformed")
    if _BUILDER_IMAGE.fullmatch(builder_image) is None:
        raise AbiBuildError("ABI build requires an OCI image pinned by sha256 digest")
    if source_date_epoch < 1:
        raise AbiBuildError("SOURCE_DATE_EPOCH must be positive")
    source = source.resolve(strict=True)
    if (source / "VERSION").read_text(encoding="utf-8").strip() != version.core:
        raise AbiBuildError("tag does not match the checked-out VERSION")
    if _run(["git", "-C", str(source), "rev-parse", "HEAD"]) != commit:
        raise AbiBuildError("ABI source checkout does not match the selected commit")
    if _run(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=all"]):
        raise AbiBuildError("ABI source checkout is not clean")
    if work.exists() or output.exists():
        raise AbiBuildError("ABI work and output directories must be new")
    work.mkdir(parents=True)
    output.mkdir(parents=True)
    build = work / "build"
    stage = work / "stage"
    environment = {
        **{
            name: value
            for name, value in os.environ.items()
            if name
            not in {
                "BOOST_ROOT",
                "CMAKE_PREFIX_PATH",
                "CMAKE_TOOLCHAIN_FILE",
                "CPPFLAGS",
                "CXXFLAGS",
                "LDFLAGS",
                "OPENSSL_ROOT_DIR",
                "nlohmann_json_DIR",
            }
        },
        "CC": shutil.which("gcc-13") or "gcc-13",
        "CXX": shutil.which("g++-13") or "g++-13",
        "SOURCE_DATE_EPOCH": str(source_date_epoch),
    }
    source_map = f"-fdebug-prefix-map={source}={ABI_RECIPE['source_prefix_map']}"
    build_map = f"-fdebug-prefix-map={build}={ABI_RECIPE['build_prefix_map']}"
    source_file_map = f"-ffile-prefix-map={source}={ABI_RECIPE['source_prefix_map']}"
    build_file_map = f"-ffile-prefix-map={build}={ABI_RECIPE['build_prefix_map']}"
    flags = " ".join(["-O2", "-g", "-DNDEBUG", source_map, build_map, source_file_map, build_file_map])
    _run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            f"-DCMAKE_INSTALL_PREFIX={stage}",
            f"-DCMAKE_CXX_FLAGS_RELWITHDEBINFO={flags}",
            "-DCMAKE_CXX_STANDARD=20",
            "-DCMAKE_CXX_STANDARD_REQUIRED=ON",
            "-DCMAKE_CXX_EXTENSIONS=ON",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DMCP_CPP_SDK_BUILD_SHARED=ON",
            "-DMCP_CPP_SDK_BUILD_STATIC=OFF",
            "-DMCP_CPP_SDK_DEFAULT_LINKAGE=shared",
            "-DBUILD_TESTING=OFF",
            "-DBUILD_EXAMPLES=OFF",
            "-DBUILD_DOCS=OFF",
        ],
        environment=environment,
    )
    _run(["cmake", "--build", str(build), "--target", "mcp-cpp-sdk-shared"], environment=environment)
    _run(["cmake", "--install", str(build)], environment=environment)
    _compile_commands_are_canonical(build / "compile_commands.json", source=source, build=build)
    openssl_library = _resolved_openssl_library(build / "CMakeCache.txt")
    full_library_name = f"libmcp-cpp-sdk.so.{version.core}"
    libraries = [path for path in stage.glob(f"lib*/{full_library_name}") if path.is_file()]
    if len(libraries) != 1:
        raise AbiBuildError("canonical installed shared SDK library was not produced exactly once")
    library = libraries[0]
    soname, needed = _elf_identity(library)
    expected_soname = f"libmcp-cpp-sdk.so.{version.loader_identity}"
    if soname != expected_soname:
        raise AbiBuildError("shared SDK SONAME does not match the loader identity")
    corpus_name = f"mcp-cpp-sdk-{version.core}-{ABI_BUILD_TUPLE}.abi.xml"
    corpus = output / corpus_name
    include_directories = [path for path in stage.glob("include") if path.is_dir()]
    if len(include_directories) != 1:
        raise AbiBuildError("canonical installed SDK headers are missing")
    _run(
        [
            "abidw",
            "--no-corpus-path",
            "--no-comp-dir-path",
            "--headers-dir1",
            str(include_directories[0]),
            "--drop-private-types",
            "--out-file",
            str(corpus),
            str(library),
        ],
        environment=environment,
    )
    validate_corpus(corpus, forbidden_paths=(source, work))
    identity: dict[str, object] = {
        "schema_version": 1,
        "kind": "abi-build-identity",
        "build_tuple": ABI_BUILD_TUPLE,
        "builder_image": builder_image,
        "source": {"tag": version.tag, "commit": commit},
        "platform": collect_platform(),
        "components": collect_components(openssl_library=openssl_library),
        "recipe": dict(ABI_RECIPE),
        "environment_sha256": "",
        "outputs": {
            "library_name": soname,
            "library_sha256": sha256_file(library),
            "corpus_name": corpus.name,
            "corpus_sha256": sha256_file(corpus),
            "needed": needed,
        },
    }
    identity["environment_sha256"] = environment_sha256(identity)
    normalized = validate_build_identity(identity)
    identity_path = output / ABI_IDENTITY_NAME
    identity_path.write_bytes(_canonical_json(normalized))
    return corpus, identity_path


def _container_command_prefix(*, image: str, source: Path, io_directory: Path) -> list[str]:
    if _BUILDER_IMAGE.fullmatch(image) is None:
        raise AbiBuildError("ABI builder image must be pinned by sha256 digest")
    return [
        "docker",
        "run",
        "--rm",
        "--platform=linux/amd64",
        "--network=none",
        "--read-only",
        "--cap-drop=ALL",
        "--security-opt=no-new-privileges",
        "--pids-limit=1024",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "--workdir",
        "/source",
        "--env",
        "HOME=/tmp",
        "--env",
        "LANG=C.UTF-8",
        "--env",
        "LC_ALL=C.UTF-8",
        "--mount",
        f"type=bind,src={source},dst=/source,readonly",
        "--mount",
        f"type=bind,src={io_directory},dst=/release-io",
        "--tmpfs",
        "/tmp:rw,exec,nosuid,nodev,size=1g",
        image,
    ]


def container_build_command(
    *,
    image: str,
    source: Path,
    io_directory: Path,
    tag: str,
    commit: str,
    source_date_epoch: int,
) -> list[str]:
    """Return the locked-down Docker command used for the ABI builder."""

    return [
        *_container_command_prefix(image=image, source=source, io_directory=io_directory),
        "python3",
        "-I",
        "-S",
        "/source/scripts/run_release_tool.py",
        "release.abi_build",
        "build",
        "--source",
        "/source",
        "--work",
        "/release-io/work",
        "--output",
        "/release-io/output",
        "--tag",
        tag,
        "--commit",
        commit,
        "--source-date-epoch",
        str(source_date_epoch),
        "--builder-image",
        image,
    ]


def container_compare_command(
    *, image: str, source: Path, io_directory: Path, selection: BaselineSelection
) -> list[str]:
    """Return the locked-down command for an ABI comparison in the builder image."""

    if selection.baseline is None:
        raise AbiBuildError("a first comparison-series baseline must not run abidiff")
    baseline_name = (
        f"mcp-cpp-sdk-{selection.baseline.version.core}-{ABI_BUILD_TUPLE}.abi.xml"
    )
    candidate_name = f"mcp-cpp-sdk-{selection.current.core}-{ABI_BUILD_TUPLE}.abi.xml"
    return [
        *_container_command_prefix(image=image, source=source, io_directory=io_directory),
        "python3",
        "-I",
        "-S",
        "/source/scripts/run_release_tool.py",
        "release.abi_policy",
        "compare",
        "--selection",
        "/release-io/selection.json",
        "--baseline-corpus",
        f"/release-io/baseline/{baseline_name}",
        "--candidate-corpus",
        f"/release-io/output/{candidate_name}",
        "--baseline-identity",
        f"/release-io/baseline/{ABI_IDENTITY_NAME}",
        "--candidate-identity",
        f"/release-io/output/{ABI_IDENTITY_NAME}",
        "--report",
        "/release-io/evidence/abidiff-report.txt",
        "--result",
        "/release-io/evidence/abidiff-result.json",
        "--policy",
        "/source/release/abi-policy.json",
        "--output",
        "/release-io/evidence/abi-decision.json",
    ]


def _require_local_builder_image(image: str) -> None:
    inspected = subprocess.run(
        ["docker", "image", "inspect", "--format={{json .RepoDigests}}", image],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    ).stdout
    try:
        repo_digests = json.loads(inspected)
    except json.JSONDecodeError as error:
        raise AbiBuildError("Docker returned malformed builder image identity") from error
    expected_digest = image.rsplit("@", 1)[1]
    if (
        not isinstance(repo_digests, list)
        or not repo_digests
        or any(not isinstance(value, str) for value in repo_digests)
        or not any(value.endswith(f"@{expected_digest}") for value in repo_digests)
    ):
        raise AbiBuildError("local ABI builder image does not match the pinned OCI digest")


def build_in_pinned_container(
    *,
    image: str,
    source: Path,
    io_directory: Path,
    tag: str,
    commit: str,
    source_date_epoch: int,
) -> tuple[Path, Path]:
    """Pull an immutable image and run the canonical builder without network."""

    if _BUILDER_IMAGE.fullmatch(image) is None:
        raise AbiBuildError("ABI builder image must be pinned by sha256 digest")
    source = source.resolve(strict=True)
    if io_directory.exists():
        raise AbiBuildError("ABI container I/O directory must be new")
    io_directory.mkdir(parents=True)
    subprocess.run(
        ["docker", "pull", "--platform=linux/amd64", "--quiet", image],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=1800,
    )
    _require_local_builder_image(image)
    subprocess.run(
        container_build_command(
            image=image,
            source=source,
            io_directory=io_directory.resolve(),
            tag=tag,
            commit=commit,
            source_date_epoch=source_date_epoch,
        ),
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=3600,
    )
    output = io_directory / "output"
    version = Version.from_tag(tag, stable_only=True)
    corpus = output / f"mcp-cpp-sdk-{version.core}-{ABI_BUILD_TUPLE}.abi.xml"
    identity = output / ABI_IDENTITY_NAME
    validate_candidate_pair(identity, corpus, tag=tag, commit=commit)
    return corpus, identity


def compare_in_pinned_container(*, image: str, source: Path, io_directory: Path) -> None:
    """Compare downloaded and candidate corpora with the exact bound abidiff binary."""

    if _BUILDER_IMAGE.fullmatch(image) is None:
        raise AbiBuildError("ABI builder image must be pinned by sha256 digest")
    source = source.resolve(strict=True)
    io_directory = io_directory.resolve(strict=True)
    selection_path = io_directory / "selection.json"
    try:
        selection = BaselineSelection.from_mapping(
            json.loads(selection_path.read_text(encoding="ascii"))
        )
    except (OSError, UnicodeError, json.JSONDecodeError, AbiPolicyError) as error:
        raise AbiBuildError("ABI baseline selection is invalid") from error
    if selection.baseline is None:
        raise AbiBuildError("a first comparison-series baseline must not run abidiff")
    evidence = io_directory / "evidence"
    if evidence.exists():
        raise AbiBuildError("ABI comparison evidence directory must be new")
    evidence.mkdir()
    _require_local_builder_image(image)
    subprocess.run(
        container_compare_command(
            image=image,
            source=source,
            io_directory=io_directory,
            selection=selection,
        ),
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=3600,
    )
    for name in ("abidiff-report.txt", "abidiff-result.json", "abi-decision.json"):
        path = evidence / name
        if not path.is_file() or path.is_symlink():
            raise AbiBuildError(f"ABI comparison did not produce {name}")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    validate = commands.add_parser("validate-identity")
    validate.add_argument("--identity", type=Path, required=True)

    build = commands.add_parser("build")
    build.add_argument("--source", type=Path, default=Path("."))
    build.add_argument("--work", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--tag", required=True)
    build.add_argument("--commit", required=True)
    build.add_argument("--source-date-epoch", type=int, required=True)
    build.add_argument("--builder-image", required=True)

    container = commands.add_parser("container-build")
    container.add_argument("--builder-image", required=True)
    container.add_argument("--source", type=Path, default=Path("."))
    container.add_argument("--io-directory", type=Path, required=True)
    container.add_argument("--tag", required=True)
    container.add_argument("--commit", required=True)
    container.add_argument("--source-date-epoch", type=int, required=True)
    comparison = commands.add_parser("container-compare")
    comparison.add_argument("--builder-image", required=True)
    comparison.add_argument("--source", type=Path, default=Path("."))
    comparison.add_argument("--io-directory", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
        if args.command == "validate-identity":
            value = json.loads(args.identity.read_text(encoding="ascii"))
            normalized = validate_build_identity(value)
            if _canonical_json(normalized) != args.identity.read_bytes():
                raise AbiBuildError("ABI build identity JSON is not canonical")
        elif args.command == "build":
            build_corpus(
                source=args.source,
                work=args.work,
                output=args.output,
                tag=args.tag,
                commit=args.commit,
                source_date_epoch=args.source_date_epoch,
                builder_image=args.builder_image,
            )
        elif args.command == "container-build":
            build_in_pinned_container(
                image=args.builder_image,
                source=args.source,
                io_directory=args.io_directory,
                tag=args.tag,
                commit=args.commit,
                source_date_epoch=args.source_date_epoch,
            )
        else:
            compare_in_pinned_container(
                image=args.builder_image,
                source=args.source,
                io_directory=args.io_directory,
            )
    except (AbiBuildError, OSError, UnicodeError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"abi-build: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
