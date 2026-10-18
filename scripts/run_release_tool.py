#!/usr/bin/env python3
"""Run a reviewed release module with Python isolated from ambient imports."""

from __future__ import annotations

from pathlib import Path
import re
import runpy
import subprocess
import sys


_ALLOWED_MODULES = frozenset(
    {
        "release.assemble_unsigned",
        "release.aur_preflight",
        "release.aur_ssh",
        "release.aur_validation",
        "release.cloudsmith_publish",
        "release.conan_validation",
        "release.aur_publish",
        "release.construct_core",
        "release.github_publication",
        "release.github_anchor",
        "release.github_provider",
        "release.native_builder",
        "release.native_builder_bootstrap",
        "release.package_validation",
        "release.provider_preflight",
        "release.publication_contract",
        "release.repository_readiness",
        "release.signing",
        "release.verify_candidate",
        "release.workflow_gate",
    }
)
_GH_MODULES = frozenset(
    {
        "release.github_anchor",
        "release.github_provider",
        "release.github_publication",
        "release.provider_preflight",
        "release.repository_readiness",
    }
)
_GH_MINIMUM_VERSION = (2, 96, 0)


def _verify_github_cli() -> None:
    try:
        completed = subprocess.run(
            ["gh", "--version"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise SystemExit("the pinned GitHub CLI is unavailable") from error
    first_line = completed.stdout.splitlines()[0] if completed.stdout else ""
    words = first_line.split()
    match = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)", words[2]) if len(words) > 2 else None
    if (
        words[:2] != ["gh", "version"]
        or match is None
        or tuple(int(part) for part in match.groups()) < _GH_MINIMUM_VERSION
    ):
        minimum = ".".join(str(part) for part in _GH_MINIMUM_VERSION)
        raise SystemExit(f"GitHub CLI {minimum} or newer is required")


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in _ALLOWED_MODULES:
        allowed = ", ".join(sorted(_ALLOWED_MODULES))
        raise SystemExit(f"usage: run_release_tool.py <module> [args]; allowed: {allowed}")
    module = sys.argv.pop(1)
    if module in _GH_MODULES:
        _verify_github_cli()
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root))
    runpy.run_module(module, run_name="__main__", alter_sys=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
