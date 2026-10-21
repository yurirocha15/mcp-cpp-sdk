from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from release.model import ValidationError
from release.native_builder_lock import load_builder_lock
from release.strict_json import load_strict, loads_strict


class StrictJsonTests(unittest.TestCase):
    def test_rejects_duplicate_keys_and_non_finite_numbers(self) -> None:
        for document, message in (
            ('{"value":1,"value":2}', "duplicate key"),
            ('{"outer":{"value":1,"value":2}}', "duplicate key"),
            ('{"value":NaN}', "non-finite number"),
            ('{"value":-Infinity}', "non-finite number"),
            ('{"value":1e400}', "non-finite number"),
            ('{"value":-1e400}', "non-finite number"),
        ):
            with self.subTest(document=document), self.assertRaisesRegex(
                ValidationError, message
            ):
                loads_strict(document, label="signed document")

    def test_rejects_malformed_json_and_invalid_utf8(self) -> None:
        for document in ('{"value":', b'{"value":"\xff"}'):
            with self.subTest(document=document), self.assertRaisesRegex(
                ValidationError, "malformed JSON"
            ):
                loads_strict(document, label="signed document")

    def test_accepts_text_and_bytes(self) -> None:
        self.assertEqual(loads_strict('{"value":[1,2]}', label="document"), {"value": [1, 2]})
        self.assertEqual(loads_strict(b'{"value":true}', label="document"), {"value": True})

    def test_file_loader_is_non_empty_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "document.json"
            path.write_bytes(b"")
            with self.assertRaisesRegex(ValidationError, "non-empty"):
                load_strict(path, label="signed document")
            path.write_text('{"value":1}\n', encoding="utf-8")
            self.assertEqual(
                load_strict(path, label="signed document"), {"value": 1}
            )
            with self.assertRaisesRegex(ValidationError, "at most"):
                load_strict(path, label="signed document", limit=4)

    def test_file_loader_requires_a_regular_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "document.json"
            target.write_text('{"value":1}\n', encoding="utf-8")
            link = root / "link.json"
            link.symlink_to(target)
            for path in (link, root, root / "missing.json"):
                with self.subTest(path=path.name), self.assertRaisesRegex(
                    ValidationError, "regular file"
                ):
                    load_strict(path, label="signed document")

    def test_builder_lock_rejects_duplicate_keys(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            path.write_text('{"schema_version":1,"schema_version":1}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValidationError, "native builder lock contains duplicate key"):
                load_builder_lock(path, require_resolved=False)


if __name__ == "__main__":
    unittest.main()
