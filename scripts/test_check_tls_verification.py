"""Tests for check_tls_verification.py."""

import tempfile
import unittest
from pathlib import Path

import check_tls_verification


def write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class ViolationsTest(unittest.TestCase):
    def test_the_factory_may_set_verification(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(root, "src/transport/tls.cpp", "SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);\n")
            self.assertEqual(check_tls_verification.violations(root), [])

    def test_a_setting_elsewhere_is_reported_with_its_place(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(root, "src/transport/http_client.cpp", "ok();\ncontext.set_verify_mode(verify_none);\n")
            write(root, "include/mcp/transport/tls.hpp", "// SSL_VERIFY_NONE is never used\n")
            write(root, "examples/client.cpp", "SSL_set_verify(ssl, 0, nullptr);\n")
            self.assertEqual(
                check_tls_verification.violations(root),
                [
                    "examples/client.cpp:1: SSL_set_verify(ssl, 0, nullptr);",
                    "include/mcp/transport/tls.hpp:1: // SSL_VERIFY_NONE is never used",
                    "src/transport/http_client.cpp:2: context.set_verify_mode(verify_none);",
                ],
            )

    def test_tests_and_other_files_are_not_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(root, "test/transport/tls_test.cpp", "context.set_verify_mode(verify_none);\n")
            write(root, "src/notes.md", "set_verify_mode\n")
            self.assertEqual(check_tls_verification.violations(root), [])

    def test_main_fails_on_a_violation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(root, "src/a.cpp", "SSL_CTX_set_cert_verify_callback(context, nullptr, nullptr);\n")
            self.assertEqual(check_tls_verification.main(["check", str(root)]), 1)
            (root / "src/a.cpp").write_text("int main() {}\n", encoding="utf-8")
            self.assertEqual(check_tls_verification.main(["check", str(root)]), 0)


if __name__ == "__main__":
    unittest.main()
