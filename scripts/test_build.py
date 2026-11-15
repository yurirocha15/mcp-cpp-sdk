#!/usr/bin/env python3
"""Tests for the helpers in scripts/build.py."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build  # noqa: E402


class CcacheSloppinessTests(unittest.TestCase):
    def test_adds_the_precompiled_header_settings(self) -> None:
        self.assertEqual(build.ccache_pch_sloppiness(""),
                         "pch_defines,time_macros,include_file_mtime,include_file_ctime")

    def test_keeps_the_settings_already_made_without_repeating_any(self) -> None:
        self.assertEqual(build.ccache_pch_sloppiness("locale, time_macros\n"),
                         "locale,time_macros,pch_defines,include_file_mtime,include_file_ctime")


if __name__ == "__main__":
    unittest.main()
