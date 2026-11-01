#!/usr/bin/env python3
"""RQ-20260917-01 slice 1: 网络库当前不可达 ledger keys, trilingual, fmt order."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LEDGER = ROOT / "locale" / "harken.ui.strings.json"
CJK_RE = re.compile(r"[一-鿿]")
FMT_RE = re.compile(r"%(?:ll)?[sd]")


class NetworkUnreachableLocaleContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        payload = json.loads(LEDGER.read_text(encoding="utf-8"))
        cls.by_id = {entry["id"]: entry for entry in payload["entries"]}

    def test_status_copy_exists_trilingual_and_exact_zh(self) -> None:
        entry = self.by_id.get("Scan_NetworkLibraryUnreachable")
        self.assertIsNotNone(entry, "missing Scan_NetworkLibraryUnreachable ledger key")
        assert entry is not None
        self.assertEqual(entry["zh"], "网络库当前不可达")
        self.assertTrue(entry["en"].strip(), "en column must be filled")
        self.assertTrue(entry["ja"].strip(), "ja column must be filled")
        self.assertFalse(
            CJK_RE.search(entry["en"]),
            "en column must not contain Chinese characters",
        )
        self.assertNotEqual(entry["ja"], entry["zh"], "ja must not be leftover Chinese")
        self.assertEqual(FMT_RE.findall(entry["zh"]), FMT_RE.findall(entry["en"]))
        self.assertEqual(FMT_RE.findall(entry["zh"]), FMT_RE.findall(entry["ja"]))

    def test_network_result_uses_the_shared_failure_count_format(self) -> None:
        # The compact result groups network failures under paths; full network
        # reasons remain available in the separately translated details.
        entry = self.by_id.get("Main_FailedFmt")
        self.assertIsNotNone(entry, "missing shared result failure-count ledger key")
        assert entry is not None
        zh_fmt = FMT_RE.findall(entry["zh"])
        self.assertEqual(zh_fmt, ["%d"])
        self.assertEqual(zh_fmt, FMT_RE.findall(entry["en"]))
        self.assertEqual(zh_fmt, FMT_RE.findall(entry["ja"]))
        self.assertTrue(entry["zh"].strip())
        self.assertTrue(entry["en"].strip())
        self.assertTrue(entry["ja"].strip())
        self.assertFalse(CJK_RE.search(entry["en"]))
        self.assertNotEqual(entry["ja"], entry["zh"])
        for key in ("ScanUi_NetworkTitle", "ScanUi_NetworkAction", "ScanUi_FailedPath"):
            with self.subTest(key=key):
                message = self.by_id[key]
                self.assertTrue(all(message.get(language, "").strip() for language in ("zh", "en", "ja")))
                self.assertFalse(CJK_RE.search(message["en"]))


if __name__ == "__main__":
    unittest.main()
