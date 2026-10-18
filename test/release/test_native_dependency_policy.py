from __future__ import annotations

from pathlib import Path
import unittest

from release.artifacts import public_dependency_closure
from release.model import ValidationError
from release.verify_candidate import validate_dependency_closure


ROOT = Path(__file__).resolve().parents[2]
PUBLIC_DEPENDENCY_CLOSURE = [
    {"name": "boost", "minimum": "1.74"},
    {"name": "nlohmann_json", "minimum": "3.10.5"},
    {"name": "openssl", "minimum": "3.0"},
]

class NativeDependencyPolicyTests(unittest.TestCase):
    def test_native_metadata_matches_the_public_cmake_link_interface(self) -> None:
        self.assertEqual(public_dependency_closure(), PUBLIC_DEPENDENCY_CLOSURE)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        public_interface = cmake.split(
            "function(mcp_cpp_sdk_configure_library target)", 1
        )[1].split("endfunction()", 1)[0]
        self.assertIn(
            "PUBLIC Boost::headers nlohmann_json::nlohmann_json\n"
            "                     OpenSSL::Crypto",
            public_interface,
        )
        self.assertNotIn("zlib", public_interface.lower())

        debian = (ROOT / "packaging/debian/control.in").read_text(encoding="utf-8")
        self.assertNotIn("zlib", debian.lower())
        self.assertEqual(
            next(line for line in debian.splitlines() if line.startswith("Build-Depends:")),
            "Build-Depends: debhelper-compat (= 13), cmake (>= 3.20), "
            "ninja-build, g++ (>= 11), libboost-dev (>= 1.74), libgtest-dev, "
            "nlohmann-json3-dev (>= 3.10.5), libssl-dev (>= 3.0), pkg-config",
        )
        self.assertEqual(
            [line for line in debian.splitlines() if line.startswith("Depends:")],
            [
                "Depends: ${shlibs:Depends}, ${misc:Depends}",
                "Depends: libmcp-cpp-sdk@ABI_VERSION@ (= ${binary:Version}), "
                "libboost-dev (>= 1.74), nlohmann-json3-dev (>= 3.10.5), "
                "libssl-dev (>= 3.0), ${misc:Depends}",
                "Depends: libmcp-cpp-sdk-dev (= ${binary:Version}), ${misc:Depends}",
            ],
        )

        rpm = (ROOT / "packaging/rpm/mcp-cpp-sdk.spec.in").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("zlib", rpm.lower())
        self.assertEqual(
            [line for line in rpm.splitlines() if line.startswith("BuildRequires:")],
            [
                "BuildRequires:  cmake >= 3.20",
                "BuildRequires:  gcc-c++ >= 11",
                "BuildRequires:  ninja-build",
                "BuildRequires:  boost-devel >= 1.74",
                "BuildRequires:  gtest-devel",
                "BuildRequires:  json-devel >= 3.10.5",
                "BuildRequires:  openssl-devel >= 3.0",
                "BuildRequires:  pkgconfig",
            ],
        )
        self.assertEqual(
            [line for line in rpm.splitlines() if line.startswith("Requires:")],
            [
                "Requires: mcp-cpp-sdk@ABI_VERSION@-libs%{?_isa} = "
                "%{version}-%{release}",
                "Requires: boost-devel >= 1.74",
                "Requires: json-devel >= 3.10.5",
                "Requires: openssl-devel >= 3.0",
                "Requires: %{name}-devel%{?_isa} = %{version}-%{release}",
            ],
        )

    def test_candidate_dependency_closure_is_exact_and_rejects_zlib(self) -> None:
        validate_dependency_closure(PUBLIC_DEPENDENCY_CLOSURE)
        mutations = (
            PUBLIC_DEPENDENCY_CLOSURE[:-1],
            list(reversed(PUBLIC_DEPENDENCY_CLOSURE)),
            [
                *PUBLIC_DEPENDENCY_CLOSURE,
                {"name": "zlib", "minimum": "1.2.11"},
            ],
            [
                *PUBLIC_DEPENDENCY_CLOSURE[:-1],
                {"name": "openssl", "minimum": "3.1"},
            ],
        )
        for value in mutations:
            with self.subTest(value=value), self.assertRaises(ValidationError):
                validate_dependency_closure(value)
