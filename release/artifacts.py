"""Deterministic source archives, manifests, checksums, and SBOM documents."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile
import tempfile
from typing import Any, Iterable, Sequence
import zipfile

from .model import SemVer, ValidationError


CONTROL_ASSET_NAMES = frozenset(
    {"release-manifest.json", "release-manifest.json.asc", "SHA256SUMS", "SHA256SUMS.asc"}
)
STABLE_CHANNEL_CAPABILITIES = (
    "github",
    "conan2",
    "apt",
    "rpm",
    "aur",
    "homebrew",
    "chocolatey",
)
CONAN_REQUIREMENT_NAMES = ("boost", "nlohmann_json", "openssl")
PUBLIC_LINK_DEPENDENCIES = (
    ("boost", "1.74"),
    ("nlohmann_json", "3.10.5"),
    ("openssl", "3.0"),
)
CONAN_REFERENCE_RE = re.compile(r"[a-z0-9_+.-]+/[0-9][A-Za-z0-9+_.-]*")
BUILD_TUPLE_RE = re.compile(r"[a-z0-9][a-z0-9._+-]{0,127}")
TARGET_BASE_FIELDS = frozenset({"id", "format", "distribution", "release", "architecture", "runner"})
TARGET_APT_FIELDS = TARGET_BASE_FIELDS | frozenset(
    {
        "builder_os_id", "builder_os_version_id", "builder_os_version_codename",
        "builder_dpkg_architecture", "builder_uname_machine",
    }
)
TARGET_RPM_FIELDS = TARGET_BASE_FIELDS | frozenset(
    {
        "builder_os_id", "builder_os_version_id", "builder_rpm_fedora",
        "builder_rpm_rhel", "builder_rpm_dist", "builder_rpm_architecture",
        "builder_uname_machine",
    }
)
ROUTE_FIELDS = frozenset(
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
ABI_BUILD_TUPLE = "ubuntu-noble-amd64-gcc13-libstdcxx-abigail2.4"
ABI_BUILD_IDENTITY_NAME = f"build-identity-{ABI_BUILD_TUPLE}.json"
WINDOWS_BUILD_TUPLE = "windows-x64-v143-md"


def public_dependency_closure() -> list[dict[str, str]]:
    return [
        {"name": name, "minimum": minimum}
        for name, minimum in PUBLIC_LINK_DEPENDENCIES
    ]


def canonical_json_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _sha1_file(path: Path) -> str:
    digest = hashlib.sha1(usedforsecurity=False)
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    except BaseException:
        Path(temporary_name).unlink(missing_ok=True)
        raise


class SourceInventory:
    """A reviewed allow-list of exact files and recursive directory entries."""

    def __init__(self, entries: Sequence[str]) -> None:
        if not entries:
            raise ValidationError("source archive inventory is empty")
        normalized = [self._validate_entry(entry) for entry in entries]
        if len(set(normalized)) != len(normalized):
            raise ValidationError("source archive inventory contains duplicates")
        self.entries = tuple(normalized)

    @classmethod
    def from_file(cls, path: Path) -> "SourceInventory":
        entries = []
        for raw_line in path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if line and not line.startswith("#"):
                entries.append(line)
        return cls(entries)

    @staticmethod
    def _validate_entry(entry: str) -> str:
        if not isinstance(entry, str) or not entry or "\\" in entry:
            raise ValidationError("inventory paths must be non-empty POSIX paths")
        recursive = entry.endswith("/**")
        base = entry[:-3] if recursive else entry
        path = PurePosixPath(base)
        if path.is_absolute() or base in {"", "."} or ".." in path.parts:
            raise ValidationError(f"unsafe inventory entry: {entry}")
        if any(part in {".git", ".github", ".release-local"} for part in path.parts):
            raise ValidationError(f"forbidden inventory entry: {entry}")
        return f"{path.as_posix()}/**" if recursive else path.as_posix()

    def expand(self, root: Path) -> tuple[Path, ...]:
        resolved_root = root.resolve(strict=True)
        selected: dict[str, Path] = {}
        for entry in self.entries:
            recursive = entry.endswith("/**")
            relative = entry[:-3] if recursive else entry
            candidate = root / relative
            if recursive:
                if not candidate.is_dir() or candidate.is_symlink():
                    raise ValidationError(f"inventory directory is missing or unsafe: {relative}")
                paths = sorted((path for path in candidate.rglob("*") if path.is_file()), key=lambda path: path.as_posix())
                if not paths:
                    raise ValidationError(f"inventory directory has no files: {relative}")
            else:
                paths = [candidate]
            for path in paths:
                if path.is_symlink() or not path.is_file():
                    raise ValidationError(f"inventory member is missing or not a regular file: {path}")
                resolved = path.resolve(strict=True)
                try:
                    member = resolved.relative_to(resolved_root).as_posix()
                except ValueError as error:
                    raise ValidationError(f"inventory member escapes the source root: {path}") from error
                selected[member] = resolved
        return tuple(selected[name] for name in sorted(selected))


def _normalized_mode(path: Path) -> int:
    return 0o755 if path.stat().st_mode & stat.S_IXUSR else 0o644


def build_source_archives(
    *, root: Path, inventory: SourceInventory, version: SemVer, output_dir: Path, source_date_epoch: int
) -> tuple[Path, Path]:
    if source_date_epoch < 0:
        raise ValidationError("SOURCE_DATE_EPOCH must be non-negative")
    members = inventory.expand(root)
    version_path = (root / "VERSION").resolve()
    if version_path not in members:
        raise ValidationError("source archive inventory must include VERSION")
    embedded_version = version_path.read_text(encoding="utf-8").strip()
    if embedded_version != str(version):
        raise ValidationError("requested version does not match the source VERSION file")
    prefix = f"mcp-cpp-sdk-{version}"
    tar_path = output_dir / f"{prefix}.tar.gz"
    zip_path = output_dir / f"{prefix}.zip"

    tar_buffer = io.BytesIO()
    with tarfile.open(fileobj=tar_buffer, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for path in members:
            relative = path.resolve().relative_to(root.resolve()).as_posix()
            info = archive.gettarinfo(str(path), arcname=f"{prefix}/{relative}")
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            info.mtime = source_date_epoch
            info.mode = _normalized_mode(path)
            info.pax_headers = {}
            with path.open("rb") as source:
                archive.addfile(info, source)
    compressed = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=compressed, mtime=source_date_epoch, compresslevel=9) as output:
        output.write(tar_buffer.getvalue())
    write_atomic(tar_path, compressed.getvalue())

    zip_buffer = io.BytesIO()
    zip_time = datetime.fromtimestamp(max(source_date_epoch, 315532800), timezone.utc)
    zip_tuple = (zip_time.year, zip_time.month, zip_time.day, zip_time.hour, zip_time.minute, zip_time.second)
    with zipfile.ZipFile(zip_buffer, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in members:
            relative = path.resolve().relative_to(root.resolve()).as_posix()
            info = zipfile.ZipInfo(f"{prefix}/{relative}", date_time=zip_tuple)
            info.create_system = 3
            info.external_attr = (_normalized_mode(path) | stat.S_IFREG) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())
    write_atomic(zip_path, zip_buffer.getvalue())
    return tar_path, zip_path


@dataclass(frozen=True)
class ArtifactRecord:
    name: str
    size: int
    sha256: str
    role: str
    build_tuple: str | None = None

    def __post_init__(self) -> None:
        if (
            not self.name
            or PurePosixPath(self.name).name != self.name
            or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}", self.name) is None
            or self.name in CONTROL_ASSET_NAMES
        ):
            raise ValidationError(f"invalid payload asset name: {self.name}")
        if type(self.size) is not int or self.size <= 0:
            raise ValidationError("payload asset size must be a positive integer")
        if len(self.sha256) != 64 or any(character not in "0123456789abcdef" for character in self.sha256):
            raise ValidationError("payload asset SHA-256 is not canonical")
        if not self.role:
            raise ValidationError("payload asset role must not be empty")
        if self.build_tuple is not None and BUILD_TUPLE_RE.fullmatch(self.build_tuple) is None:
            raise ValidationError("payload build tuple is not canonical")

    @classmethod
    def from_path(cls, path: Path, *, role: str, build_tuple: str | None = None) -> "ArtifactRecord":
        return cls(path.name, path.stat().st_size, sha256_file(path), role, build_tuple)

    def to_mapping(self) -> dict[str, Any]:
        value: dict[str, Any] = {"name": self.name, "size": self.size, "sha256": self.sha256, "role": self.role}
        if self.build_tuple is not None:
            value["build_tuple"] = self.build_tuple
        return value

    @classmethod
    def from_mapping(cls, value: Any) -> "ArtifactRecord":
        if not isinstance(value, dict) or not {"name", "size", "sha256", "role"} <= set(value):
            raise ValidationError("payload record is malformed")
        if set(value) - {"name", "size", "sha256", "role", "build_tuple"}:
            raise ValidationError("payload record contains unknown fields")
        if not all(isinstance(value[field], str) for field in ("name", "sha256", "role")):
            raise ValidationError("payload record string fields are malformed")
        build_tuple = value.get("build_tuple")
        if build_tuple is not None and not isinstance(build_tuple, str):
            raise ValidationError("payload build tuple is malformed")
        return cls(value["name"], value["size"], value["sha256"], value["role"], build_tuple)


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
        if not isinstance(value, dict) or value.get("format") not in {"apt", "rpm"}:
            raise ValidationError("native target schema is not exact")
        fields = TARGET_APT_FIELDS if value["format"] == "apt" else TARGET_RPM_FIELDS
        if set(value) != fields:
            raise ValidationError("native target schema is not exact")
        empty_allowed = {"builder_rpm_fedora", "builder_rpm_rhel"}
        if any(
            not isinstance(value[field], str) or (not value[field] and field not in empty_allowed)
            for field in fields
        ):
            raise ValidationError("native target fields are malformed")
        target = cls(
            **{field: value[field] for field in TARGET_BASE_FIELDS},
            builder_os_id=value["builder_os_id"],
            builder_os_version_id=value["builder_os_version_id"],
            builder_uname_machine=value["builder_uname_machine"],
            builder_os_version_codename=value.get("builder_os_version_codename"),
            builder_dpkg_architecture=value.get("builder_dpkg_architecture"),
            builder_rpm_fedora=value.get("builder_rpm_fedora"),
            builder_rpm_rhel=value.get("builder_rpm_rhel"),
            builder_rpm_dist=value.get("builder_rpm_dist"),
            builder_rpm_architecture=value.get("builder_rpm_architecture"),
        )
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", target.id):
            raise ValidationError("native target ID is unsafe")
        expected_id = f"{target.distribution}-{target.release}-{target.architecture}"
        expected_runner = (
            "ubuntu-24.04-arm"
            if target.builder_uname_machine == "aarch64"
            else "ubuntu-24.04"
        )
        if target.id != expected_id or target.runner != expected_runner:
            raise ValidationError("native target identity is noncanonical")
        expected_architectures = {"apt": {"amd64", "arm64"}, "rpm": {"x86_64", "aarch64"}}
        if target.architecture not in expected_architectures[target.format]:
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
        if not isinstance(value, dict) or set(value) != ROUTE_FIELDS:
            raise ValidationError("native route schema is not exact")
        if not all(isinstance(value[field], str) and value[field] for field in ROUTE_FIELDS):
            raise ValidationError("native route fields must be non-empty strings")
        route = cls(**value)
        if PurePosixPath(route.asset).name != route.asset or route.asset.count("--") != 1:
            raise ValidationError("native route asset name is unsafe")
        if not route.asset.startswith(f"{route.route_id}--"):
            raise ValidationError("native route asset is not bound to its target")
        extension = ".deb" if route.format == "apt" else ".rpm"
        if route.format not in {"apt", "rpm"} or not route.asset.endswith(extension):
            raise ValidationError("native route asset extension does not match its format")
        expected_tuple = f"{route.format}-{route.route_id}"
        if route.build_tuple != expected_tuple:
            raise ValidationError("native route build tuple does not match its target")
        if route.identity_asset != f"build-identity-{route.route_id}.json":
            raise ValidationError("native route identity asset does not match its target")
        return route

    def to_mapping(self) -> dict[str, str]:
        return {
            "asset": self.asset,
            "format": self.format,
            "route_id": self.route_id,
            "distribution": self.distribution,
            "release": self.release,
            "target_architecture": self.target_architecture,
            "package_name": self.package_name,
            "package_version": self.package_version,
            "package_architecture": self.package_architecture,
            "build_tuple": self.build_tuple,
            "identity_asset": self.identity_asset,
        }


def load_native_targets(path: Path) -> tuple[NativeTarget, ...]:
    from .build_identity import (
        CURRENT_NATIVE_TARGET_IDS,
        BuildIdentityError,
        load_and_validate_target_projection,
    )

    try:
        value = load_and_validate_target_projection(path)
    except BuildIdentityError as error:
        raise ValidationError(f"native target inventory is invalid: {error}") from error
    targets = tuple(NativeTarget.from_mapping(item) for item in value)
    if len({target.id for target in targets}) != len(targets):
        raise ValidationError("native target inventory contains duplicate IDs")
    if tuple(target.id for target in targets) != CURRENT_NATIVE_TARGET_IDS:
        raise ValidationError("native target inventory is incomplete or noncanonical")
    return targets


def load_conan_requirements(path: Path) -> tuple[str, ...]:
    """Load the exact Conan references used to render and verify the recipe."""
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or tuple(value) != CONAN_REQUIREMENT_NAMES:
        raise ValidationError("Conan requirement inventory is incomplete or noncanonical")
    requirements = tuple(value[name] for name in CONAN_REQUIREMENT_NAMES)
    if any(
        not isinstance(reference, str) or CONAN_REFERENCE_RE.fullmatch(reference) is None
        for reference in requirements
    ):
        raise ValidationError("Conan requirement reference is malformed")
    if tuple(sorted(requirements)) != requirements or len(set(requirements)) != len(requirements):
        raise ValidationError("Conan requirement references are not uniquely ordered")
    return requirements


def _expected_fixed_payload_roles(version: "SemVer") -> dict[str, str]:
    prefix = f"mcp-cpp-sdk-{version}"
    common = {
        f"{prefix}.tar.gz": "source-or-binary-archive",
        f"{prefix}.zip": "source-or-binary-archive",
        f"{prefix}.spdx.json": "sbom",
        f"{prefix}.cdx.json": "sbom",
        "release-signing-key.asc": "publisher-input",
    }
    if version.is_prerelease:
        return common
    return {
        **common,
        "aur-PKGBUILD": "publisher-input",
        "aur-SRCINFO": "publisher-input",
        "homebrew-mcp-cpp-sdk.rb": "publisher-input",
        "conan-recipe-config-entry.json": "publisher-input",
        "conan-recipe-conandata-entry.json": "publisher-input",
        "conan-recipe-conanfile.py": "publisher-input",
        "conan-recipe-test-CMakeLists.txt": "publisher-input",
        "conan-recipe-test-conanfile.py": "publisher-input",
        "conan-recipe-test-test_package.cpp": "publisher-input",
        "conan-source.json": "publisher-input",
        "cloudsmith-routes.json": "publisher-input",
        "release-publication-contract.json": "publisher-input",
        f"{prefix}-windows-x64-v143-md.zip": "source-or-binary-archive",
        f"mcp-cpp-sdk.{version}.nupkg": "native-package",
        f"{prefix}-{ABI_BUILD_TUPLE}.abi.xml": "abi-corpus",
        ABI_BUILD_IDENTITY_NAME: "abi-build-identity",
        "build-identity-windows-x64-v143-md.json": "build-identity",
    }


def _expected_route_packages(version: "SemVer", target: NativeTarget) -> set[tuple[str, str, str]]:
    if target.format == "apt":
        return {
            (f"libmcp-cpp-sdk{version.abi_version}", version.debian_version, target.architecture),
            ("libmcp-cpp-sdk-dev", version.debian_version, target.architecture),
            ("libmcp-cpp-sdk-static-dev", version.debian_version, target.architecture),
        }
    rpm_version, rpm_release = version.rpm_version_release
    dist_suffix = f".fc{target.release}" if target.distribution == "fedora" else f".el{target.release}"
    package_version = f"{rpm_version}-{rpm_release}{dist_suffix}"
    packages = {
        (f"mcp-cpp-sdk{version.abi_version}-libs", package_version, target.architecture),
        ("mcp-cpp-sdk-devel", package_version, target.architecture),
        ("mcp-cpp-sdk-static", package_version, target.architecture),
    }
    if target.architecture == "x86_64":
        packages.add(("mcp-cpp-sdk", package_version, "src"))
    return packages


def validate_candidate_inventory(
    *,
    version: "SemVer",
    payloads: Iterable[ArtifactRecord],
    routes: Iterable[NativeRoute | dict[str, Any]],
    targets: Iterable[NativeTarget | dict[str, Any]],
    provenance_subjects: Sequence[dict[str, Any]],
) -> None:
    records = tuple(payloads)
    record_map = {record.name: record for record in records}
    if len(record_map) != len(records):
        raise ValidationError("candidate payload names are duplicated")
    target_values = tuple(
        target if isinstance(target, NativeTarget) else NativeTarget.from_mapping(target)
        for target in targets
    )
    route_values = tuple(
        route if isinstance(route, NativeRoute) else NativeRoute.from_mapping(route)
        for route in routes
    )
    expected_roles = _expected_fixed_payload_roles(version)
    native_names = {route.asset for route in route_values}
    identity_names = (
        set() if version.is_prerelease
        else {f"build-identity-{target.id}.json" for target in target_values}
    )
    expected_names = set(expected_roles) | native_names | identity_names
    if set(record_map) != expected_names:
        raise ValidationError("candidate payload inventory is incomplete or contains unexpected assets")
    for name, role in expected_roles.items():
        if record_map[name].role != role:
            raise ValidationError(f"candidate payload role is incorrect: {name}")
    if any(record_map[name].role != "native-package" for name in native_names):
        raise ValidationError("native route assets must use the native-package role")
    if any(record_map[name].role != "build-identity" for name in identity_names):
        raise ValidationError("native build identity assets must use the build-identity role")

    expected_provenance = [
        {"name": record.name, "digest": {"sha256": record.sha256}}
        for record in sorted(records, key=lambda item: item.name)
    ]
    if list(provenance_subjects) != expected_provenance:
        raise ValidationError("provenance subjects do not exactly match candidate payloads")

    if version.is_prerelease:
        if route_values:
            raise ValidationError("release candidates cannot contain native publication routes")
        return

    abi_name = f"mcp-cpp-sdk-{version}-{ABI_BUILD_TUPLE}.abi.xml"
    windows_names = {
        f"mcp-cpp-sdk-{version}-windows-x64-v143-md.zip",
        f"mcp-cpp-sdk.{version}.nupkg",
        "build-identity-windows-x64-v143-md.json",
    }
    if record_map[abi_name].build_tuple != ABI_BUILD_TUPLE:
        raise ValidationError("ABI corpus is not bound to the canonical build tuple")
    if record_map[ABI_BUILD_IDENTITY_NAME].build_tuple != ABI_BUILD_TUPLE:
        raise ValidationError("ABI build identity is not bound to the canonical build tuple")
    if any(record_map[name].build_tuple != WINDOWS_BUILD_TUPLE for name in windows_names):
        raise ValidationError("Windows payload is not bound to the canonical build tuple")

    target_map = {target.id: target for target in target_values}
    if len(target_map) != len(target_values) or len(target_map) != 18:
        raise ValidationError("stable native target inventory is incomplete or duplicated")
    if len({route.asset for route in route_values}) != len(route_values):
        raise ValidationError("native route assets are duplicated")
    packages_by_target: dict[str, set[tuple[str, str, str]]] = {
        target_id: set() for target_id in target_map
    }
    for route in route_values:
        target = target_map.get(route.route_id)
        if target is None:
            raise ValidationError("native route references an unknown target")
        if (
            route.format != target.format
            or route.distribution != target.distribution
            or route.release != target.release
            or route.target_architecture != target.architecture
        ):
            raise ValidationError("native route coordinates conflict with its target")
        if record_map[route.asset].build_tuple != route.build_tuple:
            raise ValidationError("native package build tuple conflicts with its route")
        identity = record_map.get(route.identity_asset)
        if identity is None or identity.build_tuple != route.build_tuple:
            raise ValidationError("native package lacks its signed build identity")
        package = (route.package_name, route.package_version, route.package_architecture)
        if package in packages_by_target[target.id]:
            raise ValidationError("native route package identity is duplicated")
        packages_by_target[target.id].add(package)
    for target_id, target in target_map.items():
        if packages_by_target[target_id] != _expected_route_packages(version, target):
            raise ValidationError(f"native route package inventory is incomplete: {target_id}")


def build_release_manifest(
    *,
    version: SemVer,
    tag: str,
    commit: str,
    source_tree_sha256: str,
    primary_fingerprint: str,
    tag_subkey_fingerprint: str,
    artifact_subkey_fingerprint: str,
    channel_capabilities: Sequence[str],
    payloads: Iterable[ArtifactRecord],
    dependency_closure: Sequence[dict[str, Any]],
    conan_requirements: Sequence[str],
    provenance_subjects: Sequence[dict[str, str]],
) -> dict[str, Any]:
    if tag != version.tag:
        raise ValidationError("manifest tag and version disagree")
    _require_hex("commit", commit, 40, lowercase=True)
    _require_hex("source_tree_sha256", source_tree_sha256, 64, lowercase=True)
    for name, fingerprint in (
        ("primary_fingerprint", primary_fingerprint),
        ("tag_subkey_fingerprint", tag_subkey_fingerprint),
        ("artifact_subkey_fingerprint", artifact_subkey_fingerprint),
    ):
        _require_hex(name, fingerprint, (40, 64), lowercase=False)
    records = sorted(payloads, key=lambda item: item.name)
    names = [record.name for record in records]
    if len(names) != len(set(names)) or any(name in CONTROL_ASSET_NAMES for name in names):
        raise ValidationError("payload names must be unique and exclude control assets")
    expected_capabilities = ("github",) if version.is_prerelease else STABLE_CHANNEL_CAPABILITIES
    if tuple(channel_capabilities) != expected_capabilities:
        raise ValidationError("manifest channel capabilities are incomplete or noncanonical")
    if (
        not conan_requirements
        or tuple(sorted(set(conan_requirements))) != tuple(conan_requirements)
        or any(
            not isinstance(reference, str) or CONAN_REFERENCE_RE.fullmatch(reference) is None
            for reference in conan_requirements
        )
    ):
        raise ValidationError("manifest Conan requirements are incomplete or noncanonical")
    return {
        "schema_version": 2,
        "package": "mcp-cpp-sdk",
        "version": str(version),
        "tag": tag,
        "commit": commit,
        "source_tree_sha256": source_tree_sha256,
        "signers": {
            "primary_fingerprint": primary_fingerprint,
            "tag_subkey_fingerprint": tag_subkey_fingerprint,
            "artifact_subkey_fingerprint": artifact_subkey_fingerprint,
        },
        "channel_capabilities": list(channel_capabilities),
        "payloads": [record.to_mapping() for record in records],
        "dependency_closure": list(dependency_closure),
        "conan_requirements": list(conan_requirements),
        "provenance_subjects": list(provenance_subjects),
    }


def _require_hex(name: str, value: str, length: int | tuple[int, ...], *, lowercase: bool) -> None:
    lengths = (length,) if isinstance(length, int) else length
    alphabet = "0123456789abcdef" if lowercase else "0123456789ABCDEF"
    if len(value) not in lengths or any(character not in alphabet for character in value):
        case = "lowercase" if lowercase else "uppercase"
        raise ValidationError(f"{name} must be full {case} hexadecimal")


def build_sha256sums(paths: Iterable[Path]) -> bytes:
    records: dict[str, str] = {}
    for path in paths:
        if path.name in {"SHA256SUMS", "SHA256SUMS.asc"}:
            raise ValidationError("SHA256SUMS cannot hash itself or its signature")
        if path.name in records:
            raise ValidationError(f"duplicate checksum asset name: {path.name}")
        records[path.name] = sha256_file(path)
    return "".join(f"{records[name]}  {name}\n" for name in sorted(records)).encode()


def build_sboms(
    *, version: SemVer, files: Sequence[Path], root: Path, source_date_epoch: int, namespace_base: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    created = datetime.fromtimestamp(source_date_epoch, timezone.utc).isoformat().replace("+00:00", "Z")
    root_resolved = root.resolve()
    components = []
    spdx_files = []
    verification_hashes = []
    for index, path in enumerate(sorted(files, key=lambda item: item.as_posix()), start=1):
        relative = path.resolve().relative_to(root_resolved).as_posix()
        digest = sha256_file(path)
        verification_hashes.append(_sha1_file(path))
        spdx_files.append(
            {
                "SPDXID": f"SPDXRef-File-{index}",
                "fileName": relative,
                "checksums": [{"algorithm": "SHA256", "checksumValue": digest}],
                "licenseConcluded": "NOASSERTION",
                "copyrightText": "NOASSERTION",
            }
        )
        components.append({"type": "file", "name": relative, "hashes": [{"alg": "SHA-256", "content": digest}]})
    namespace = f"{namespace_base.rstrip('/')}/mcp-cpp-sdk/{version}"
    spdx = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"mcp-cpp-sdk-{version}",
        "documentNamespace": namespace,
        "creationInfo": {"created": created, "creators": ["Tool: mcp-cpp-sdk-release-tooling"]},
        "packages": [
            {
                "SPDXID": "SPDXRef-Package",
                "name": "mcp-cpp-sdk",
                "versionInfo": str(version),
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "packageVerificationCode": {"packageVerificationCodeValue": _verification_code(verification_hashes)},
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "Apache-2.0",
                "copyrightText": "NOASSERTION",
            }
        ],
        "files": spdx_files,
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-Package"},
            *(
                {"spdxElementId": "SPDXRef-Package", "relationshipType": "CONTAINS", "relatedSpdxElement": item["SPDXID"]}
                for item in spdx_files
            ),
        ],
    }
    cyclonedx = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{_stable_uuid(namespace)}",
        "version": 1,
        "metadata": {
            "timestamp": created,
            "component": {
                "type": "library",
                "name": "mcp-cpp-sdk",
                "version": str(version),
                "licenses": [{"license": {"id": "Apache-2.0"}}],
            },
        },
        "components": components,
    }
    return spdx, cyclonedx


def _stable_uuid(value: str) -> str:
    digest = bytearray(hashlib.sha256(value.encode()).digest()[:16])
    digest[6] = (digest[6] & 0x0F) | 0x50
    digest[8] = (digest[8] & 0x3F) | 0x80
    hexadecimal = digest.hex()
    return f"{hexadecimal[:8]}-{hexadecimal[8:12]}-{hexadecimal[12:16]}-{hexadecimal[16:20]}-{hexadecimal[20:]}"


def _verification_code(file_hashes: Sequence[str]) -> str:
    return hashlib.sha1("".join(sorted(file_hashes)).encode(), usedforsecurity=False).hexdigest()
