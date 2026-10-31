from __future__ import annotations

import json
from pathlib import Path
import unittest

from release.model import ValidationError
from release.publication_contract import load_publication_contract, parse_publication_contract


# The contract signed into the v0.2.0 release, byte for byte. It still carries the
# historical ``provider_controls`` field, and signed contracts cannot be reissued.
V0_2_0_CONTRACT = Path(__file__).parent / "fixtures/v0.2.0-release-publication-contract.json"


class PublicationContractCompatibilityTests(unittest.TestCase):
    def contract(self) -> dict[str, object]:
        return json.loads(V0_2_0_CONTRACT.read_text(encoding="utf-8"))

    def test_signed_v0_2_0_contract_loads(self) -> None:
        self.assertIn("provider_controls", self.contract())
        contract = load_publication_contract(V0_2_0_CONTRACT)
        self.assertEqual(
            contract.destinations["chocolatey"]["repository"],
            "yurirocha15/mcp-cpp-sdk-chocolatey-publisher",
        )

    def test_contract_without_provider_controls_parses(self) -> None:
        value = self.contract()
        del value["provider_controls"]
        parse_publication_contract(value)

    def test_unknown_or_missing_fields_are_rejected(self) -> None:
        extra = self.contract()
        extra["provider_controls_v2"] = {}
        missing = self.contract()
        del missing["destinations"]
        for value in (extra, missing):
            with self.subTest(fields=sorted(value)), self.assertRaisesRegex(
                ValidationError, "schema is not exact"
            ):
                parse_publication_contract(value)


if __name__ == "__main__":
    unittest.main()
