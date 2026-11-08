#!/usr/bin/env python3
"""RQ-20260724-04: JSON ledger gates (keys / generated table drift)."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

LEDGER_PATH = ROOT / "locale" / "harken.ui.strings.json"
GENERATOR_PATH = ROOT / "tools" / "generate_localization.py"
FROZEN_ZH = {
    "View_InfoPanel": "Info",
    "View_DemoWindow": "Demo窗口",
    "Panel_InfoDetach": "Info独立窗口",
    "Set_UILanguage": "界面语言 / Language:",
}


class LocalizationLedgerContractTests(unittest.TestCase):
    def test_ledger_and_generator_exist(self) -> None:
        self.assertTrue(GENERATOR_PATH.is_file(), "tools/generate_localization.py is required")
        self.assertTrue(LEDGER_PATH.is_file(), "locale/harken.ui.strings.json is the only copy ledger")

    def test_missing_key_fails(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_File", "type": "ui", "zh": "文件", "en": "File"},
            ],
        }
        with self.assertRaises(generate_localization.LedgerError) as raised:
            generate_localization.check_keys(
                generate_localization.normalize_ledger(payload),
                expected_ids=["Menu_File", "Menu_View"],
            )
        self.assertIn("Menu_View", str(raised.exception))

    def test_wrong_key_fails(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_Nope", "type": "ui", "zh": "文件", "en": "File"},
            ],
        }
        with self.assertRaises(generate_localization.LedgerError) as raised:
            generate_localization.check_keys(
                generate_localization.normalize_ledger(payload),
                expected_ids=["Menu_File"],
            )
        self.assertIn("Menu_Nope", str(raised.exception))

    def test_call_site_missing_from_ledger_fails(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_File", "type": "ui", "zh": "文件", "en": "File"},
            ],
        }
        ledger = generate_localization.normalize_ledger(payload)
        with self.assertRaises(generate_localization.LedgerError) as raised:
            generate_localization.check_call_sites(ledger, ["Menu_File", "Menu_View"])
        self.assertIn("Menu_View", str(raised.exception))

    def test_unreferenced_ledger_entry_fails(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_File", "type": "ui", "zh": "文件", "en": "File"},
                {"id": "Dead_UnusedCopy", "type": "ui", "zh": "死条目", "en": "Dead"},
            ],
        }
        ledger = generate_localization.normalize_ledger(payload)
        with self.assertRaises(generate_localization.LedgerError) as raised:
            generate_localization.check_unreferenced_ids(ledger, ["Menu_File"])
        self.assertIn("Dead_UnusedCopy", str(raised.exception))

    def test_lw_call_is_a_live_reference(self) -> None:
        import generate_localization

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            src = root / "src"
            src.mkdir()
            (src / "NativeDialog.cpp").write_text(
                "SetWindowTextW(hwnd, Lw(Consent_Title).c_str());\n",
                encoding="utf-8",
            )
            used = generate_localization.collect_used_lstr_ids(root)
        self.assertIn("Consent_Title", used)

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Consent_Title", "type": "ui", "zh": "许可", "en": "Consent"},
            ],
        }
        ledger = generate_localization.normalize_ledger(payload)
        generate_localization.check_unreferenced_ids(ledger, used)

    def test_included_cpp_fragment_is_a_live_reference(self) -> None:
        import generate_localization

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            src = root / "src"
            src.mkdir()
            (src / "SettingsDialog.inl").write_text(
                "L(Set_UILanguage); Lw(Consent_Title); LStr::Set_ThemeDark;\n",
                encoding="utf-8",
            )
            self.assertEqual(generate_localization.collect_used_lstr_ids(root),
                             ["Consent_Title", "Set_ThemeDark", "Set_UILanguage"])

    def test_missing_translation_fails_generator(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_File", "type": "ui", "zh": "文件", "en": "File"},
                {"id": "Menu_View", "type": "ui", "zh": "视图", "en": ""},
            ],
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            ledger = Path(temp_dir) / "ledger.json"
            ledger.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
            with self.assertRaises(generate_localization.LedgerError) as raised:
                generate_localization.validate_ledger(generate_localization.load_ledger(ledger))
            self.assertIn("Menu_View", str(raised.exception))
            self.assertRegex(str(raised.exception), r"en|英文")

    def test_generated_table_drift_fails(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_File", "type": "ui", "zh": "文件", "en": "File"},
                {"id": "Set_ThemeDark", "type": "theme", "zh": "经典深色", "en": "Classic Dark"},
                {"id": "Set_ThemeOcean", "type": "theme", "zh": "极光青", "en": "Aurora Teal"},
                {"id": "Set_ThemeRoseCedar", "type": "theme", "zh": "柔雾粉", "en": "Soft Blush"},
                {"id": "Set_ThemeCustom", "type": "theme", "zh": "自定义", "en": "Custom"},
                {"id": "Set_ThemeLapisBlue", "type": "theme", "zh": "深海军蓝", "en": "Deep Navy"},
                {"id": "Set_ThemeJadeMist", "type": "theme", "zh": "海玻璃", "en": "Sea Glass"},
                {"id": "Set_DevModeEnabled", "type": "beta-hidden", "zh": "dev", "en": "dev"},
            ],
        }
        ledger = generate_localization.normalize_ledger(payload)
        with tempfile.TemporaryDirectory() as temp_dir:
            out = Path(temp_dir)
            generate_localization.write_generated(ledger, out)
            drifted = (out / "LocalizationStrings.generated.h").read_text(encoding="utf-8")
            (out / "LocalizationStrings.generated.h").write_text(
                drifted.replace("文件", "档案"),
                encoding="utf-8",
            )
            with self.assertRaises(generate_localization.LedgerError) as raised:
                generate_localization.check_generated_matches_ledger(ledger, out)
            self.assertRegex(str(raised.exception), r"drift|漂移")

    def test_optional_ja_defaults_to_empty_and_keeps_zh_en(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_File", "type": "ui", "zh": "文件", "en": "File"},
            ],
        }
        ledger = generate_localization.normalize_ledger(payload)
        generate_localization.validate_ledger(ledger)
        self.assertEqual(ledger["entries"][0]["zh"], "文件")
        self.assertEqual(ledger["entries"][0]["en"], "File")
        self.assertEqual(ledger["entries"][0]["ja"], "")

    def test_generated_ja_table_and_drift(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Menu_File", "type": "ui", "zh": "文件", "en": "File", "ja": "ファイル"},
                {"id": "Set_ThemeDark", "type": "theme", "zh": "经典深色", "en": "Classic Dark"},
                {"id": "Set_ThemeOcean", "type": "theme", "zh": "极光青", "en": "Aurora Teal"},
                {"id": "Set_ThemeRoseCedar", "type": "theme", "zh": "柔雾粉", "en": "Soft Blush"},
                {"id": "Set_ThemeCustom", "type": "theme", "zh": "自定义", "en": "Custom"},
                {"id": "Set_ThemeLapisBlue", "type": "theme", "zh": "深海军蓝", "en": "Deep Navy"},
                {"id": "Set_ThemeJadeMist", "type": "theme", "zh": "海玻璃", "en": "Sea Glass"},
                {"id": "Set_DevModeEnabled", "type": "beta-hidden", "zh": "dev", "en": "dev"},
            ],
        }
        ledger = generate_localization.normalize_ledger(payload)
        with tempfile.TemporaryDirectory() as temp_dir:
            out = Path(temp_dir)
            generate_localization.write_generated(ledger, out)
            generated = (out / "LocalizationStrings.generated.h").read_text(encoding="utf-8")
            self.assertIn("static const char* g_strings_ja[]", generated)
            self.assertIn("ファイル", generated)
            generate_localization.check_generated_matches_ledger(ledger, out)
            (out / "LocalizationStrings.generated.h").write_text(
                generated.replace("ファイル", "档案"),
                encoding="utf-8",
            )
            with self.assertRaises(generate_localization.LedgerError) as raised:
                generate_localization.check_generated_matches_ledger(ledger, out)
            self.assertRegex(str(raised.exception), r"drift|漂移")

    def test_explicit_empty_auth_contact_admin_is_allowed(self) -> None:
        import generate_localization

        payload = {
            "schema_version": 1,
            "entries": [
                {"id": "Auth_ContactAdmin", "type": "ui", "zh": "", "en": ""},
            ],
        }
        ledger = generate_localization.normalize_ledger(payload)
        generate_localization.validate_ledger(ledger)
        self.assertEqual(ledger["entries"][0]["zh"], "")
        self.assertEqual(ledger["entries"][0]["en"], "")

    def test_repo_ledger_matches_generated_tables_and_frozen_labels(self) -> None:
        import generate_localization

        self.assertTrue(LEDGER_PATH.is_file())
        ledger = generate_localization.normalize_ledger(generate_localization.load_ledger(LEDGER_PATH))
        generate_localization.validate_ledger(ledger)
        ids = [entry["id"] for entry in ledger["entries"]]
        used_ids = generate_localization.collect_used_lstr_ids(ROOT)
        generate_localization.check_call_sites(ledger, used_ids)
        generate_localization.check_unreferenced_ids(ledger, used_ids)
        self.assertEqual(set(ids), set(used_ids) | generate_localization.PROTECTED_LEDGER_IDS)
        with tempfile.TemporaryDirectory() as temp_dir:
            out = Path(temp_dir)
            generate_localization.write_generated(ledger, out)
            generate_localization.check_generated_matches_ledger(ledger, out)
            header_ids = generate_localization.parse_lstr_ids(
                (out / "LocalizationGenerated.h").read_text(encoding="utf-8")
            )
        self.assertEqual(ids, header_ids)
        self.assertTrue(used_ids)
        self.assertTrue(set(used_ids) <= set(ids))
        self.assertIn("Auth_ContactAdmin", ids)
        auth = next(entry for entry in ledger["entries"] if entry["id"] == "Auth_ContactAdmin")
        self.assertEqual(auth["zh"], "")
        self.assertEqual(auth["en"], "")
        types = {entry["type"] for entry in ledger["entries"]}
        self.assertTrue({"ui", "theme", "beta-hidden"} <= types)
        theme_ids = [entry["id"] for entry in ledger["entries"] if entry["type"] == "theme"]
        self.assertEqual(
            theme_ids,
            [
                "Set_ThemeDark",
                "Set_ThemeOcean",
                "Set_ThemeRoseCedar",
                "Set_ThemeCustom",
                "Set_ThemeLapisBlue",
                "Set_ThemeJadeMist",
            ],
        )
        frozen = {entry["id"]: entry for entry in ledger["entries"]}
        for key, zh in FROZEN_ZH.items():
            self.assertEqual(frozen[key]["zh"], zh)
        with tempfile.TemporaryDirectory() as temp_dir:
            out = Path(temp_dir)
            generate_localization.write_generated(ledger, out)
            generated = (out / "LocalizationStrings.generated.h").read_text(encoding="utf-8")
            self.assertIn("static const char* g_strings_ja[]", generated)
            ja = generate_localization.parse_generated_string_table(generated, "g_strings_ja")
            self.assertEqual(len(ja), len(ids))
            exempt = {ids.index("Auth_ContactAdmin")}
            empty_ja = [ids[i] for i, value in enumerate(ja) if value == "" and i not in exempt]
            self.assertEqual(
                empty_ja,
                [],
                "knife-3 completion gate: only Auth_ContactAdmin may keep an empty ja",
            )


if __name__ == "__main__":
    unittest.main()
