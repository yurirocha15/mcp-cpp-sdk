from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.request import urlopen

from release.loopback_archive import archive_server
from release.model import ValidationError


class LoopbackArchiveTests(unittest.TestCase):
    def test_serves_only_the_exact_archive_on_loopback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "candidate archive.tar.gz"
            archive.write_bytes(b"candidate")
            with archive_server(archive) as url:
                self.assertTrue(url.startswith("http://127.0.0.1:"))
                with urlopen(url, timeout=2) as response:
                    self.assertEqual(response.read(), b"candidate")
                with self.assertRaises(HTTPError) as missing:
                    urlopen(url.rsplit("/", 1)[0] + "/other", timeout=2)
                self.assertEqual(missing.exception.code, 404)

    def test_rejects_empty_or_missing_archive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "empty.zip"
            archive.touch()
            with self.assertRaisesRegex(ValidationError, "must not be empty"):
                with archive_server(archive):
                    pass
            with self.assertRaisesRegex(ValidationError, "regular file"):
                with archive_server(archive.with_name("missing.zip")):
                    pass


if __name__ == "__main__":
    unittest.main()
