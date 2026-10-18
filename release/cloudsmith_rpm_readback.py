"""Pinned-key and immutable-content verification for Cloudsmith RPM readback."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from pathlib import Path
import re
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


GPG_FINGERPRINT = re.compile(r"[0-9A-F]{40}")
SHA256 = re.compile(r"[0-9a-f]{64}")
MAX_GPG_KEY_BYTES = 1024 * 1024
MAX_RPM_SIGNATURE_GROWTH = 1024 * 1024
RPM_QUERY = (
    "%{NAME}\\t%{EPOCHNUM}\\t%{VERSION}\\t%{RELEASE}\\t%{ARCH}\\t"
    "%{SHA256HEADER}\\t%{PAYLOADDIGEST}\\n"
)
RPM_VERSION = "RPM version 4.18.2"


class CloudsmithPublishError(ValueError):
    """Raised when repository or route state is ambiguous or conflicting."""


class _NoRedirects(HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, new_url):
        return None


def download(url: str, destination: Path, maximum_bytes: int) -> None:
    """Download one exact HTTPS object without redirects or unbounded buffering."""

    request = Request(url, headers={"User-Agent": "mcp-cpp-sdk-release/1"})
    with build_opener(_NoRedirects).open(request, timeout=120) as response:
        if response.status != 200:
            raise CloudsmithPublishError("Cloudsmith download did not return HTTP 200")
        written = 0
        with destination.open("xb") as output:
            while chunk := response.read(1024 * 1024):
                written += len(chunk)
                if written > maximum_bytes:
                    raise CloudsmithPublishError("Cloudsmith download exceeds its size limit")
                output.write(chunk)


def repository_key_url(namespace: str, repository: str, fingerprint: str) -> str:
    if GPG_FINGERPRINT.fullmatch(fingerprint) is None:
        raise CloudsmithPublishError("Cloudsmith repository GPG fingerprint is invalid")
    return (
        f"https://dl.cloudsmith.io/public/{namespace}/{repository}/cfg/gpg/"
        f"gpg.{fingerprint[-16:]}.key"
    )


def rpm_download_url(record: Mapping[str, object], *, coordinate: str) -> str:
    value = record.get("cdn_url")
    if not isinstance(value, str):
        raise CloudsmithPublishError("Cloudsmith completed RPM has no download URL")
    parsed = urlsplit(value)
    try:
        port = parsed.port
    except ValueError as error:
        raise CloudsmithPublishError("Cloudsmith RPM download URL is malformed") from error
    prefix = f"/public/{coordinate}/"
    if (
        parsed.scheme != "https"
        or parsed.hostname != "dl.cloudsmith.io"
        or port not in {None, 443}
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or not parsed.path.startswith(prefix)
        or "%" in parsed.path
        or "/../" in parsed.path
    ):
        raise CloudsmithPublishError("Cloudsmith RPM download URL is outside the repository")
    return value


def _public_key_fingerprint(key: Path, *, run: Callable[[list[str]], str]) -> str:
    listing = run(
        ["gpg", "--batch", "--show-keys", "--with-colons", "--fingerprint", str(key)]
    )
    fingerprints: list[str] = []
    awaiting_primary = False
    for line in listing.splitlines():
        fields = line.split(":")
        if fields[0] == "pub":
            awaiting_primary = True
        elif fields[0] == "fpr" and awaiting_primary:
            if len(fields) <= 9 or GPG_FINGERPRINT.fullmatch(fields[9]) is None:
                raise CloudsmithPublishError("Cloudsmith repository GPG key is malformed")
            fingerprints.append(fields[9])
            awaiting_primary = False
    if len(fingerprints) != 1:
        raise CloudsmithPublishError("Cloudsmith repository GPG key is missing or ambiguous")
    return fingerprints[0]


def download_repository_key(
    *,
    namespace: str,
    repository: str,
    fingerprint: str,
    destination: Path,
    run: Callable[[list[str]], str],
    download_file: Callable[[str, Path, int], None],
) -> None:
    download_file(
        repository_key_url(namespace, repository, fingerprint),
        destination,
        MAX_GPG_KEY_BYTES,
    )
    if _public_key_fingerprint(destination, run=run) != fingerprint:
        raise CloudsmithPublishError("Cloudsmith repository GPG key fingerprint changed")


def _rpm_identity(path: Path, *, run: Callable[[list[str]], str]) -> tuple[str, ...]:
    fields = run(["rpm", "-qp", "--qf", RPM_QUERY, str(path)]).rstrip("\n").split("\t")
    if (
        len(fields) != 7
        or any(not field or "\n" in field or "\r" in field for field in fields)
        or any(SHA256.fullmatch(field.lower()) is None for field in fields[-2:])
    ):
        raise CloudsmithPublishError("RPM immutable identity is malformed")
    return tuple(field.lower() if index >= 5 else field for index, field in enumerate(fields))


def verify_rpm_tools(*, run: Callable[[list[str]], str]) -> None:
    if run(["rpm", "--version"]).strip() != RPM_VERSION:
        raise CloudsmithPublishError(f"{RPM_VERSION} is required for RPM readback")
    if run(["rpmkeys", "--version"]).strip() != RPM_VERSION:
        raise CloudsmithPublishError(f"{RPM_VERSION} rpmkeys is required for signature checks")


def verify_signed_rpm(
    *,
    local: Path,
    published: Path,
    repository_key: Path,
    repository_key_fingerprint: str,
    work: Path,
    run: Callable[[list[str]], str],
) -> None:
    database = work / "rpmdb"
    database.mkdir()
    run(["rpm", "--dbpath", str(database), "--initdb"])
    run(["rpmkeys", "--dbpath", str(database), "--import", str(repository_key)])
    signature_status = run(
        ["rpmkeys", "--dbpath", str(database), "--checksig", "--verbose", str(published)]
    )
    key_ids = {
        repository_key_fingerprint[-8:].lower(),
        repository_key_fingerprint[-16:].lower(),
    }
    if not any(
        re.search(rf"(?i)signature.*key id {key_id}.*:\s*OK(?:\s|$)", signature_status)
        for key_id in key_ids
    ):
        raise CloudsmithPublishError("Cloudsmith RPM lacks the pinned repository signature")
    if _rpm_identity(local, run=run) != _rpm_identity(published, run=run):
        raise CloudsmithPublishError("Cloudsmith-signed RPM changed immutable package content")
