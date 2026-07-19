from __future__ import annotations

import base64
import hashlib
import struct
import unittest

from release.aur_ssh import AurSshError, validate_known_hosts


def key_material(fill: int = 7) -> tuple[str, str]:
    algorithm = b"ssh-ed25519"
    blob = struct.pack(">I", len(algorithm)) + algorithm + struct.pack(">I", 32) + bytes([fill]) * 32
    encoded = base64.b64encode(blob).decode("ascii")
    digest = base64.b64encode(hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")
    return encoded, f"SHA256:{digest}"


class AurSshKnownHostsTests(unittest.TestCase):
    def test_accepts_only_the_exact_ed25519_host_key(self) -> None:
        encoded, fingerprint = key_material()
        line = f"aur.archlinux.org ssh-ed25519 {encoded}\n"
        self.assertEqual(
            validate_known_hosts(line, expected_fingerprint=fingerprint),
            line,
        )

    def test_rejects_wrong_host_key_extra_entries_and_noncanonical_forms(self) -> None:
        encoded, fingerprint = key_material()
        other, _ = key_material(8)
        valid = f"aur.archlinux.org ssh-ed25519 {encoded}"
        invalid = (
            f"evil.example ssh-ed25519 {encoded}",
            f"aur.archlinux.org ssh-ed25519 {other}",
            f"aur.archlinux.org,1.2.3.4 ssh-ed25519 {encoded}",
            f"|1|hash|host ssh-ed25519 {encoded}",
            f"aur.archlinux.org ssh-rsa {encoded}",
            valid + " comment",
            valid + "\n" + valid,
            valid + "\n\n",
        )
        for value in invalid:
            with self.subTest(value=value[:50]), self.assertRaises(AurSshError):
                validate_known_hosts(value, expected_fingerprint=fingerprint)

    def test_rejects_declared_algorithm_that_differs_from_blob(self) -> None:
        encoded, fingerprint = key_material()
        rsa_blob = base64.b64encode(
            struct.pack(">I", len(b"ssh-rsa")) + b"ssh-rsa" + b"payload"
        ).decode("ascii")
        with self.assertRaises(AurSshError):
            validate_known_hosts(
                f"aur.archlinux.org ssh-ed25519 {rsa_blob}",
                expected_fingerprint=fingerprint,
            )


if __name__ == "__main__":
    unittest.main()
