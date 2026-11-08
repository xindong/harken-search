import hashlib
import json
import pathlib
import re
import unittest
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
REAPER_EXT = ROOT / "reaper_extension"
LEDGER = ROOT / "locale" / "harken.ui.strings.json"
INSTALLER = ROOT / "installer" / "HarkenSearch_Setup.iss"

BRAND_RE = re.compile(r"(?<![A-Za-z0-9])(?:Aris|Auro)(?![A-Za-z0-9])")
STRING_LIT_RE = re.compile(r'(?:u8|L|u|U)?"((?:\\.|[^"\\])*)"')
ISS_STRING_RE = re.compile(r"(?:'((?:''|[^'])*)'|\"((?:\\.|[^\"\\])*)\")")
HAN_RE = re.compile("[\u3400-\u9fff]")
KANA_RE = re.compile("[\u3040-\u30ff]")
SRC_SUFFIXES = {".cpp", ".h", ".hpp", ".inl", ".cc", ".c", ".mm"}
WEBUI_SRC_SUFFIXES = {".ts", ".tsx"}

# Exact legacy identities the current code still reads or emits.
# Key: (repository path, complete string literal). Value: (existing count, reason).
COMPATIBILITY_SRC_LITERALS = {
    ("src/ConsentManager.cpp", "Local\\Aris.ConsentFlow.v1"): (1, "旧版同意记录键"),
    ("src/ConsentManager.cpp", "# Aris legal consent (auto-generated)\n"): (1, "旧版同意记录文件头"),
    ("src/DefaultAudioPlayerAssociation.cpp", "Auro.AudioFile"): (1, "Windows 历史文件关联身份"),
    ("src/HostSendModel.cpp", "Auro"): (1, "既有宿主发送暂存目录"),
    ("src/ReaperBridgeDoctor.cpp", "Aris"): (1, "识别历史 REAPER 扩展签名"),
    ("src/ReaperBridgeDoctor.cpp", "Aris REAPER Extension"): (1, "识别历史 REAPER 扩展签名"),
    ("src/UserActionLog.cpp", "# Aris User Action Log\n"): (1, "历史操作日志文件头"),
    ("src/UserActionLog.cpp", "# Aris User Action Timeline ("): (1, "历史操作时间线文件头"),
    ("src/Utils.cpp", "\\Auro.db"): (1, "历史数据库文件路径"),
    ("src/Utils.cpp", "/Auro.db"): (1, "历史数据库文件路径"),
    ("src/Utils.cpp", "# Aris settings (auto-generated)\n"): (1, "历史设置文件头"),
    ("src/Utils.cpp", "Aris/SiliconFlowApiKey"): (4, "读取并清理历史凭据键"),
    ("src/WindowsMain.cpp", "\\database\\Aris_Dev.db"): (1, "历史开发库文件路径"),
    ("src/WindowsMain.cpp", "Global\\Auro_SingleInstance_Mutex"): (1, "历史单实例互斥体身份"),
    ("src/WindowsMain.cpp", "Global\\Auro_SingleInstance_Mutex_"): (1, "历史单实例互斥体身份"),
    ("src/WindowsMain.cpp", "Aris_WebView2"): (1, "既有 WebView2 运行目录"),
    ("src/WindowsMain.cpp", "Auro.AudioFile"): (1, "读取 Windows 历史文件关联身份，不是显示文案"),
    ("src/platform/floating_window_corners_win32.cpp", "Aris.ModuleShell.RoundWidth"): (1, "Win32 窗口属性键，不是显示文案"),
    ("src/platform/floating_window_corners_win32.cpp", "Aris.ModuleShell.RoundHeight"): (1, "Win32 窗口属性键，不是显示文案"),
    ("src/platform/floating_window_corners_win32.cpp", "Aris.ModuleShell.RoundRadius"): (1, "Win32 窗口属性键，不是显示文案"),
    ("src/SettingsFileWriter.h", "# Aris settings (auto-generated)\n"): (1, "历史设置文件头"),
    ("src/SettingsDeveloperTools.inl", "# Aris dev mode\n"): (1, "历史开发模式配置头"),
    ("src/platform/harken_user_data_layout.h", "Aris" + ".manifest"): (1, "迁移旧进程清单文件名"),
    ("src/platform/system_macos.mm", "Auro_Drag"): (1, "既有拖放数据格式身份"),
    ("src/platform/system_win32.cpp", "Auro_Drag\\"): (1, "既有拖放数据格式身份"),
}

REQUIRED_UA = (
    "HarkenSearch/2.0",
    "HarkenSearch/1.0",
    "HarkenSearchFeedback/2.0",
    "Harken Search Update/1",
    "Harken Search Update Check/1",
)

REQUIRED_REAPER_HOST_COPY = (
    "[Harken Search] REAPER bridge ",
    "Harken Search: Insert Media Batch from Pipe",
    "Harken Search: Insert Media Batch",
    "Harken Search: REAPER batch rolled back",
)

REQUIRED_RUNTIME_NAMES = (
    "HarkenSearch.exe",
    "HarkenSearchBootstrap.exe",
    "HarkenSearchProToolsBridge.exe",
)


def _cpp_unescape(text):
    return (
        text.replace("\\n", "\n")
        .replace("\\t", "\t")
        .replace("\\\"", "\"")
        .replace("\\\\", "\\")
    )


def _visible_payload(text):
    if "##" in text:
        return text.split("##", 1)[0]
    return text


def _brand_hits(text):
    return [match.group(0) for match in BRAND_RE.finditer(text)]


def _src_brand_literal_occurrences(rel, text):
    """Return (line, whole literal, old word) for active source strings."""
    found = []
    for line_no, raw in enumerate(text.splitlines(), 1):
        code = raw.split("//", 1)[0]
        for match in STRING_LIT_RE.finditer(code):
            literal = _visible_payload(_cpp_unescape(match.group(1)))
            for brand in _brand_hits(literal):
                found.append((line_no, literal, brand))
    return found


def _unapproved_brand_literals(rel, text):
    """An exact allowance can only cover its original file, literal and count."""
    seen = Counter()
    unapproved = []
    for line_no, literal, brand in _src_brand_literal_occurrences(rel, text):
        key = (rel, literal)
        seen[key] += 1
        approved = COMPATIBILITY_SRC_LITERALS.get(key)
        if approved is None or seen[key] > approved[0]:
            unapproved.append((line_no, literal, brand))
    return unapproved


def _compatibility_exemption_drift():
    """Reject obsolete allowances as well as newly added old-name literals."""
    found = Counter()
    for path, rel in _iter_src_files():
        for _, literal, _ in _src_brand_literal_occurrences(
            rel, path.read_text(encoding="utf-8")
        ):
            key = (rel, literal)
            if key in COMPATIBILITY_SRC_LITERALS:
                found[key] += 1
    return [
        f"{rel}: {literal!r} expected {count}, found {found[(rel, literal)]}"
        for (rel, literal), (count, reason) in COMPATIBILITY_SRC_LITERALS.items()
        if not reason.strip() or found[(rel, literal)] != count
    ]


def _iter_src_files():
    for root in (SRC, REAPER_EXT):
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() not in SRC_SUFFIXES:
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel.endswith("harken_legacy_data_migrator.cpp"):
                continue
            yield path, rel


def _iter_webui_src_files():
    for path in SRC.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() not in WEBUI_SRC_SUFFIXES:
            continue
        yield path, path.relative_to(ROOT).as_posix()


# RQ-20260915-04: the old scan only read string literals, so markup living in a
# raw string literal (LR"HTML_L2( ... )HTML_L2") was invisible. Startup and
# welcome pages shipped `<div class="brand-word">aris</div>` and
# `<title>ARIS Loading</title>` while every gate stayed green.
RAW_STRING_RE = re.compile(r'(?:u8|L|u|U)?R"([A-Za-z_][A-Za-z0-9_]{0,15})\((.*?)\)\1"', re.S)
MARKUP_TEXT_RE = re.compile(r">([^<>{}]{2,200})<")
ANY_CASE_BRAND_RE = re.compile("(?<![A-Za-z0-9])(Aris|Auro|Aura|Airs)(?![A-Za-z0-9])", re.I)
FONT_NAME_RE = re.compile("Arial|Aurora|Binaural", re.I)


# RQ-20260915-04: known residuals of the widened scan surface.  Every entry is
# attributed to the card that decides its disposition; a hit that is NOT listed here is a
# regression, and a listed entry that disappeared is stale.  Both fail the gate
# so this table cannot silently rot.
BASELINE = ROOT / "tests" / "harken_search_visible_copy_baseline.json"


def _baseline_entries():
    payload = json.loads(BASELINE.read_text(encoding="utf-8"))
    return payload["entries"]


def _fingerprint(rel, fragment):
    norm = re.sub(r"\s+", " ", fragment).strip()
    digest = hashlib.sha256(norm.encode("utf-8")).hexdigest()[:16]
    return "%s::%s" % (rel, digest)


def _partition_by_baseline(found, entries):
    """Return (regressions, matched_keys).  found: iterable of (rel, line, brand, fragment).

    Keys carry an occurrence ordinal, so baseline entries are matched one-for-one.
    Adding a *second* copy of an already-exempt string in the same file therefore
    surfaces as a regression instead of silently reusing the old exemption.
    """
    regressions = []
    matched = set()
    seen = {}
    for rel, line, brand, fragment in sorted(found, key=lambda row: (row[0], row[1])):
        base = _fingerprint(rel, fragment)
        ordinal = seen.get(base, 0) + 1
        seen[base] = ordinal
        key = "%s#%d" % (base, ordinal)
        if key in entries:
            matched.add(key)
        else:
            regressions.append("%s:%d:%s:%s" % (rel, line, brand, fragment[:120]))
    return regressions, matched


def _markup_text_hits(text):
    """Brand names sitting in user-visible markup text nodes.

    Case-insensitive because the splash brand word is deliberately lowercase
    (the CSS applies text-transform:lowercase), and font-family noise is
    excluded so Arial/Aurora never register as a brand.
    """
    hits = []
    for match in RAW_STRING_RE.finditer(text):
        body = match.group(2)
        base_line = text[: match.start()].count(chr(10)) + 1
        for node_match in MARKUP_TEXT_RE.finditer(body):
            node = node_match.group(1).strip()
            if not node or FONT_NAME_RE.search(node):
                continue
            for brand in ANY_CASE_BRAND_RE.finditer(node):
                line = base_line + body[: node_match.start()].count(chr(10))
                hits.append((line, brand.group(0), node[:160]))
    return hits


# RQ-20260915-04: WebUI copy was skipped wholesale before.  Scanning only JSX text
# nodes and attributes still missed TypeScript object fields that render to the
# user (`label: '\u5173\u4e8e Aris'`, `windowTitle: 'Aris - ...'`), so the extractor
# reads every quoted string and suppresses identifier/path shapes instead.
WEBUI_STRING_RE = re.compile(r"""(['"`])((?:[^'"`\\\n]|\\.)*)\1""")
WEBUI_PATH_NOISE_RE = re.compile(
    r"[\\/]|^\.[a-z]|\.(?:css|ts|tsx|png|svg|json|exe|dll)$", re.I
)
WEBUI_IDENT_NOISE_RE = re.compile(r"^[A-Za-z_$][A-Za-z0-9_$]*$")
WEBUI_CONST_NOISE_RE = re.compile(r"^[A-Z][A-Z0-9_]{2,}$")
WEBUI_TEMPLATE_NOISE_RE = re.compile(r"\$\{")


def _webui_copy_hits(text):
    hits = []
    for line_no, raw in enumerate(text.splitlines(), 1):
        code = raw.split("//", 1)[0]
        for match in WEBUI_STRING_RE.finditer(code):
            fragment = match.group(2).strip()
            if not fragment or FONT_NAME_RE.search(fragment):
                continue
            # Identifier, path, SCREAMING_CONST and template-expression shapes are
            # code, not copy; everything else is treated as user-visible text.
            if (
                WEBUI_PATH_NOISE_RE.search(fragment)
                or WEBUI_IDENT_NOISE_RE.match(fragment)
                or WEBUI_CONST_NOISE_RE.match(fragment)
                or WEBUI_TEMPLATE_NOISE_RE.search(fragment)
            ):
                continue
            for brand in ANY_CASE_BRAND_RE.finditer(fragment):
                hits.append((line_no, brand.group(0), fragment[:160]))
    return hits


def _collect_widened_hits():
    """Every hit the widened surface produces, tagged by repo-relative path.

    Kept in one place so the baseline check and the regression message agree on
    exactly what was scanned.
    """
    found = []
    for rel in ("src/WindowsMain.cpp", "src/webui/WebUiEmbedded.generated.h"):
        path = ROOT / rel
        if not path.is_file():
            continue
        for line, brand, fragment in _markup_text_hits(path.read_text(encoding="utf-8")):
            found.append((rel, line, brand, fragment))
    for path, rel in _iter_webui_src_files():
        for line, brand, fragment in _webui_copy_hits(path.read_text(encoding="utf-8")):
            found.append((rel, line, brand, fragment))
    # macOS window title was invisible before .mm entered SRC_SUFFIXES.
    macos = SRC / "platform" / "app_entry_macos.mm"
    if macos.is_file():
        for line_no, raw in enumerate(macos.read_text(encoding="utf-8").splitlines(), 1):
            for match in re.finditer(r'setTitle:\s*@"([^"]*)"', raw):
                title = match.group(1).strip()
                if not title or FONT_NAME_RE.search(title):
                    continue
                for brand in ANY_CASE_BRAND_RE.finditer(title):
                    found.append(("src/platform/app_entry_macos.mm", line_no, brand.group(0), title[:160]))
    return found


def _script_issue(lang, text):
    if not text:
        return None
    lowered = lang.lower()
    has_han = bool(HAN_RE.search(text))
    has_kana = bool(KANA_RE.search(text))
    if lowered in {"en", "english"} and has_han:
        return "en column contains Han script"
    if lowered in {"ja", "jp", "japanese"} and has_han and not has_kana:
        return "ja column contains Han without Kana"
    return None


class HarkenSearchVisibleCopyScanTests(unittest.TestCase):
    def test_new_same_word_visible_copy_cannot_reuse_compatibility_exemption(self):
        self.assertEqual(
            ["Aris"],
            [brand for _, _, brand in _unapproved_brand_literals(
                "src/Utils.cpp", '"# Aris settings are great"'
            )],
        )

    def test_compatibility_exemption_cannot_move_to_another_file(self):
        self.assertEqual(
            ["Auro"],
            [brand for _, _, brand in _unapproved_brand_literals(
                "src/main.cpp", '"Auro.AudioFile"'
            )],
        )

    def test_new_duplicate_compatibility_literal_is_rejected(self):
        text = '"Auro.AudioFile"\n"Auro.AudioFile"\n'
        self.assertEqual(
            [(2, "Auro.AudioFile", "Auro")],
            _unapproved_brand_literals("src/DefaultAudioPlayerAssociation.cpp", text),
        )

    def test_compatibility_exemption_list_matches_existing_code(self):
        self.assertEqual([], _compatibility_exemption_drift())

    def test_word_boundary_ignores_binaural_and_aurora(self):
        self.assertEqual([], _brand_hits("Binaural Aurora"))
        self.assertEqual(["Auro", "Aris"], _brand_hits("Auro and Aris"))

    def test_ja_column_filled_with_chinese_is_rejected(self):
        self.assertEqual(_script_issue("ja", "\u5173\u95ed\u5e94\u7528"), "ja column contains Han without Kana")
        self.assertIsNone(_script_issue("ja", "\u30a2\u30d7\u30ea\u3092\u9589\u3058\u308b"))
        self.assertIsNone(_script_issue("en", "Close Harken Search"))
        self.assertEqual(_script_issue("en", "\u5173\u95ed"), "en column contains Han script")

    def test_ledger_language_columns_drop_old_brand_and_stay_script_pure(self):
        payload = json.loads(LEDGER.read_text(encoding="utf-8"))
        entries = payload["entries"]
        self.assertTrue(entries)
        hits = []
        langs = set()
        for entry in entries:
            ident = entry.get("id", "")
            for key, value in entry.items():
                if key in {"id", "type"} or not isinstance(value, str):
                    continue
                langs.add(key)
                for brand in _brand_hits(value):
                    hits.append("%s.%s:%s" % (ident, key, brand))
        self.assertEqual(hits, [], "ledger still has old brand:" + chr(10) + chr(10).join(hits[:80]))
        # 05 merge 76f413e2 introduced this scan when the ledger had zh/en only.
        # Later i18n knives added a ja column and a few legitimate en labels that
        # contain Han (language names). Whole-ledger ja Han-without-kana is owned
        # by TestLocalizationPurity / locale/aris.ui.strings.purity-baseline.json.
        # RQ-20260910-14 only requires the repaired Dlg_FilterAuroDb row to stay
        # script-pure on zh/en/ja after replacing visible Auro with Harken Search.
        repaired = [entry for entry in entries if entry.get("id") == "Dlg_FilterAuroDb"]
        self.assertEqual(len(repaired), 1)
        repaired_script = []
        for key, value in repaired[0].items():
            if key in {"id", "type"} or not isinstance(value, str):
                continue
            issue = _script_issue(key, value)
            if issue:
                repaired_script.append("%s.%s:%s" % ("Dlg_FilterAuroDb", key, issue))
        self.assertEqual(repaired_script, [])
        self.assertTrue({"zh", "en"} <= langs)
        # Auth_Title was a dead ledger row removed by 7e829aa1 / PR #172.
        # Pin the live product-name canary instead of resurrecting an unused key.
        titles = [entry for entry in entries if entry.get("id") == "Main_AppNameBrand"]
        self.assertEqual(len(titles), 1)
        self.assertEqual(titles[0]["zh"], "Harken Search")
        self.assertEqual(titles[0]["en"], "Harken Search")

    def test_installer_display_copy_uses_harken_search(self):
        text = INSTALLER.read_text(encoding="utf-8")
        legacy_start = text.index("function LegacyProductIsRunning(")
        legacy_end = text.index("function IsUserProfileInstallDir(", legacy_start)
        hits = []
        for line_no, raw in enumerate(text.splitlines(), 1):
            stripped = raw.lstrip()
            if stripped.startswith(";"):
                continue
            line = raw
            for match in ISS_STRING_RE.finditer(line):
                literal = match.group(1) if match.group(1) is not None else match.group(2)
                offset = sum(len(line) + 1 for line in text.splitlines()[:line_no - 1])
                if (legacy_start <= offset < legacy_end and
                    literal in {"Auro.exe", "Aris.exe", "ArisBootstrap.exe"} and
                    "IsFileLocked(AddBackslash(InstallLocation) + '" + literal + "')" in raw):
                    continue
                for brand in _brand_hits(literal):
                    hits.append("HarkenSearch_Setup.iss:%d:%s:%s" % (line_no, brand, literal[:120]))
        self.assertEqual(hits, [], "installer display copy still has old brand:" + chr(10) + chr(10).join(hits))
        self.assertIn('#define MyAppName "Harken Search"', text)
        self.assertIn("Harken Search", text)
        self.assertIn("{userappdata}\\ArisAudio", text)

    def test_src_visible_strings_and_ua_drop_old_brand(self):
        hits = []
        found = {token: False for token in REQUIRED_UA + REQUIRED_RUNTIME_NAMES}
        for path, rel in _iter_src_files():
            text = path.read_text(encoding="utf-8")
            for token in list(found):
                if token in text:
                    found[token] = True
            for line_no, literal, brand in _unapproved_brand_literals(rel, text):
                hits.append("%s:%d:%s:%s" % (rel, line_no, brand, literal[:160]))
        self.assertEqual(hits, [], "src visible strings still have old brand:" + chr(10) + chr(10).join(hits[:120]))
        missing = [token for token, ok in found.items() if not ok]
        self.assertEqual(missing, [], "required Harken Search identity missing: " + ", ".join(missing))

    def test_reaper_extension_is_on_the_visible_copy_scan_surface(self):
        scanned = {rel for _, rel in _iter_src_files()}
        self.assertIn("reaper_extension/reaper_harken.cpp", scanned)
        self.assertNotIn(
            ("reaper_extension/reaper_harken.cpp", "Aris REAPER Extension"),
            COMPATIBILITY_SRC_LITERALS,
        )

    def test_reaper_extension_cpp_literals_use_harken_search(self):
        """Host-visible REAPER copy must use Harken Search; .rc identity is out of scope."""
        cpp_files = [
            (path, rel)
            for path, rel in _iter_src_files()
            if rel.startswith("reaper_extension/") and rel.endswith(".cpp")
        ]
        self.assertTrue(cpp_files, "expected reaper_extension/*.cpp on the scan surface")
        hits = []
        found = {token: False for token in REQUIRED_REAPER_HOST_COPY}
        for path, rel in cpp_files:
            text = path.read_text(encoding="utf-8")
            for token in list(found):
                if token in text:
                    found[token] = True
            for line_no, raw in enumerate(text.splitlines(), 1):
                code = raw.split("//", 1)[0]
                for match in STRING_LIT_RE.finditer(code):
                    literal = _visible_payload(_cpp_unescape(match.group(1)))
                    for brand in _brand_hits(literal):
                        hits.append("%s:%d:%s:%s" % (rel, line_no, brand, literal[:160]))
        self.assertEqual(
            hits,
            [],
            "reaper_extension cpp literals still have old brand:" + chr(10) + chr(10).join(hits[:80]),
        )
        missing = [token for token, ok in found.items() if not ok]
        self.assertEqual(missing, [], "required REAPER host copy missing: " + ", ".join(missing))

    # ------------------------------------------------------------------
    # RQ-20260915-04: the three blind spots that let the splash brand word
    # ship while every gate stayed green.
    # ------------------------------------------------------------------
    def test_markup_text_nodes_are_scanned_case_insensitively(self):
        """The old scan read only quoted literals, so markup text was invisible.

        Positive control: this is the exact shape that shipped `aris` on the
        startup page. The probe must fire on it.
        """
        source = 'const char* k = R"HTML(<div class="brand-word">aris</div>)HTML";'
        hits = _markup_text_hits(source)
        self.assertEqual(["aris"], [brand for _, brand, _ in hits])

    def test_markup_scan_ignores_font_family_names(self):
        source = 'R"HTML(font-family:\"Segoe UI\",Arial,sans-serif)HTML"'
        self.assertEqual([], _markup_text_hits(source))
        self.assertEqual([], _markup_text_hits('R"HTML(<span>Aurora</span>)HTML"'))

    def test_widened_surface_matches_baseline(self):
        """Single baseline-driven assertion over the widened scan surface.

        A hit absent from the baseline is a regression (new old-brand copy), and a
        baseline entry that no longer fires is stale.  Both fail, so the table can
        neither hide new defects nor keep dead exemptions alive.
        """
        entries = _baseline_entries()
        found = _collect_widened_hits()
        regressions, matched = _partition_by_baseline(found, entries)
        self.assertEqual(
            [],
            regressions,
            "new old-brand copy on the widened surface (not in baseline):"
            + chr(10)
            + chr(10).join(regressions[:60]),
        )
        stale = sorted(set(entries) - matched)
        self.assertEqual(
            [],
            stale,
            "baseline entries no longer present; delete stale exemptions and review the owning card:"
            + chr(10)
            + chr(10).join("%s (%s)" % (key, entries[key].get("owner")) for key in stale),
        )

    def test_duplicate_copy_in_same_file_does_not_reuse_exemption(self):
        """Same file, same fragment, second occurrence must not inherit the first exemption."""
        found = [
            ("src/webui/src/components/menu/MenuBar.tsx", 268, "Aris", "关于 Aris"),
            ("src/webui/src/components/menu/MenuBar.tsx", 999, "Aris", "关于 Aris"),
        ]
        first_key = "%s#%d" % (_fingerprint(found[0][0], found[0][3]), 1)
        entries = {
            first_key: {"path": found[0][0], "owner": "RQ-20260915-01"},
        }
        regressions, matched = _partition_by_baseline(found, entries)
        self.assertEqual([first_key], sorted(matched))
        self.assertEqual(1, len(regressions))
        self.assertIn(":999:Aris:", regressions[0])
        self.assertIn("关于 Aris", regressions[0])

    def test_baseline_entries_are_attributed_to_a_card(self):
        """Every exemption names the card responsible for its disposition."""
        unattributed = [
            key
            for key, entry in _baseline_entries().items()
            if not str(entry.get("owner") or "").strip()
        ]
        self.assertEqual([], unattributed, "baseline entries missing an owner card")

    def test_retained_webui_references_have_the_decision_card_as_owner(self):
        entries = _baseline_entries()
        retained = {
            "src/webui/WebUiEmbedded.generated.h": ("<title>AURO</title>",),
            "src/webui/src/components/runtime-twin/runtime-contract.ts": (
                "capturedAt: '2026-07-21'",
                "windowTitle: 'Aris - JUSTMIXRFull (0 文件)'",
            ),
        }
        self.assertTrue(set(retained) <= {entry["path"] for entry in entries.values()})
        for path, markers in retained.items():
            entry = next(entry for entry in entries.values() if entry["path"] == path)
            self.assertEqual(entry["owner"], "RQ-20260917-08")
            self.assertIn("保留", entry["why"])
            text = (ROOT / path).read_text(encoding="utf-8")
            for marker in markers:
                self.assertIn(marker, text)

    def test_macos_window_title_is_scanned(self):
        """.mm was missing from SRC_SUFFIXES, so the whole macOS layer was exempt."""
        self.assertIn(".mm", SRC_SUFFIXES)
        scanned = {rel for _, rel in _iter_src_files()}
        self.assertIn("src/platform/app_entry_macos.mm", scanned)
        text = (SRC / "platform" / "app_entry_macos.mm").read_text(encoding="utf-8")
        window_titles = re.findall(r'setTitle:\s*@"([^"]*)"', text)
        self.assertTrue(window_titles, "expected to find the macOS window title assignment")
        found = _collect_widened_hits()
        regressions, _ = _partition_by_baseline(found, _baseline_entries())
        self.assertEqual([], [r for r in regressions if "app_entry_macos.mm" in r])


if __name__ == "__main__":
    unittest.main()
