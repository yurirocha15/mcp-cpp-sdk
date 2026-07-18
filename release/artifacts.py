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
import stat
import tarfile
import tempfile
from typing import Any, Iterable, Sequence
from urllib.parse import urlsplit
import zipfile

from .model import SemVer, ValidationError


CONTROL_ASSET_NAMES = frozenset(
    {"release-manifest.json", "release-manifest.json.asc", "SHA256SUMS", "SHA256SUMS.asc"}
)


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
        if not self.name or PurePosixPath(self.name).name != self.name or self.name in CONTROL_ASSET_NAMES:
            raise ValidationError(f"invalid payload asset name: {self.name}")
        if self.size < 0:
            raise ValidationError("payload asset size must be non-negative")
        if len(self.sha256) != 64 or any(character not in "0123456789abcdef" for character in self.sha256):
            raise ValidationError("payload asset SHA-256 is not canonical")
        if not self.role:
            raise ValidationError("payload asset role must not be empty")

    @classmethod
    def from_path(cls, path: Path, *, role: str, build_tuple: str | None = None) -> "ArtifactRecord":
        return cls(path.name, path.stat().st_size, sha256_file(path), role, build_tuple)

    def to_mapping(self) -> dict[str, Any]:
        value: dict[str, Any] = {"name": self.name, "size": self.size, "sha256": self.sha256, "role": self.role}
        if self.build_tuple is not None:
            value["build_tuple"] = self.build_tuple
        return value


def build_release_manifest(
    *,
    version: SemVer,
    tag: str,
    commit: str,
    source_tree_sha256: str,
    ledger_issue_id: str,
    ledger_issue_url: str,
    primary_fingerprint: str,
    tag_subkey_fingerprint: str,
    artifact_subkey_fingerprint: str,
    payloads: Iterable[ArtifactRecord],
    dependency_closure: Sequence[dict[str, Any]],
    provenance_subjects: Sequence[dict[str, str]],
) -> dict[str, Any]:
    if tag != version.tag:
        raise ValidationError("manifest tag and version disagree")
    _require_hex("commit", commit, 40, lowercase=True)
    _require_hex("source_tree_sha256", source_tree_sha256, 64, lowercase=True)
    if not ledger_issue_id.isdecimal() or ledger_issue_id.startswith("0"):
        raise ValidationError("ledger issue ID must be a positive canonical decimal")
    parsed_issue_url = urlsplit(ledger_issue_url)
    expected_path = f"/yurirocha15/mcp-cpp-sdk/issues/{ledger_issue_id}"
    if (
        parsed_issue_url.scheme != "https"
        or parsed_issue_url.netloc != "github.com"
        or parsed_issue_url.path != expected_path
        or parsed_issue_url.query
        or parsed_issue_url.fragment
    ):
        raise ValidationError("ledger issue URL is not the expected canonical GitHub URL")
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
    return {
        "schema_version": 1,
        "package": "mcp-cpp-sdk",
        "version": str(version),
        "tag": tag,
        "commit": commit,
        "source_tree_sha256": source_tree_sha256,
        "release_ledger": {"issue_id": ledger_issue_id, "issue_url": ledger_issue_url},
        "signers": {
            "primary_fingerprint": primary_fingerprint,
            "tag_subkey_fingerprint": tag_subkey_fingerprint,
            "artifact_subkey_fingerprint": artifact_subkey_fingerprint,
        },
        "payloads": [record.to_mapping() for record in records],
        "dependency_closure": list(dependency_closure),
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
