#!/usr/bin/env python3
"""Run a reviewed release module with Python isolated from ambient imports."""

from __future__ import annotations

from pathlib import Path
import runpy
import sys


_ALLOWED_MODULES = frozenset({"release.native_builder_bootstrap"})


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in _ALLOWED_MODULES:
        allowed = ", ".join(sorted(_ALLOWED_MODULES))
        raise SystemExit(f"usage: run_release_tool.py <module> [args]; allowed: {allowed}")
    module = sys.argv.pop(1)
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root))
    runpy.run_module(module, run_name="__main__", alter_sys=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
