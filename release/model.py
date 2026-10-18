"""Strict, provider-independent release data models."""

from __future__ import annotations

from dataclasses import dataclass
import re


class ValidationError(ValueError):
    """Raised when release input is not canonical or violates policy."""


_SEMVER_RE = re.compile(
    r"(?P<major>0|[1-9][0-9]*)\."
    r"(?P<minor>0|[1-9][0-9]*)\."
    r"(?P<patch>0|[1-9][0-9]*)"
    r"(?:-rc\.(?P<rc>[1-9][0-9]*))?"
)


@dataclass(frozen=True)
class SemVer:
    """The project's deliberately narrow canonical SemVer profile."""

    major: int
    minor: int
    patch: int
    rc: int | None = None

    def __post_init__(self) -> None:
        if min(self.major, self.minor, self.patch) < 0 or (self.rc is not None and self.rc < 1):
            raise ValidationError("version components must be non-negative and RC numbers must be positive")

    @classmethod
    def parse(cls, value: str, *, stable_only: bool = False) -> "SemVer":
        if not isinstance(value, str) or len(value) > 64:
            raise ValidationError("version must be a string of at most 64 characters")
        match = _SEMVER_RE.fullmatch(value)
        if match is None:
            raise ValidationError("version must be MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-rc.N")
        version = cls(
            major=int(match.group("major")),
            minor=int(match.group("minor")),
            patch=int(match.group("patch")),
            rc=int(match.group("rc")) if match.group("rc") else None,
        )
        if stable_only and version.rc is not None:
            raise ValidationError("a stable version is required")
        return version

    @classmethod
    def from_tag(cls, tag: str, *, stable_only: bool = False) -> "SemVer":
        if not isinstance(tag, str) or not tag.startswith("v"):
            raise ValidationError("tag must start with v")
        return cls.parse(tag[1:], stable_only=stable_only)

    @property
    def is_prerelease(self) -> bool:
        return self.rc is not None

    @property
    def core(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    @property
    def tag(self) -> str:
        return f"v{self}"

    @property
    def debian_version(self) -> str:
        suffix = f"~rc.{self.rc}" if self.rc is not None else ""
        return f"{self.core}{suffix}-1"

    @property
    def rpm_version_release(self) -> tuple[str, str]:
        release = f"0.{self.rc}.rc.{self.rc}" if self.rc is not None else "1"
        return self.core, release

    @property
    def arch_pkgver(self) -> str:
        suffix = f"rc{self.rc}" if self.rc is not None else ""
        return f"{self.core}{suffix}"

    def __str__(self) -> str:
        suffix = f"-rc.{self.rc}" if self.rc is not None else ""
        return f"{self.core}{suffix}"
