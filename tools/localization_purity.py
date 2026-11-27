#!/usr/bin/env python3
"""Language purity gate in baseline (no-regression) mode for RQ-20260909-02.

Visible UI copy is not yet fully localized or cleaned, so this is not a hard
zero-violation gate yet. Existing violations are recorded in a committed
baseline (locale/aris.ui.strings.purity-baseline.json); the gate passes while
every finding is inside the baseline and FAILS only when a NEW violation
appears, reporting each new item with a locatable description. Once the
baseline is emptied, this same code becomes a hard gate.

Proper nouns, units and format tokens are not treated as language mixes.
Wwise wz() strings and diagnostics panels stay out of scope.

Run "python tools/localization_purity.py check" for the same gate used by
the tests. "inventory --format json" links localized call sites to all three
ledger cells; "--format markdown" lists remaining copy by window. A dynamic
site is unresolved, not assumed localized. The coverage baseline counts
(source path, display slot, literal) occurrences, without pinning line numbers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LEDGER_PATH = ROOT / "locale" / "harken.ui.strings.json"
SRC_ROOT = ROOT / "src"
BASELINE_PATH = ROOT / "locale" / "aris.ui.strings.purity-baseline.json"
COVERAGE_BASELINE = Path("tests/localization_coverage_baseline.json")
NATIVE_SRC_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".m", ".mm"}
LANGUAGES = ("zh", "en", "ja")

FROZEN_IDS = {
    "View_InfoPanel",
    "View_DemoWindow",
    "Panel_InfoDetach",
    "Set_UILanguage",
}
FROZEN_ZH = {
    "View_InfoPanel": "Info",
    "View_DemoWindow": "Demo窗口",
    "Panel_InfoDetach": "Info独立窗口",
    "Set_UILanguage": "界面语言 / Language:",
}

CJK_RE = re.compile(r"[㐀-鿿]")
CJK_ANY_RE = re.compile(r"[぀-ヿ㐀-鿿]")  # han + kana (registry gates must see Japanese too)
# Simplified Chinese forms whose Japanese text normally uses a different Kanji.
# This keeps valid Kanji-only Japanese words (for example 「設定」) allowed
# while catching copied Chinese in the ja column.
SIMPLIFIED_ONLY_CHARS = frozenset(
    "\u4e13\u4e1a\u4e1c\u4e1d\u4e22\u4e24\u4e25\u4e27\u4e2a\u4e3a\u4eec\u53d1\u53d8\u540e\u542f\u5458\u56e2\u56fe\u5706\u573a\u5757\u575a\u575b\u5784\u5792\u5956\u5986\u5987\u5988\u5a31\u5a74\u5b59\u5b9d\u5b9e\u5ba0\u5ba1\u5e7f\u5e84\u5e86\u5e93\u5e94\u5f02\u5f03\u5f20\u5f39\u5f3a\u5f52\u5f55\u5fc6\u5fe7\u6001\u604b\u6076\u6094\u60a6\u60ca\u60ef\u6269\u626b\u626c\u62a4\u62a5\u62c5\u62df\u62e2\u62e3\u62e5\u62e9\u6321\u6325\u635f\u6362\u636e\u63ed\u6446\u6447\u654c\u65f6\u663e\u6643\u664b\u6653\u6682\u672f\u6740\u6742\u6743\u6768\u6784\u67aa\u67dc\u6837\u68c0\u697c\u6b22\u6bc1\u6c14\u6c47\u6c49\u6c61\u6c64\u6c89\u6c9f\u6ca7\u6caa\u6cea\u6d01\u6d45\u6d46\u6d47\u6d4a\u6d4b\u6d4e\u6d53\u6d9b\u706d\u706f\u7075\u707e\u7089\u70bc\u70c2\u70ed\u7130\u7231\u7237\u7275\u732e\u73af\u73d1\u7597\u77ff\u7801\u780d\u7814\u786e\u79bb\u79cd\u79ef\u7a33\u7a77\u7a83\u7ad6\u7b7e\u7b80\u7cae\u7ea6\u7ea7\u7eaa\u7eaf\u7eb1\u7eb2\u7eb3\u7eb5\u7eb8\u7eba\u7ec8\u7ecf\u7ed1\u7ed3\u7ed5\u7ed9\u7edc\u7edd\u7ede\u7edf\u7ee7\u7eed\u7eff\u7f15\u7f20\u7f51\u7f57\u804c\u8054\u8083\u80a0\u80f6\u8109\u8111\u8138\u8230\u827a\u8282\u82a6\u82cf\u82f9\u8303\u830e\u8367\u836f\u83b7\u841d\u867e\u8680\u86ee\u8721\u8747\u8865\u89c2\u89c4\u89c6\u89c8\u8ba1\u8ba2\u8ba4\u8ba9\u8baf\u8bb2\u8bb8\u8bba\u8bbe\u8bc1\u8bc4\u8bc6\u8bd1\u8bd5\u8bd7\u8bda\u8be5\u8be6\u8bed\u8bf4\u8bf7\u8bfb\u8c03\u8c08\u8c0a\u8c0b\u8c22\u8c28\u8d1f\u8d22\u8d23\u8d24\u8d25\u8d27\u8d28\u8d29\u8d2d\u8d2e\u8d2f\u8d34\u8d35\u8d37\u8d39\u8d41\u8d44\u8d4b\u8d4c\u8d4f\u8d54\u8d62\u8d8b\u8dc3\u8f6c\u8f6e\u8f6f\u8f70\u8f7b\u8f7d\u8f83\u8f86\u8fa9\u8fbe\u8fc1\u8fc7\u8fc8\u8fd0\u8fd8\u8fd9\u8fdb\u8fdc\u8fdd\u8fde\u8fdf\u9009\u903b\u9057\u90ae\u90bb\u90d1\u9274\u94dd\u9500\u9501\u9505\u9519\u952e\u95f9\u9605\u961f\u9633\u9634\u9636\u9645\u9646\u9648\u9669\u96a7\u96be\u96fe\u9876\u9879\u987a\u987b\u987f\u9886\u9898\u989c\u98ce\u98de\u996d\u996e\u9970\u9971\u9986\u9a6c\u9c7c\u9e1f\u9e21\u9f84\u9f99\u9f9f"
)
LATIN_WORD_RE = re.compile(r"[A-Za-z]{2,}")
# ImGui id suffix (## / ###) is never visible; strip before any language check
# so pure-CJK labels are not flagged mixed by the suffix's ASCII.
IMGUI_ID_SUFFIX_RE = re.compile(r"##.*", re.S)
FORMAT_RE = re.compile(
    r"%(?:\d+\$)?[-+#0 ]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|z|t|j)?[diuoxXfFeEgGaAcspn%]"
)
PROPER_NOUNS = (
    "Aris",
    "Aura",
    "Cuevi",
    "Harken",
    "ImGui",
    "SQLite",
    "Whisper",
    "Wwise",
    "WAAPI",
    "REAPER",
    "Pro Tools",
    "Dolby",
    "Atmos",
    "SiliconFlow",
    "FLUX",
    "Camelot",
    "Studio Slate",
    "miniaudio",
    "Reaper",
    "AI",
    # Knife-4 batch-4 word package — each entry traces to the Lead knife-5
    # rulings (① whitelist, ② per-item, triage-table C-class notes) and the
    # triage table is the authority; the package is NOT self-extendable.
    "MIDI", "DAW", "EQ", "ASR",
    "Enter", "Esc", "Tab", "Ctrl", "Alt", "Shift", "Space",
    "Return",  # keycap sibling of Enter (lead 7-A mix ruling, tier 2)
    "Down", "Up", "Left", "Right",
    "F12", "F11", "F10", "F9", "F8", "F7", "F6", "F5", "F4", "F3", "F2", "F1",
    "Bug", "Bootstrap", "HTTP",
    "PT",  # direct abbreviation of the ratified brand phrase "Pro Tools" (lead 7-A mix ruling, tier 1)
    "Auro",  # legacy brand identifier (repo-wide convention: 历史/兼容标识), lead ruling A1
    "BEXT", "UCS", "FTS", "EXIF",
    "Steinberg", "Cubase", "Nuendo", "HRTF",
    "WebView2",
    "Base", "small", "medium",
    "Solo", "Mute", "Pan", "Spotting", "DEBUG",
    "AppData", "WAL", "SHM",
    "SSD", "NVMe",
)
UNITS = (
    "Hz",
    "kHz",
    "ms",
    "sec",
    "dB",
    "dBFS",
    "dBTP",
    "LUFS",
    "RMS",
    "BPM",
    "FFT",
    "fps",
    "ST",
    "WAV",
    "MP3",
    "FLAC",
    "OGG",
    "AIFF",
    "M4A",
    "WMA",
    "JSON",
    "API",
    "EXE",
    "ID3",
    "BWF",
    "iXML",
    "UAC",
    "MB",
    "GB",
    "MiB", "CSV",
    "SRT",
    "DLL",
    "INI",
)
# Multi-word protected phrases (rulings: Pro Tools / Studio Slate standard
# numbers / UCS expansion). Matched before single words.
# Harken Search: other-lane #155 brand rename already merged into qa/current
# and main (established product fact); this card is mechanical transcript,
# not new copy. Final brand authority = user; rollback goes through that lane
# or a new card. (lead ruling on integration closeout)
PROTECTED_PHRASES = ("Pro Tools", "Studio Slate", "ITU-R BS.775", "Universal Category System", "Harken Search")
# Template placeholders <...> are user-typed syntax (ruling ①: `<字段名>` exempt);
# dotted names are file/path/URL tokens (ruling ①: file names/paths exempt).
TEMPLATE_PLACEHOLDER_RE = re.compile(r"<[^<>]{1,24}>")
DOTTED_NAME_RE = re.compile(r"[A-Za-z][\w-]*(?:\.[\w-]+)+")
MOJIBAKE_RE = re.compile("[\u00c2\u00c3\u00c5][\u0080-\u00ff]|[\u00e2][\u0080-\u009f]")
SKIP_DIR_PARTS = {"webui", "coverwall_lab", "thirdparty"}
SKIP_FILE_NAMES = {
    "Localization.cpp",
    "Localization.h",
    "WwiseTransferPanel.cpp",
    "DevToolsAuroSmoke.cpp",
    "DevToolsModule.cpp",
    "GMPanel.cpp",
    "DiagnosticsLogger.cpp",
    "DiagnosticsLoggerStub.cpp",
    "DiagnosticFileLifecycle.cpp",
    "FrameBudgetDiagnostics.cpp",
}
UI_LABEL_FUNCTIONS = (
    "TextUnformatted", "TextDisabled", "TextWrapped", "TextColored", "SmallButton",
    "Button", "ButtonText", "MenuItem", "BeginMenu", "BeginPopupModal", "BeginTabItem",
    "TabItemButton", "SeparatorText", "ProgressBar", "TableSetupColumn",
    "Selectable", "Checkbox", "RadioButton", "CollapsingHeader", "TreeNodeEx", "TreeNode",
    "BulletText", "SetTooltip", "BeginItemTooltip", "LabelText",
    "InputText", "InputInt", "Combo", "SliderInt", "Begin", "Text",
)
UI_CALL_RE = re.compile(r"\b(?:ImGui::)?(" + "|".join(UI_LABEL_FUNCTIONS) + r")\s*\(")
# The visible string is not the first argument on these calls (color/float
# comes first); locate the first literal inside the call's argument list.
# TextColored(color, "label") / ProgressBar(frac, size, "overlay").
COLOR_FIRST_FUNCS = {"TextColored", "ProgressBar"}
SAMPLE_LIMIT = 12

# ---- Knife-6 F1 (lead ruling): registered project display helpers ----
# These render visible UI text but are not ImGui calls, so UI_CALL_RE is
# blind to them by construction. Registration is EXPLICIT (no wildcards):
#   * SectionHeader(title, subtitle): literal in arg slot 1 or 2
#   * Info-table label lambdas: literal first arg, only in files that define
#     the lambda (`auto row = ...`), value args never scanned
#   * return-hint builder functions: exact (file, function) pairs below
#     NOTE (lead ruling, bundle-A): GetAIRecognitionPrompt was deliberately
#     NOT registered — its returns are ASR transcription payloads keyed by the
#     recognition-language setting, not by UI language; L()-izing them would
#     change transcription behavior when the UI language switches (functional
#     drift). Exclusion reason also recorded in knife5-triage doc.
SECTION_HEADER_RE = re.compile(r"\bSectionHeader\s*\(")
ROW_LABEL_FUNCS = ("row", "rowMono", "rowInt", "rowIntMono")
RETURN_HELPER_FNS = {
    ("main.cpp", "GetCapabilityTag"),
    ("main.cpp", "GetCapabilityDetail"),
    ("main.cpp", "BuildScanCompletionPendingLabel"),
    ("main.cpp", "BuildScanReadyToWorkPendingLabel"),
    ("main.cpp", "GetRenameModeName"),
    ("main.cpp", "GetRenameModeDescription"),
    ("ThemeEditor.cpp", "ThemeEditorGroupHintZh"),
    ("ThemeEditorSchema.h", "ThemeEditorGroupNameZh"),
    ("AIEngine.cpp", "GetAITaskTypeName"),
    ("AIEngine.cpp", "GetAITaskStatusName"),
    ("AppState.cpp", "GetShortcutPresetName"),
    # 7-0 (screenshot round-3): key-binding display + time-of-day greeting.
    ("AppState.cpp", "toString"),            # KeyBinding::toString: 未设置
}
# F4b (lead ruling): settings-search index entries. The TITLE slot (arg 1) is
# user-visible in the search results; the keywords slot (arg 2) is a matching
# corpus, deliberately NOT scanned. Exact (file, fn) pairs, no wildcards.
SEARCH_INDEX_TITLE_FUNCS = {
    ("main.cpp", "addEntry"): 1,
    ("main.cpp", "pushSectionMatch"): 1,
}

# ---- Knife-6 W6-1 (lead contract): Win32 native-surface sites ----
# Exact named sites only (no wildcards). 'call' = API/wrapper function with
# the visible-text argument slots (0-based, after top-level arg split; wide
# literals anywhere inside those slot expressions are reported, so
# concatenations like L"Aris - " + name + L" (文件)" yield one finding per
# L"..." fragment). 'assign' = statement anchor regex; the whole statement
# (up to ';') is scanned for wide literals (covers multi-line lpstrFilter).
WIN32_CALL_SITES = {
    ("WindowsMain.cpp", "CreateWindowExW"): (2,),
    ("WindowsMain.cpp", "MessageBoxW"): (1, 2),
    ("WindowsMain.cpp", "ShowMessageBox"): (1, 2),
    ("WindowsMain.cpp", "wcscpy_s"): (1,),
    ("WindowsMain.cpp", "AppendMenuW"): (3,),
    ("main.cpp", "ShowMessageBox"): (1, 2),
    ("ConsentManager.cpp", "MessageBoxW"): (1, 2),
    ("ConsentManager.cpp", "appendWideString"): (1,),
    ("ConsentManager.cpp", "showConsentDocument"): (1,),
}
WIN32_ASSIGN_SITES = (
    ("WindowsMain.cpp", r"\btitle\s*\+?="),
    ("main.cpp", r"lpstrFilter\s*="),
    ("platform/audio_file_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/avatar_image_file_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/cover_image_import_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/database_file_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/executable_file_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/reaper_file_list_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/region_wav_export_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/settings_import_dialog_win32.cpp", r"lpstrFilter\s*="),
    ("platform/theme_json_dialogs_win32.cpp", r"lpstrFilter\s*="),
    # Dialog window titles (lpstrTitle) are visible copy too (W6-2 addendum).
    ("main.cpp", r"lpstrTitle\s*="),
    ("platform/audio_file_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/avatar_image_file_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/cover_image_import_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/database_file_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/executable_file_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/reaper_file_list_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/region_wav_export_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/settings_import_dialog_win32.cpp", r"lpstrTitle\s*="),
    ("platform/theme_json_dialogs_win32.cpp", r"lpstrTitle\s*="),
)
WIDE_LIT_RE = re.compile(r'L"((?:[^"\\]|\\.)*)"')
# Ironclad fallback keep-list (Lead knife-6 rule ②): degraded / pre-consent
# dialogs must pop with the CURRENT literals even when the ledger path is
# suspect. These exact (file, fragment) pairs are exempt BY RULING, recorded
# here so they are never mistaken for misses; audit-specific checklist item.
WIN32_KEEP_LITERALS = {
    ("WindowsMain.cpp", "Harken Search 崩溃报告"),  # 7-A integration: caption text follows #155 brand rename; ironclad semantics unchanged
    ("WindowsMain.cpp", "STARTUP: before LoadAppSettings"),
    ("ConsentManager.cpp", "Segoe UI"),  # GDI font face constant, not copy (lead W6-2 exemption note)
}

# ---- Knife-7 bundle-7-0 (lead ruling 2026-09-11): the sixth family ----
# Round-3 user screenshots proved a class of visible UI copy the call-site
# anchors structurally cannot see: text fed through named tables/variables,
# custom Begin/label wrappers, ternaries in non-first arg slots, draw-list
# AddText labels, and \uXXXX-escaped literals (source bytes are pure ASCII —
# grepping the repo for the Chinese characters returns ZERO hits). All
# registration below is EXPLICIT (file, symbol) or function-name keys, no
# wildcards, smuggling-guarded like PROTECTED_ROW_TOKENS. Knife-1 method:
# findings are baseline-recorded (F6: func namespace) and any NEW literal at
# a registered anchor goes red immediately.
F6_WRAPPER_SITES = {
    # func -> visible arg slots (0-based). Window/modal titles are rendered;
    # BeginTransientScrollbarPopup/Child/Table names are pure ids -> NOT listed.
    # ImGui::OpenPopup ids are invisible too; migration waves handle OpenPopup
    # pairs manually (M-slice pre-scan practice), so no scanner anchor.
    "BeginTransientScrollbarWindow": (0,),
    "BeginTransientScrollbarPopupModal": (0,),
    "BeginTransientScrollbarCombo": (0,),
    "DrawUnifiedSearchInput": (1,),          # hint placeholder (arg0 is "##id")
    "HintAwareInputTextWithHint": (1,),      # hint placeholder (recon 7-A slice-3)
    "PopupSectionLabel": (0,),
    "MenuItemWithTooltip": (0, 2),
    "ShowCoverClipboardMessage": (0,),
    "Aura::SetTooltip": (0,),
    "HarkenSearch::SetTooltip": (0,),
}
# 7-0 ruling (2) by Lead 2026-09-11: language endonyms (each language's own
# name) stay untranslated in language pickers — Set_UILanguage frozen-label
# precedent. Exact strings only; they are exempt in F6 registry scans.
F6_ENDONYM_KEEP = {"简体中文", "繁體中文", "English", "日本語"}

# Exact technical placeholder literals (W6-2 \0*.ext precedent, lead 7-A
# report): an API-key format example, never UI language copy.
F6_TECH_KEEP_LITERALS = {"sk-...", "CatID"}

# Lead-ratified exact visible-exempt set (9c, 9 entries verbatim, no
# self-extension): meter unit headers / neutral state glyphs / notation proper
# noun / metadata default passthrough / NoTitleBar pure-id window name.
F6_VISIBLE_KEEP = {
    "Camelot", "ON", "OFF", "[x]", "Various Artists",
    "LUFS", "RMS", "TruePeak", "MainWindow",
}

# 7-A slice-4: file-scoped local-lambda call sites (report/progressCb feed the
# startup card; the names are too generic to register globally, hence exact
# (file, name) keys, no wildcards). All 20 stage sites are ledger-fed since
# slice-4; a NEW literal at these slots goes red.
F6_LOCAL_CALL_SITES = {
    ("WindowsMain.cpp", "report"): (2,),
    ("WindowsMain.cpp", "progressCb"): (2,),
    # 10-QA1 (user-QA splash leak): startup-progress local helpers fed the
    # beta splash / webview directly — invisible to UI_CALL_RE.
    ("WindowsMain.cpp", "presentProgress"): (2,),
    ("WindowsMain.cpp", "renderBetaSplashFrame"): (0,),
    # E1B: preset-popup TITLE helper (file-scope static) — label slot invisible
    # to UI_CALL_RE. PopupMuted NOT registered: its slot 0 is a printf fmt
    # (e.g. " %s"), and fmt slots are structurally not visible copy (registering
    # them would mis-flag pure-format args, weakening the gate).
    ("main.cpp", "PopupTitle"): (0,),
    # gate-ext (RQ-20260911-04): AI task progress messages (F2 pending, gate first)
    ("AIEngine.cpp", "UpdateTaskProgress"): (2,),
}

F6_TABLES = {
    # (base file, array/struct-table symbol): every CJK literal inside the
    # initializer block is visible copy fed to rows/columns at runtime.
    ("AppState.cpp", "g_shortcuts"),         # { action, name, desc, KeyBinding }
    ("main.cpp", "presets"),                 # 特征分析预设按钮名 (均衡/音色/…)
    ("main.cpp", "presetNames"),             # 快捷键预设 (含 u8"精简模式")
    ("main.cpp", "langItems"),               # ASR 语言下拉 (自动识别/简体中文)
    ("main.cpp", "resItems"),                # 封面尺寸档位
    ("main.cpp", "occurrenceItems"),         # 命中位置选项串 (\0-separated combo items)
}
F6_VARS = {
    # (base file, scalar symbol) — named label/char*/std::string feeders that
    # reach Button/Text/AddText later; English-only values count too (Back/Continue).
    ("main.cpp", "crashButtonLabel"),
    ("main.cpp", "backLabel"),
    ("main.cpp", "fwdLabel"),
    ("main.cpp", "kSettingsAvatarChangeLabelZh"),
    ("main.cpp", "kSettingsAvatarRemoveLabelZh"),
    ("main.cpp", "kSettingsBuiltinAvatarLabelZh"),
    ("main.cpp", "kSettingsAvatarStatusLabelZh"),
    ("main.cpp", "kSettingsAvatarAutoLabelZh"),
    ("main.cpp", "kSettingsAvatarCustomPriorityLabelZh"),
    ("main.cpp", "kSettingsAvatarResetAutoLabelZh"),
    ("main.cpp", "kSettingsAvatarPickerOpenLabelZh"),
    ("main.cpp", "kSettingsAvatarPickerTitleZh"),
    ("main.cpp", "kSettingsAvatarPickerUseLabelZh"),
    ("main.cpp", "kSettingsAvatarPickerCancelLabelZh"),
    ("main.cpp", "kSettingsAvatarPickerActualLabelZh"),
    # 9b named-var batch (register+same-shape migration in one wave):
    ("main.cpp", "stateHint"),
    ("main.cpp", "badgeSub"),
    ("main.cpp", "workflowStageLabel"),
    ("main.cpp", "delTxt"),
    # ("main.cpp","prefix") RETIRED same-wave: 12 technical id-prefix vars
    # (dawTarget/tab/savedSearch key namespaces) share the name — too generic,
    # not visible copy. Tab-visible [目录]/[筛选]/[搜索] arms are ledger-fed.
    ("main.cpp", "occurrenceItems"),
    # 10-QA1 (user-QA View-menu leak): menu label vars assigned by ternary
    # from literals, consumed one statement later by MenuItem/OpenPopup.
    ("main.cpp", "playlistQuickBarMenuLabel"),
    ("main.cpp", "kPopupTitle"),
}

# Secondary-slot deny list (explicit, precedent: F4b keywords-corpus ruling).
SECONDARY_DENY_SLOTS = {
    "MenuItem": (1,),  # keyboard-chord display ("Ctrl+O"), batch-4 keycaps precedent
}


def _is_technical_fragment(lit: str) -> bool:
    """A literal that is only a format spec plus ratified unit tokens
    ("%d ms", "%.1f") is a technical fmt slot, not visible copy."""
    if not lit.startswith("%"):
        return False
    residue = FORMAT_RE.sub(" ", lit)
    for token in UNITS:
        residue = re.sub(rf"(?<![A-Za-z]){re.escape(token)}(?![A-Za-z])", " ", residue, flags=re.I)
    return not re.search(r"[A-Za-z぀-ヿ㐀-鿿]", residue)

_ESCAPES_RE = re.compile(
    r"\\(?:u([0-9a-fA-F]{4})|U([0-9a-fA-F]{8})|x([0-9a-fA-F]{1,2})|([nrtvbf'\"\\?]))"
)


def decode_cpp_escapes(s: str) -> str:
    """Decode \\uXXXX / \\UXXXXXXXX / \\xNN and simple escapes so that
    ASCII-escaped CJK literals become visible to the CJK checks (7-0 E4)."""

    def rep(m: re.Match[str]) -> str:
        if m.group(1) is not None:
            return chr(int(m.group(1), 16))
        if m.group(2) is not None:
            return chr(int(m.group(2), 16))
        if m.group(3) is not None:
            return chr(int(m.group(3), 16))
        return {"n": "\n", "r": "\r", "t": "\t", "v": "\v", "b": "\b",
                "'": "'", '"': '"', "\\": "\\", "?": "?"}[m.group(4)]

    return _ESCAPES_RE.sub(rep, s)


def mask_comments(text: str) -> str:
    """Blank out // and /* */ comments (positions/lengths preserved)."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif ch == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif ch == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:min(j + 1, n)])
            i = j + 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def _match_brace(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    in_str = False
    while i < n:
        ch = text[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_str = False
        else:
            if ch == '"':
                in_str = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


def _depth0_literals(expr: str) -> list[tuple[int, str]]:
    """(offset, decoded-text) for every string literal at paren-depth 0 of the
    expression — top-level strings only; strings nested inside sub-calls
    (strcmp(a, "x")) are skipped, ternaries (c ? u8"a" : u8"b") are not."""
    out: list[tuple[int, str]] = []
    depth = 0
    i = 0
    n = len(expr)
    while i < n:
        ch = expr[i]
        if ch in "([":
            depth += 1
            i += 1
        elif ch in ")]":
            depth -= 1
            i += 1
        elif ch == '"':
            j = i + 1
            buf: list[str] = []
            while j < n:
                if expr[j] == "\\" and j + 1 < n:
                    buf.append(expr[j:j + 2])
                    j += 2
                    continue
                if expr[j] == '"':
                    break
                buf.append(expr[j])
                j += 1
            if depth == 0:
                out.append((i, decode_cpp_escapes("".join(buf))))
            i = j + 1
        else:
            i += 1
    return out


def _f6_emit(out: list[dict[str, str]], rel: str, func: str, text: str, pos: int, lit: str) -> None:
    line_start = text.rfind("\n", 0, pos) + 1
    line_end = text.find("\n", pos)
    if line_end < 0:
        line_end = len(text)
    line_text = text[line_start:line_end].strip()
    fingerprint = hashlib.sha1(f"{func}|{line_text}".encode("utf-8")).hexdigest()[:16]
    out.append({
        "key": hardcoded_key(rel, func, fingerprint),
        "path": rel,
        "func": func,
        "text": lit,
        "linetext": line_text,
        "line": str(text.count("\n", 0, pos) + 1),
    })


def _split_top_args_spanned(text: str, s: int, e: int) -> list[tuple[int, str]]:
    """Like _split_top_args but returns (absolute-start, arg) pairs. Sound
    because _split_top_args never mutates characters: each returned arg is a
    contiguous slice of the source, so a cursor-forward find locates it."""
    args = _split_top_args(text, s, e)
    spans: list[tuple[int, str]] = []
    cursor = s
    for arg in args:
        idx = text.find(arg, cursor) if arg else cursor
        if idx < 0 or idx >= e:  # defensive: fall back to running offset
            idx = cursor
        spans.append((idx, arg))
        cursor = idx + max(len(arg), 1) + 1  # past the comma
    return spans


def _f6_wrapper_findings(rel: str, masked: str) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    for fn, slots in sorted(F6_WRAPPER_SITES.items()):
        for m in re.finditer(rf"\b{re.escape(fn)}\s*\(", masked):
            close = _match_paren(masked, m.end() - 1)
            if close < 0:
                continue
            args = _split_top_args_spanned(masked, m.end(), close)
            for slot in slots:
                if slot >= len(args):
                    continue
                astart, aexpr = args[slot]
                for off, lit in _depth0_literals(aexpr):
                    if lit not in F6_TECH_KEEP_LITERALS and _keep_visible_literal(fn, lit):
                        _f6_emit(out, rel, f"F6:{fn}", masked, astart + off, lit)
    return out


def _f6_local_site_findings(rel: str, masked: str) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    base = rel.split("/")[-1]
    for (f, fn), slots in sorted(F6_LOCAL_CALL_SITES.items()):
        if f != base:
            continue
        for m in re.finditer(rf"\b{re.escape(fn)}\s*\(", masked):
            close = _match_paren(masked, m.end() - 1)
            if close < 0:
                continue
            args = _split_top_args_spanned(masked, m.end(), close)
            for slot in slots:
                if slot >= len(args):
                    continue
                astart, aexpr = args[slot]
                for off, lit in _depth0_literals(aexpr):
                    if lit not in F6_TECH_KEEP_LITERALS and _keep_visible_literal(fn, lit):
                        _f6_emit(out, rel, f"F6:{fn}", masked, astart + off, lit)
    return out


# gate-ext (RQ-20260911-04): snprintf-into-display-buffer pairs and JSON payload
# escape slots. Exact (file, buffer/fn) anchors only — measured zero false-positive
# class (pure-% format args are excluded like elsewhere). The update-band progressText
# buffer is deliberately NOT registered (pending band).
F6_SNPRINTF_DISPLAY = {
    ("main.cpp", "blendText"),
    ("main.cpp", "chLabelBuf"),
    ("AudioEngine.cpp", "errBuf"),
}
F6_PAYLOAD_FNS = {
    ("WindowsMain.cpp", "JsonEscape"),
}
_SNP_RE_TMPL = r'(?:std::)?snprintf\s*\(\s*{buf}\s*,\s*sizeof\s*\(\s*{buf}\s*\)\s*,([^;]*)'
_JS_RE_TMPL = r'\b{fn}\s*\(((?:[^()\n]|\([^()]*\))*)\)'
_PAYLOAD_LIT_RE = re.compile(r'(?:u8|u|U|L|l)?"((?:[^"\\]|\\.)*)"')


def _gate_ext_findings(rel: str, masked: str) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    base = rel.split("/")[-1]
    for (f, buf) in sorted(F6_SNPRINTF_DISPLAY):
        if f != base:
            continue
        for m in re.finditer(_SNP_RE_TMPL.replace("{buf}", re.escape(buf)), masked):
            for off, lit in _depth0_literals(m.group(1)):
                if CJK_ANY_RE.search(lit) and lit not in F6_TECH_KEEP_LITERALS and _keep_visible_literal("snprintf", lit):
                    _f6_emit(out, rel, f"F6snprintf:{buf}", masked, m.start(1) + off, lit)
    for (f, fn) in sorted(F6_PAYLOAD_FNS):
        if f != base:
            continue
        for m in re.finditer(_JS_RE_TMPL.replace("{fn}", re.escape(fn)), masked):
            # literals may sit nested (JsonEscape(std::string("...")) — scan any
            # quoted literal in the argument; CJK-only guard keeps data safe.
            for mm in _PAYLOAD_LIT_RE.finditer(m.group(1)):
                lit = mm.group(1)
                if CJK_ANY_RE.search(lit) and _keep_visible_literal(fn, lit):
                    _f6_emit(out, rel, f"F6payload:{fn}", masked, m.start(1) + mm.start(1), lit)
    return out


def _f6_registry_findings(
    rel: str,
    text: str,
    tables: set[tuple[str, str]] | None = None,
    vars: set[tuple[str, str]] | None = None,
) -> list[dict[str, str]]:
    """Findings from the explicit (file, symbol) table/var registries.
    Comments are masked internally; tables report CJK literals, vars report
    any visible-word literal (English feeders like "Back" count too)."""
    tables = F6_TABLES if tables is None else tables
    vars = F6_VARS if vars is None else vars
    masked = mask_comments(text)
    base = rel.split("/")[-1]
    out: list[dict[str, str]] = []
    for (f, sym) in sorted(tables):
        if f != base:
            continue
        m = re.search(rf"\b{re.escape(sym)}\s*(?:\[[^\]]*\])?\s*=\s*\{{", masked)
        if not m:
            continue
        close = _match_brace(masked, m.end() - 1)
        if close < 0:
            continue
        cursor = m.end()
        while cursor < close:
            line_end = masked.find("\n", cursor)
            line_end = close if line_end < 0 or line_end > close else line_end
            line = masked[cursor:line_end]
            for off, lit in _depth0_literals(line):
                if CJK_ANY_RE.search(lit) and lit not in F6_ENDONYM_KEEP and _keep_visible_literal(sym, lit):
                    _f6_emit(out, rel, f"F6table:{sym}", masked, cursor + off, lit)
            cursor = line_end + 1
    for (f, sym) in sorted(vars):
        if f != base:
            continue
        for m in re.finditer(rf"\b{re.escape(sym)}\s*=(?!=)", masked):
            semi = masked.find(";", m.start())
            stmt = masked[m.start(): semi if semi > 0 else m.start() + 400]
            for off, lit in _depth0_literals(stmt):
                if _keep_visible_literal(sym, lit):
                    _f6_emit(out, rel, f"F6var:{sym}", masked, m.start() + off, lit)
    return out


def _match_paren(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    in_str = False
    while i < n:
        ch = text[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_str = False
        else:
            if ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


def _split_top_args(text: str, s: int, e: int) -> list[str]:
    args: list[str] = []
    depth = 0
    in_str = False
    cur: list[str] = []
    i = s
    while i < e:
        ch = text[i]
        if in_str:
            cur.append(ch)
            if ch == "\\":
                if i + 1 < e:
                    cur.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            cur.append(ch)
        elif ch in "([":
            depth += 1
            cur.append(ch)
        elif ch in ")]":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
        i += 1
    args.append("".join(cur))
    return args


def _arg_literal(arg: str) -> str | None:
    return _parse_first_string(arg, 0)


def _helper_findings(rel: str, text: str) -> list[dict[str, str]]:
    """Findings from the three registered display-helper families."""
    out: list[dict[str, str]] = []

    def emit(func: str, pos: int, raw: str) -> None:
        line_start = text.rfind("\n", 0, pos) + 1
        line_end = text.find("\n", pos)
        if line_end < 0:
            line_end = len(text)
        line_text = text[line_start:line_end].strip()
        fingerprint = hashlib.sha1(f"{func}|{line_text}".encode("utf-8")).hexdigest()[:16]
        out.append({
            "key": hardcoded_key(rel, func, fingerprint),
            "path": rel,
            "func": func,
            "text": raw,
            "linetext": line_text,
            "line": str(text.count("\n", 0, pos) + 1),
        })

    for m in SECTION_HEADER_RE.finditer(text):
        close = _match_paren(text, m.end() - 1)
        if close < 0:
            continue
        args = _split_top_args(text, m.end(), close)
        for arg in args[:2]:
            lit = _arg_literal(arg)
            if lit is not None and _keep_visible_literal("SectionHeader", lit):
                emit("SectionHeader", m.start(), lit)

    defines_row = any(re.search(rf"\bauto\s+{name}\s*=", text) for name in ROW_LABEL_FUNCS)
    if defines_row:
        for name in ROW_LABEL_FUNCS:
            for m in re.finditer(rf"\b{name}\s*\(", text):
                if text[max(0, m.start() - 5):m.start()].rstrip().endswith("auto"):
                    continue  # the definition itself
                lit = _parse_first_string(text, m.end())
                if lit is not None and _keep_visible_literal(name, lit):
                    emit(name, m.start(), lit)

    if "/" in rel:
        base = rel.split("/")[-1]
    else:
        base = rel
    for fn in sorted({f for (ff, f) in RETURN_HELPER_FNS if ff == base}):
        m = re.search(rf"\b{fn}\s*\([^)]*\)[^{{;]*\{{", text)
        if not m:
            continue
        close = text.rfind("}", m.end())
        body = text[m.end():close + 1] if close > 0 else ""
        depth = 1
        i = m.end()
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    body = text[m.end():i]
                    break
            i += 1
        # 7-0 (E2): a registered return-helper may build its label from a
        # ternary/concat in one statement; scan depth-0 literals of the whole
        # return statement, not only `return "lit";`.
        for rm in re.finditer(r"\breturn\b([^;]*);", body):
            for off, lit in _depth0_literals(rm.group(1)):
                if _keep_visible_literal(fn, lit):
                    emit(fn, m.end() + rm.start(1) + off, lit)

    base = rel.split("/")[-1] if "/" in rel else rel
    for (ff, fn), slot in sorted(SEARCH_INDEX_TITLE_FUNCS.items()):
        if ff != base:
            continue
        for m in re.finditer(rf"\b{fn}\s*\(", text):
            if text[max(0, m.start() - 6):m.start()].rstrip().endswith("auto"):
                continue  # lambda definition site
            close = _match_paren(text, m.end() - 1)
            if close < 0:
                continue
            args = _split_top_args(text, m.end(), close)
            if len(args) <= slot:
                continue
            lit = _arg_literal(args[slot])
            if lit is not None and _keep_visible_literal(fn, lit):
                emit(fn, m.end(), lit)
    return out


class PurityError(ValueError):
    pass


def _strip_exempt(text: str) -> str:
    stripped = FORMAT_RE.sub(" ", text)
    stripped = TEMPLATE_PLACEHOLDER_RE.sub(" ", stripped)   # <字段名:N> user syntax
    for phrase in sorted(PROTECTED_PHRASES, key=len, reverse=True):
        stripped = re.sub(re.escape(phrase), " ", stripped, flags=re.I)
    stripped = DOTTED_NAME_RE.sub(" ", stripped)            # files / paths / URLs
    # Boundary fix (batch-4 ruling): \b fails when a unit/token sits directly
    # against a CJK char or a digit (仅WAV, 80Hz, 12ms, FFT大小). The guard is
    # letter-only on both sides, so digit/CJK adjacency matches while real
    # words inside English text (Chain≠AI) are no longer silently eaten.
    for token in sorted(PROPER_NOUNS, key=len, reverse=True):
        stripped = re.sub(rf"(?<![A-Za-z]){re.escape(token)}(?![A-Za-z])", " ", stripped, flags=re.I)
    for token in UNITS:
        stripped = re.sub(rf"(?<![A-Za-z]){re.escape(token)}(?![A-Za-z])", " ", stripped, flags=re.I)
    stripped = re.sub(r"[0-9%./:()\[\]{}+\-_,;|\\<>=]+", " ", stripped)
    return stripped


def is_language_mix(text: str) -> bool:
    if not text:
        return False
    remnant = _strip_exempt(IMGUI_ID_SUFFIX_RE.sub("", text))
    return bool(CJK_RE.search(remnant) and LATIN_WORD_RE.search(remnant))


def is_simplified_chinese_in_japanese(text: str) -> bool:
    """Return whether ja contains a character specific to simplified Chinese."""
    return bool(text) and any(char in SIMPLIFIED_ONLY_CHARS for char in text)


def residual_tokens(text: str) -> set[str]:
    """Latin word tokens (>=2 letters, digits allowed inside) that still sit
    in the text after every global exemption. Single letters (C 盘, L+R, N>2)
    never carry a mix by themselves."""
    body = IMGUI_ID_SUFFIX_RE.sub("", text)
    body = FORMAT_RE.sub(" ", body)
    body = TEMPLATE_PLACEHOLDER_RE.sub(" ", body)
    for phrase in sorted(PROTECTED_PHRASES, key=len, reverse=True):
        body = re.sub(re.escape(phrase), " ", body, flags=re.I)
    body = DOTTED_NAME_RE.sub(" ", body)
    for token in sorted(PROPER_NOUNS, key=len, reverse=True):
        body = re.sub(rf"(?<![A-Za-z]){re.escape(token)}(?![A-Za-z])", " ", body, flags=re.I)
    for token in UNITS:
        body = re.sub(rf"(?<![A-Za-z]){re.escape(token)}(?![A-Za-z])", " ", body, flags=re.I)
    return {
        w.lower()
        for w in re.findall(r"[A-Za-z][A-Za-z0-9]*", body)
        if re.search(r"[A-Za-z]{2}", w)
    }


# Row-scoped notation registry (knife-4 batch-4 ruling): on these rows the
# Latin tokens ARE the data/literal content itself (channel-label conventions,
# query operators, status values rendered verbatim elsewhere, example words in
# syntax help). Protection is limited to the exact per-row token set; any
# future prose mixed into the same row still fails the gate. Each entry's
# source is the triage table (docs/process/RQ-20260909-02-knife5-triage.md).
PROTECTED_ROW_TOKENS = {
    # About page: these product and license names are official names on these
    # exact rows. Additional English prose in either row remains a violation.
    "Main_AboutLicenseNote": {"apache", "license"},
    "Main_AboutCindyCredit": {"cindy"},
    "Main_ChannelOrderHint": {"smpte", "film", "ambix", "acn", "sn3d", "lfe", "ls", "rs"},
    "Main_DownmixCenterGainHint": {"center"},
    "Set_DownmixCenterDb": {"center"},
    "Main_EnglishMaterialHint": {"english"},
    "Main_ExcludeWordTip": {"not"},
    "Main_SearchMultiKeywordHint": {"and"},
    "Main_MissingLibraryReconnectTip": {"missing", "disconnected"},
    "Main_ScanErrorsNoFileFmt": {"scan"},
    "Main_SystemFontHint": {"info"},
    "Set_BuiltinSourceHanSans": {"sc"},
    "Nav_KeyPlaceholder": {"major"},
    "Playlist_UpgradeForFolder": {"standard"},
    "Playlist_NewInFolderLocked": {"standard"},
    "ModuleUI_EmbedIxmlUser": {"user"},
    "ModuleUI_EmbedMetadataIntro": {"list", "info"},
    "ModuleUI_UCSBrowserTitle": {"universal", "category", "system"},
    "Search_SyntaxAND": {"explosion", "fire", "and"},
    "Search_SyntaxEx1": {"explosion", "fire", "and"},
    "Search_SyntaxOR": {"explosion", "fire", "or"},
    "Search_SyntaxEx2": {"explosion", "thunder", "or"},
    "Search_SyntaxEx3": {"car", "door"},
    # Lead 7-A mix ruling tier 3: Reverse/Mono/Tap look like ordinary English
    # words, so they are bound to their exact keys instead of the global
    # package — the same anti-smuggling logic that created this registry.
    # 2026-11-20 external AI page: the nav row names the feature by its
    # domain acronyms; no other row may carry them.
    "SetUX_ExternalAiMcp": {"ai", "mcp"},
    "ExtAi_AiSetupPromptFmt": {"codex", "config", "harken", "home", "mcp", "profile", "server", "toml"},
    "Sc_ToggleReverse": {"reverse"},
    "Sc_ToggleMono": {"mono"},
    "ScDesc_TapTempo": {"tap"},
    # 9b: row-bound tokens (lead tier-3 precedent): "mini" and "px" are domain
    # terms/units appearing in these exact cells; NOT globalised — smuggling
    # elsewhere stays red.
    "Wf_StageMiniWave": {"mini"},
    "CoverResLow": {"px"},
    "CoverResMid": {"px"},
    "CoverResHigh": {"px"},
    # 9d: original runtime literals already carried these latin domain tokens
    # (mini/metadata) and the path: search operator — row-bound, never global.
    "Scan_FullModeHint": {"mini", "metadata"},
    "Scan_FastModeHint": {"mini", "metadata"},
    "Search_NoResultTips": {"path"},
    "Search_SyntaxNOT": {"not", "music"},
    "Search_SyntaxField": {"category", "amb"},
    "Search_SyntaxCat": {"category", "amb"},
    "Search_SyntaxArtist": {"artist", "composer"},
    "Search_SyntaxDesc": {"description", "forest"},
    "Search_SyntaxKw": {"keywords", "loop"},
}


def row_mix_is_protected(entry_id: str, text: str) -> bool:
    allowed = PROTECTED_ROW_TOKENS.get(entry_id)
    if allowed is None:
        return False
    return residual_tokens(text) <= allowed


def load_ledger(path: Path = LEDGER_PATH) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


# Stable keys are reproducible across re-runs; they deliberately avoid the raw
# line number for hardcoded UI copy (line drift) so a moved-but-unchanged
# literal stays inside the baseline.

def ledger_mix_key(entry_id: str, lang: str) -> str:
    return f"mix|{entry_id}|{lang}"


def hardcoded_key(path: str, func: str, line_fingerprint: str) -> str:
    return f"ui|{path}|{func}|{line_fingerprint}"


def japanese_missing_key(entry_id: str) -> str:
    return f"ja|{entry_id}"


def mojibake_key(entry_id: str, lang: str) -> str:
    return f"mojibake|{entry_id}|{lang}"


def find_ledger_mixes(ledger: dict[str, Any] | None = None) -> list[dict[str, str]]:
    payload = ledger if ledger is not None else load_ledger()
    findings: list[dict[str, str]] = []
    for entry in payload.get("entries", []):
        ident = str(entry.get("id", ""))
        if ident in FROZEN_IDS:
            continue
        for lang in ("zh", "en"):
            value = entry.get(lang, "")
            if isinstance(value, str) and is_language_mix(value) \
                    and not row_mix_is_protected(ident, value):
                findings.append(
                    {
                        "key": ledger_mix_key(ident, lang),
                        "id": ident,
                        "lang": lang,
                        "text": value,
                    }
                )
        japanese = entry.get("ja", "")
        if isinstance(japanese, str) and is_simplified_chinese_in_japanese(japanese):
            findings.append(
                {
                    "key": ledger_mix_key(ident, "ja"),
                    "id": ident,
                    "lang": "ja",
                    "text": japanese,
                }
            )
    return findings


def find_missing_japanese(ledger: dict[str, Any] | None = None) -> list[dict[str, str]]:
    payload = ledger if ledger is not None else load_ledger()
    missing: list[dict[str, str]] = []
    for entry in payload.get("entries", []):
        ident = str(entry.get("id", ""))
        japanese = entry.get("ja")
        if (not isinstance(japanese, str) or japanese == "") and ident != "Auth_ContactAdmin":
            missing.append({"key": japanese_missing_key(ident), "id": ident})
    return missing


def find_mojibake(ledger: dict[str, Any] | None = None) -> list[dict[str, str]]:
    """Hard gate (never baselined): latin-1 double-encoding artifacts.

    Catches the exact corruption family that hit the knife-4 authoring
    pipeline: 'â' + C1 control (→, –, …) and 'Â/Ã/Å' + supplement (±, ‰).
    Any hit is a live product-display defect, so it fails immediately.
    """
    payload = ledger if ledger is not None else load_ledger()
    hits: list[dict[str, str]] = []
    for entry in payload.get("entries", []):
        ident = str(entry.get("id", ""))
        for lang in ("zh", "en", "ja"):
            value = entry.get(lang, "")
            if isinstance(value, str) and MOJIBAKE_RE.search(value):
                hits.append({"key": mojibake_key(ident, lang), "id": ident, "lang": lang, "text": value})
    return hits


def _parse_first_string(text: str, start: int) -> str | None:
    i = start
    n = len(text)
    while i < n and text[i] in " \t\r\n":
        i += 1
    if text.startswith(("L(", "GetLocalizedString", "wz("), i):
        return None
    pieces: list[str] = []
    while True:
        while i < n and text[i] in " \t\r\n":
            i += 1
        if text.startswith('u8"', i):
            i += 2
        if i >= n or text[i] != '"':
            break
        i += 1
        chunk: list[str] = []
        while i < n:
            ch = text[i]
            if ch == "\\" and i + 1 < n:
                chunk.append(text[i : i + 2])
                i += 2
                continue
            if ch == '"':
                i += 1
                pieces.append("".join(chunk))
                break
            chunk.append(ch)
            i += 1
        else:
            break
        while i < n and text[i] in " \t\r\n":
            i += 1
        if i < n and (text[i] == '"' or text.startswith('u8"', i)):
            continue
        break
    if not pieces:
        return None
    # 7-0 (E4): decode \uXXXX / \xNN so ASCII-escaped CJK literals (34 in-tree
    # sites at 95e0c027) are visible to every downstream CJK/keep check.
    return decode_cpp_escapes("".join(pieces))


def _parse_arg_string(text: str, start: int) -> str | None:
    """First string literal inside the argument list when arg 1 is not text.

    Used for calls like TextColored(ImVec4(...), "label"): scan forward from
    the call start for the first quote, refusing to cross a statement end.
    """
    end = text.find(";", start)
    q = text.find('"', start)
    if q < 0 or (end >= 0 and q > end) or q - start > 300:
        return None
    if "{" in text[start:q] or "//" in text[start:q]:
        return None
    return _parse_first_string(text, q)


def _keep_visible_literal(func: str, literal: str) -> bool:
    visible = IMGUI_ID_SUFFIX_RE.sub("", literal)
    if not visible or literal.startswith("#"):
        return False
    # 7-0 (E5): a single CJK character IS copy ("无" in "ローカル: 无"); the
    # old len<2 latin heuristic silently dropped it. Latin keeps its guard.
    if CJK_ANY_RE.search(visible):
        return True
    if not re.search(r"[A-Za-z]", visible):
        return False
    if len(visible.strip()) < 2:
        return False
    if visible.startswith("%") and not re.search(r"[A-Za-z]{2,}", visible):
        return False
    return True


def _win32_findings(rel: str, text: str) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []

    def emit(api: str, pos: int, lit: str) -> None:
        if not re.search(r"[A-Za-z一-鿿぀-ヿ]", lit):
            return  # pure punctuation fragments are not copy
        if re.fullmatch(r"(?:\\[nrt]){1,}", lit):
            return  # escape-only fragments (\n runs) are not copy
        if lit.startswith("\\0") or "\\0*" in lit:
            return  # dialog filter PATTERN segments (\0*.ext) are technical constants, not copy (lead W6-2: patterns stay in code)
        if (rel, lit) in WIN32_KEEP_LITERALS:
            return  # ironclad fallback literals kept by ruling (see registry)
        line_start = text.rfind("\n", 0, pos) + 1
        line_end = text.find("\n", pos)
        if line_end < 0:
            line_end = len(text)
        line_text = text[line_start:line_end].strip()
        fingerprint = hashlib.sha1(f"Win32:{api}|{line_text}".encode("utf-8")).hexdigest()[:16]
        out.append({
            "key": hardcoded_key(rel, f"Win32:{api}", fingerprint),
            "path": rel,
            "func": f"Win32:{api}",
            "text": lit,
            "linetext": line_text,
            "line": str(text.count("\n", 0, pos) + 1),
        })

    base = rel.split("/")[-1]
    for (ff, api), slots in sorted(WIN32_CALL_SITES.items()):
        if ff != base:
            continue
        for m in re.finditer(rf"\b{re.escape(api)}\s*\(", text):
            close = _match_paren(text, m.end() - 1)
            if close < 0:
                continue
            args = _split_top_args(text, m.end(), close)
            for slot in slots:
                if slot < len(args):
                    for lit in WIDE_LIT_RE.findall(args[slot]):
                        emit(api, m.start(), lit)

    for f, anchor in WIN32_ASSIGN_SITES:
        if f != rel:
            continue
        for m in re.finditer(anchor, text):
            semi = text.find(";", m.start())
            stmt = text[m.start(): semi if semi > 0 else m.start() + 400]
            for lit in WIDE_LIT_RE.findall(stmt):
                emit("assign", m.start(), lit)
    return out


def find_hardcoded_ui(src_root: Path = SRC_ROOT) -> list[dict[str, str]]:
    findings: list[dict[str, str]] = []
    legacy_stock = load_baseline(src_root.parent / BASELINE_PATH.relative_to(ROOT))
    legacy_files = {key.split("|", 2)[1] for key in legacy_stock if key.startswith("ui|")}
    files = sorted(list(src_root.rglob("*.cpp")) + list(src_root.rglob("*.h")))
    for path in files:
        rel = path.relative_to(src_root).as_posix()
        if any(part in SKIP_DIR_PARTS for part in rel.split("/")):
            continue
        if path.name in SKIP_FILE_NAMES or "generated" in path.name:
            continue
        out_of_scope = _COVERAGE_FILE_SKIP_RE.match(path.name)
        if out_of_scope and rel not in legacy_files:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        masked = mask_comments(text)
        code, _ = _coverage_lex(text)
        first_finding = len(findings)

        def emit_call(func: str, func_pos: int, literal: str) -> None:
            line_no = text.count("\n", 0, func_pos)
            line_start = text.rfind("\n", 0, func_pos) + 1
            line_end = text.find("\n", func_pos)
            if line_end < 0:
                line_end = len(text)
            line_text = text[line_start:line_end].strip()
            fingerprint = hashlib.sha1(
                f"{func}|{line_text}".encode("utf-8")
            ).hexdigest()[:16]
            findings.append(
                {
                    "key": hardcoded_key(rel, func, fingerprint),
                    "path": rel,
                    "func": func,
                    "text": literal,
                    "linetext": line_text,
                    "line": str(line_no + 1),
                }
            )

        for match in UI_CALL_RE.finditer(code):
            func = match.group(1)
            literal = _parse_first_string(masked, match.end())
            if literal is None and func in COLOR_FIRST_FUNCS:
                literal = _parse_arg_string(masked, match.end())
            if (literal is not None and _keep_visible_literal(func, literal)
                    and not (func in _COVERAGE_FORMAT_SLOTS and _is_technical_fragment(literal.lstrip()))):
                emit_call(func, match.start(), literal)
                continue
            # 7-0 (E5): arg 0 carried no usable copy — either no literal at all
            # (L(id) / variable / ternary) or a bare format slot ("%s"). Visible
            # copy may then hide at depth 0 of ANY top-level argument
            # (Text(L(fmt), x ? u8"无" : v)). ID-ish arg0 ("##x", "×") stays
            # suppressed exactly as before.
            if literal is None or literal.lstrip().startswith("%"):
                close = _match_paren(code, match.end() - 1)
                if close > 0:
                    for ai, (astart, aexpr) in enumerate(
                            _split_top_args_spanned(masked, match.end(), close)):
                        # MenuItem(label, "Ctrl+O", ...): the chord slot is
                        # global keycap notation (batch-4 ratified keycaps),
                        # never copy — same never-scanned-slot precedent as
                        # the search-index keywords corpus (F4b ruling).
                        if ai in SECONDARY_DENY_SLOTS.get(func, ()):
                            continue
                        for off, lit in _depth0_literals(aexpr):
                            if _keep_visible_literal(func, lit) and not _is_technical_fragment(lit.lstrip()):
                                emit_call(func, astart + off, lit)
        findings.extend(_f6_wrapper_findings(rel, masked))
        findings.extend(_f6_local_site_findings(rel, masked))
        findings.extend(_f6_registry_findings(rel, masked))
        findings.extend(_gate_ext_findings(rel, masked))
        findings.extend(_helper_findings(rel, text))
        findings.extend(_win32_findings(rel, text))
        if out_of_scope:
            findings[first_finding:] = [item for item in findings[first_finding:] if item["key"] in legacy_stock]
    return [x for x in findings
            if x["text"] not in F6_VISIBLE_KEEP and x["text"] not in F6_TECH_KEEP_LITERALS]


def all_current_violations(root: Path = ROOT) -> list[dict[str, str]]:
    ledger = load_ledger(root / "locale" / "harken.ui.strings.json")
    return [
        *find_ledger_mixes(ledger),
        *find_hardcoded_ui(root / "src"),
        *find_missing_japanese(ledger),
        *find_mojibake(ledger),
    ]


def build_baseline_keys(root: Path = ROOT) -> list[str]:
    # Baseline only covers mixed-language copy and hardcoded UI literals.
    # Missing Japanese and mojibake are hard gates, never baselined.
    ledger = load_ledger(root / "locale" / "harken.ui.strings.json")
    keys = [item["key"] for item in find_ledger_mixes(ledger)]
    keys += [item["key"] for item in find_hardcoded_ui(root / "src")]
    return sorted(set(keys))


def load_baseline(path: Path = BASELINE_PATH) -> set[str]:
    if not path.is_file():
        return set()
    payload = json.loads(path.read_text(encoding="utf-8"))
    return {str(key) for key in payload.get("keys", [])}


GROWTH_OWNER_RQ_BUG_RE = re.compile(r"^(RQ|BUG)-[0-9]{8}-[0-9]{2}$")
GROWTH_OWNER_GOV_RE = re.compile(r"^GOV-[0-9]{8}-[A-Z0-9-]+$")


def _valid_growth_owner(owner: str) -> bool:
    return bool(GROWTH_OWNER_RQ_BUG_RE.match(owner) or GROWTH_OWNER_GOV_RE.match(owner))


def check_baseline_growth(payload: dict[str, Any]) -> None:
    """Refuse unexempted growth of the committed purity baseline.

    stockKeys is the frozen stock set; cap must equal len(stockKeys).
    extras = keys not in stockKeys, and every extra needs a valid
    growthExemptions entry. Exempting a stock key cannot hide extras.
    Exemptions pointing at missing keys are dead and fail. Shrinking
    keys is allowed without changing cap/stockKeys.
    """
    if payload.get("schema") != 2:
        raise PurityError("purity baseline schema must be 2")
    stock_raw = payload.get("stockKeys")
    if not isinstance(stock_raw, list):
        raise PurityError("purity baseline stockKeys must be a list")
    stock_keys = [str(key) for key in stock_raw]
    stock_set = set(stock_keys)
    cap = payload.get("cap")
    if not isinstance(cap, int) or isinstance(cap, bool) or cap != len(stock_keys):
        raise PurityError("purity baseline cap must equal len(stockKeys)")
    keys = [str(key) for key in payload.get("keys", [])]
    key_set = set(keys)
    extras = [key for key in keys if key not in stock_set]
    exemptions = payload.get("growthExemptions", {})
    if exemptions is None:
        exemptions = {}
    if not isinstance(exemptions, dict):
        raise PurityError("growthExemptions must be an object")

    dead = sorted(str(key) for key in exemptions if str(key) not in key_set)
    if dead:
        raise PurityError(
            "dead growth exemption(s) not present in keys: " + ", ".join(dead)
        )

    for raw_key, meta in exemptions.items():
        key = str(raw_key)
        if not isinstance(meta, dict):
            raise PurityError(f"{key} growth exemption must be an object with owner and reason")
        owner = str(meta.get("owner", "")).strip()
        reason = str(meta.get("reason", "")).strip()
        if not _valid_growth_owner(owner):
            raise PurityError(
                f"{key} growth exemption owner must match "
                "^(RQ|BUG)-[0-9]{8}-[0-9]{2}$ or ^GOV-[0-9]{8}-[A-Z0-9-]+$"
            )
        if not reason:
            raise PurityError(f"{key} growth exemption reason must be non-empty")

    unexempted_extras = [key for key in extras if key not in exemptions]
    if unexempted_extras:
        raise PurityError(
            "unexempted baseline growth key(s): " + ", ".join(unexempted_extras)
        )


def describe_violation(item: dict[str, str]) -> str:
    key = item["key"]
    if key.startswith("mix|"):
        return f"{item['id']}[{item['lang']}]: {item['text']}"
    if key.startswith("ja|"):
        return f"{item['id']}[ja]: 缺日语"
    if key.startswith("mojibake|"):
        return f"{item['id']}[{item['lang']}]: 双重编码乱码 {item['text'][:40]!r}"
    return f"{item['path']}:{item['line']} {item['func']}(): {item['text']}"


def find_new_violations(
    items: list[dict[str, str]],
    baseline: set[str],
) -> list[dict[str, str]]:
    return [item for item in items if item["key"] not in baseline]


_COVERAGE_TOKEN_RE = re.compile(
    r'//(?:\\\n|[^\n])*|/\*[\s\S]*?\*/'
    r'|(?:u8|[uUL])?R"(?P<delimiter>[^()\\\s]{0,16})\((?P<raw>[\s\S]*?)\)(?P=delimiter)"'
    r'|(?:u8|[uUL@])?"(?P<quoted>(?:\\[\s\S]|[^"\\\n])*)"'
    r"|(?<![\w])(?:u8|[uUL])?'(?:\\[\s\S]|[^'\\\n])+'"
)
_COVERAGE_KEY_RE = re.compile(r"\b(?:(?:L|Lw)\s*\(\s*(\w+)\s*\)|LStr\s*::\s*(\w+))")
_COVERAGE_FILE_SKIP_RE = re.compile(r"^(?:CoverWallLab|(?:Settings)?(?:DeveloperTools|Diagnostic)|DevTools|Wwise)", re.I)
_COVERAGE_INTERNAL_FUNCTION_RE = re.compile(r"^(?:Draw|Render|Show|Open)\w*(?:Diagnostic|Wwise|CoverWallLab)")
_COVERAGE_INTERNAL_CONTEXTS = {
    ("src/ThemeEditor.cpp", "ThemeDiagPublicThemeLabel"),
    ("src/WindowsMain.cpp", "WriteBugReportJson"),
    ("src/ThemeMascot.cpp", "ThemeMascot::drawSleepZzz"),
}
_COVERAGE_IMGUI_LABELS = {
    *UI_LABEL_FUNCTIONS,
    "BeginCombo", "BeginListBox", "ColorEdit3", "ColorEdit4", "ColorPicker3", "ColorPicker4",
    "DragFloat", "DragFloat2", "DragFloat3", "DragFloat4", "DragFloatRange2",
    "DragInt", "DragInt2", "DragInt3", "DragInt4", "DragIntRange2",
    "InputDouble", "InputFloat", "InputFloat2", "InputFloat3", "InputFloat4",
    "InputInt2", "InputInt3", "InputInt4", "InputScalar", "InputScalarN",
    "InputTextMultiline", "SliderAngle", "SliderFloat", "SliderFloat2", "SliderFloat3",
    "SliderFloat4", "SliderInt2", "SliderInt3", "SliderInt4", "SliderScalar", "SliderScalarN",
    "TableHeader", "TextLink", "TextLinkOpenURL", "VSliderFloat", "VSliderInt", "VSliderScalar",
}
_COVERAGE_IMGUI_SLOTS = {
    **{name: (0,) for name in _COVERAGE_IMGUI_LABELS - {"BeginItemTooltip"}},
    "BeginCombo": (0, 1), "InputTextWithHint": (0, 1), "TextColored": (1,),
    "LabelText": (0, 1), "ProgressBar": (2,), "Combo": (0, 2),
    "SetItemTooltip": (0,), "PlotLines": (0, 4), "PlotHistogram": (0, 4),
}
_COVERAGE_FORMAT_SLOTS = {
    "Text": 0, "TextDisabled": 0, "TextWrapped": 0, "TextColored": 1, "LabelText": 1,
    "BulletText": 0, "SetTooltip": 0, "SetItemTooltip": 0,
}
_COVERAGE_WRAPPERS = {
    **F6_WRAPPER_SITES,
    "SectionHeader": (0,), "BeginStandaloneWindow": (0,), "BeginAlertDialog": (1,),
    "DrawSettingsSearchableLabel": (0,), "NotifySettingsSearchLabeledItem": (0,),
    "SettingsButtonRow": (0,), "SettingsSliderRow": (0, 5), "SettingsToggleRow": (0,),
    "HintAwareInputTextWithHint": (0, 1), "DrawForegroundMaintenanceToastCard": (0,),
    "ActionButton": (0,), "SmallActionButton": (0,), "IconButton": (1,), "TextInput": (0,),
    "NumberInput": (0, 5), "ComboMulti": (0, 2), "Tooltip": (0,),
    "CreateWindowExW": (2,), "CreateWindowExA": (2,), "SetWindowTextW": (1,), "SetWindowTextA": (1,),
    "MessageBox": (1, 2), "MessageBoxW": (1, 2), "MessageBoxA": (1, 2), "ShowMessageBox": (1, 2),
    "AppendMenuW": (3,), "AppendMenuA": (3,), "AddText": (),
    "PopupTitle": (0,), "PopupMuted": (0,), "registerField": (1, 2),
}
_COVERAGE_CALL_RE = re.compile(r"\b(?P<name>"
    + "|".join(re.escape(name) for name in sorted(
        {f"ImGui::{name}" for name in _COVERAGE_IMGUI_SLOTS}
        | {f"aura::ui::{name}" for name in ("Button", "Checkbox", "Radio", "Toggle", "SliderFloat", "SliderRange", "Combo")}
        | set(_COVERAGE_WRAPPERS), key=lambda name: (-len(name), name)))
    + r")\s*\(")
_COVERAGE_FIELD_RE = re.compile(
    r"\b(?:display(?:Name|Label)(?:_?(?:Zh|En|Ja))?|lpstr(?:Title|Filter)|"
    r"placeholder|trailingTooltip|errorText|tooltip)\b",
    re.I,
)
_COVERAGE_OBJC_RE = re.compile(r"\b(setTitle|setMessageText|setInformativeText|addButtonWithTitle):")
_COVERAGE_DISPLAY_FN_RE = re.compile(
    r"(?i)(?:\w*Display(?:Name|Label)(?:For\w+|Utf8)?|GetWorkflowNode(?:Name|Help)|"
    r"getNodeType(?:Name(?:Zh)?|Description)|\w+(?:Theme|Preset|Mode)(?:Name|Label|Description)|"
    r"ThemeEditorGroup(?:Name|Hint)Zh)"
)
_COVERAGE_FUNCTION_RE = re.compile(
    r"(?m)^[ \t]*(?:(?!(?:return|if|for|while|switch)\b)[\w:<>,*&]+[ \t]+)+"
    r"(?P<name>(?:\w+::)*\w+)\s*\("
)
_COVERAGE_FUNCTION_BODY_RE = re.compile(r"\s*(?:(?:const|noexcept|override|final)\s*)*\{")
_COVERAGE_ASSIGN_RE = re.compile(r"\b(\w+)\s*(?:\[[^\]\n]*\]\s*)?=(?![=>])")
_COVERAGE_BRANCH_RE = re.compile(r"\b(?:if(?:\s+constexpr)?|switch|for|while|catch)\s*\(")
_COVERAGE_BRANCH_KEYWORD_RE = re.compile(r"\b(?:else|do|try)\b\s*")
_COVERAGE_SPACE_RE = re.compile(r"\s*")
_COVERAGE_LOCAL_RE = re.compile(
    r"(?:^|(?<=[;{}]))\s*(?:(?:const|constexpr|static|volatile)\s+)*"
    r"(?!return\b|throw\b)[\w:]+(?:<[^<>{};\n]+>)?(?:\s+|\s*[*&]+\s*)"
    r"(?P<name>\w+)\s*(?=[=;\[])"
)
_COVERAGE_RETURN_RE = re.compile(r"\breturn\b([^;]*);")
_COVERAGE_VARIABLE_RE = re.compile(r"\s*(\w+)(?:\.(?:c_str|data)\(\))?\s*")
_COVERAGE_COMPARISON_RE = re.compile(r"(?:==|!=|<=|>=|&&|\|\|)")
_COVERAGE_TRANSPARENT_RE = re.compile(
    r"\b(?:string|wstring|string_view|LedgerText|Utf8ToWide|WideFromUtf8|format|MakeLabel)\s*$"
)
_COVERAGE_TOKEN_FORMAT_RE = re.compile(
    r"(?<!%)%(?:\d+\$)?[-+# 0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
    r"(?:hh|ll|[hlLjzt]|I64)?[diouxXfFeEgGaAcspn%]"
)
_COVERAGE_UNIT_RE = re.compile(
    r"\b(?:" + "|".join(re.escape(unit) for unit in sorted(UNITS, key=len, reverse=True)) + r")\b",
    re.I,
)
_COVERAGE_ESCAPE_RE = re.compile(r"(?:(?:\\x[0-9a-fA-F]{2})+|\\[0-7]{1,3}|\\(?:u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|[nrtvbf'\"\\?]|\n))")
_COVERAGE_NON_COPY = {
    ("src/main.cpp", "ImGui::TableSetupColumn[0]"): {"k", "v"},
    ("src/main.cpp", "ImGui::SmallButton[0]"): {"X"},
    ("src/main.cpp", "assignment:bmTxt"): {"BM", "bm"},
    ("src/update/HarkenSearchUpdateNoticeUi.cpp", "ImGui::TableSetupColumn[0]"): {"title", "author"},
    ("src/CubaseAdapter.cpp", "return:GetHostDisplayName"): {"Cubase", "Nuendo"},
}


def _coverage_unescape(quoted: str) -> str:
    def replace(match: re.Match[str]) -> str:
        token = match.group()
        if token.startswith("\\x"):
            raw = bytes.fromhex(token.replace("\\x", ""))
            try:
                return raw.decode("utf-8")
            except UnicodeDecodeError:
                return raw.decode("latin-1")
        if token[1].isdigit():
            return chr(int(token[1:], 8))
        return "" if token == "\\\n" else decode_cpp_escapes(token)

    return _COVERAGE_ESCAPE_RE.sub(replace, quoted)


def _coverage_lex(source: str) -> tuple[str, list[tuple[int, int, str]]]:
    literals = []

    def blank(match: re.Match[str]) -> str:
        if match["raw"] is not None:
            literals.append((*match.span(), match["raw"]))
        elif match["quoted"] is not None:
            literals.append((*match.span(), _coverage_unescape(match["quoted"])))
        return re.sub(r"[^\n]", " ", match.group())

    return _COVERAGE_TOKEN_RE.sub(blank, source), literals


def _coverage_arguments(code: str, start: int, end: int) -> list[tuple[int, int]]:
    arguments = []
    depth = 0
    for offset, char in enumerate(code[start:end], start):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            arguments.append((start, offset))
            start = offset + 1
    arguments.append((start, end))
    return arguments


def _coverage_functions(code: str) -> list[tuple[int, int, str]]:
    functions = []
    for match in _COVERAGE_FUNCTION_RE.finditer(code):
        close = _match_paren(code, match.end() - 1)
        if close < 0:
            continue
        body = _COVERAGE_FUNCTION_BODY_RE.match(code, close + 1)
        if body:
            start = body.end()
            end = _match_brace(code, start - 1)
            if end >= 0:
                functions.append((start, end, match["name"]))
    return functions


def _coverage_caption(text: str, site: str, path: str) -> str:
    visible = text.split("##", 1)[0].strip()
    if visible in F6_VISIBLE_KEEP | F6_TECH_KEEP_LITERALS | F6_ENDONYM_KEEP or visible in PROTECTED_PHRASES:
        return ""
    if visible in _COVERAGE_NON_COPY.get((path, site), ()):
        return ""
    if (path.removeprefix("src/"), visible) in WIN32_KEEP_LITERALS:
        return ""
    if site == "lpstrFilter":
        visible = " / ".join(visible.split("\0")[::2]).strip(" /")
    content = _COVERAGE_TOKEN_FORMAT_RE.sub(" ", visible)
    content = _COVERAGE_UNIT_RE.sub(" ", content)
    return visible if any(char.isalpha() for char in content) else ""


def _coverage_window(path: str, context: str) -> str:
    scope = path.split("/")[-1] if not path.endswith(("/main.cpp", "/ModuleUI.cpp")) else context
    for pattern, window in (
        (r"Workflow(?:Engine|Node|Dialog|Preview)", "工作流引擎"),
        (r"Transfer|Export|DAW|Reaper|Cubase|ProTools", "传输与导出（仅盘点）"),
        (r"Theme|Appearance", "主题与外观"),
        (r"Settings|Preference|Shortcut", "设置与快捷键"),
        (r"Cover", "封面"),
        (r"Waveform|Audition|MagicEQ", "波形与试听"),
        (r"Similar", "相似音效"),
        (r"Sidebar|Playlist|Browser|FolderTree", "侧栏、浏览器与列表"),
        (r"Db|Database|Scan|Relink|Library", "数据库与扫描"),
        (r"Metadata|Rename|FileInfo|Field", "元数据与文件信息"),
        (r"(?:^AI|ExternalAi|AIEngine|AISettings|AIRecognition|AISearch)", "AI 工具"),
        (r"Bootstrap|WindowsMain|Consent|app_entry|UpdateNotice", "启动、同意与更新"),
    ):
        if re.search(pattern, scope, re.I):
            return window
    if path.startswith("src/platform/"):
        return "系统窗口与文件对话框"
    return "主窗口与通用控件"


def _coverage_sites(path: str, source: str, *, include_all: bool) -> list[dict[str, Any]]:
    code, literals = _coverage_lex(source)
    if not include_all and not literals:
        return []
    starts = [literal[0] for literal in literals]
    line_ends = [match.start() for match in re.finditer("\n", source)]
    functions = _coverage_functions(code)
    function_starts = [start for start, _, _ in functions]
    brace_offsets = []
    brace_scopes = []
    active_scope = -1
    scope_parent = {}
    for brace in re.finditer(r"[{}]", code):
        offset = brace.start()
        if brace.group() == "{":
            scope_parent[offset] = active_scope
            active_scope = offset
        else:
            active_scope = scope_parent.get(active_scope, -1)
        brace_offsets.append(offset)
        brace_scopes.append(active_scope)

    def scope_at(position: int) -> int:
        scope_index = bisect_left(brace_offsets, position) - 1
        return brace_scopes[scope_index] if scope_index >= 0 else -1

    branch_bodies = set()
    for match in _COVERAGE_BRANCH_RE.finditer(code):
        close = _match_paren(code, match.end() - 1)
        if close >= 0:
            branch_bodies.add(_COVERAGE_SPACE_RE.match(code, close + 1).end())
    branch_bodies.update(match.end() for match in _COVERAGE_BRANCH_KEYWORD_RE.finditer(code))
    statement_blocks = branch_bodies | {match.end() for match in re.finditer(r"[;:{}]\s*(?=\{)", code)}
    local_bindings = {}
    for match in _COVERAGE_LOCAL_RE.finditer(code):
        position = match.start("name")
        local_bindings.setdefault((scope_at(position), match["name"]), position)
    references = list(_COVERAGE_KEY_RE.finditer(code))
    reference_starts = [match.start() for match in references]
    assignments: dict[str, list[re.Match[str]]] = defaultdict(list)
    for match in _COVERAGE_ASSIGN_RE.finditer(code):
        assignments[match[1]].append(match)
    rows: list[dict[str, Any]] = []
    recorded: set[tuple[int, str, str]] = set()
    used_literals: set[int] = set()

    def context_at(position: int) -> str:
        index = bisect_right(function_starts, position) - 1
        while index >= 0:
            _, end, name = functions[index]
            if position < end:
                return name
            index -= 1
        return ""

    def emit(site: str, anchor: int, position: int, kind: str, value: str = "") -> None:
        context = context_at(anchor)
        if ((path, context) in _COVERAGE_INTERNAL_CONTEXTS
                or _COVERAGE_INTERNAL_FUNCTION_RE.match(context.split("::")[-1])):
            return
        identity = (position, site, kind)
        if identity in recorded or (kind == "literal" and position in used_literals):
            return
        recorded.add(identity)
        window = _coverage_window(path, context or site)
        if path == "src/main.cpp" and site in {"assignment:sendToEditorLabel", "assignment:recentHistoryLabel"}:
            window = "传输与导出（仅盘点）"
        row = {"path": path, "line": bisect_left(line_ends, position) + 1,
               "site": site, "kind": kind, "window": window}
        if context:
            row["context"] = context
        if kind == "literal":
            row["text"] = value
            used_literals.add(position)
        elif kind in {"ledger", "reference"}:
            row["id"] = value
        rows.append(row)

    def pieces(start: int, end: int) -> list[tuple[int, str]]:
        low, high = bisect_left(starts, start), bisect_left(starts, end)
        if low == high:
            return []
        extracted: list[tuple[int, str]] = []
        previous_end = start
        nested: list[bool] = []
        for position, stop, text in literals[low:high]:
            for mark in re.finditer(r"[()]", code[previous_end:position]):
                absolute = previous_end + mark.start()
                if mark.group() == "(":
                    prefix = code[start:absolute].rstrip()
                    groups = not prefix or prefix[-1] in "?:+=,(>"
                    nested.append(groups or bool(_COVERAGE_TRANSPARENT_RE.search(prefix)))
                elif nested:
                    nested.pop()
            before = code[max(start, position - 24):position].rstrip()
            after = code[stop:min(end, stop + 24)].lstrip()
            comparison = _COVERAGE_COMPARISON_RE.fullmatch(before[-2:]) or _COVERAGE_COMPARISON_RE.match(after)
            if all(nested) and not comparison:
                if extracted and not code[previous_end:position].strip():
                    origin, previous = extracted[-1]
                    extracted[-1] = (origin, previous + text)
                else:
                    extracted.append((position, text))
            previous_end = stop
        return extracted

    def assignment_values(name: str, anchor: int) -> list[tuple[int, int]]:
        scopes = {-1}
        scope = scope_at(anchor)
        while scope >= 0:
            scopes.add(scope)
            scope = scope_parent[scope]
        ranges = []
        for match in reversed(assignments.get(name, ())):
            position = match.start()
            if position >= anchor or code[max(0, position - 4):position].rstrip().endswith((".", "->", "::")):
                continue
            origin_scope = scope_at(position)
            scope = origin_scope
            while scope not in scopes:
                if scope not in statement_blocks or local_bindings.get((scope, name), anchor) <= position:
                    break
                scope = scope_parent[scope]
            else:
                semi = code.find(";", match.end(), anchor)
                if semi >= 0:
                    ranges.append((match.end(), semi))
                if origin_scope in scopes and position not in branch_bodies:
                    break
        return ranges

    def collect(site: str, anchor: int, start: int, end: int, *, follow_variable: bool = False) -> None:
        found = False
        variable = _COVERAGE_VARIABLE_RE.fullmatch(code[start:end]) if follow_variable else None
        ranges = [(start, end)]
        if variable:
            values = assignment_values(variable[1], anchor)
            if values:
                ranges = values
                site = f"assignment:{variable[1]}"
        for begin, stop in ranges:
            for position, literal in pieces(begin, stop):
                caption = _coverage_caption(literal, site, path)
                if caption:
                    emit(site, anchor, position, "literal", caption)
                    found = True
            if include_all:
                low = bisect_left(reference_starts, begin)
                high = bisect_left(reference_starts, stop)
                for ref in references[low:high]:
                    emit(site, anchor, ref.start(), "ledger", ref[1] or ref[2])
                    found = True
        if include_all and not found and code[start:end].strip():
            emit(site, anchor, anchor, "dynamic")

    for name, matches in assignments.items():
        if not _COVERAGE_FIELD_RE.fullmatch(name):
            continue
        for match in matches:
            end = code.find(";", match.end())
            if end >= 0:
                collect(name, match.start(), match.end(), end)
    for begin, end, name in functions:
        short = name.split("::")[-1]
        if not (_COVERAGE_DISPLAY_FN_RE.fullmatch(short)
                or (Path(path).name, short) in RETURN_HELPER_FNS):
            continue
        if name == "WorkflowEngine::getNodeTypeName" and not re.search(r"\bgetNodeTypeNameZh\s*\(", code):
            continue
        for match in _COVERAGE_RETURN_RE.finditer(code, begin, end):
            collect(f"return:{name}", match.start(), *match.span(1))
    for match in _COVERAGE_CALL_RE.finditer(code):
        name = match["name"]
        if name == "AddText" and not code[max(0, match.start() - 2):match.start()].endswith(("->", ".")):
            continue
        close = _match_paren(code, match.end() - 1)
        if close < 0:
            continue
        short = name.split("::")[-1]
        args = _coverage_arguments(code, match.end(), close)
        if name.startswith("ImGui::"):
            slots = _COVERAGE_IMGUI_SLOTS[short]
        elif short in {"Button", "Checkbox", "Radio", "Toggle", "SliderFloat", "SliderRange", "Combo"}:
            slots = {"Combo": (0, 2), "SliderFloat": (0, 4), "SliderRange": (0, 5)}.get(short, (0,))
        elif name == "AddText":
            slots = (4,) if len(args) >= 5 else (2,)
        else:
            slots = _COVERAGE_WRAPPERS[name]
        fmt = _COVERAGE_FORMAT_SLOTS.get(short) if name.startswith("ImGui::") else None
        if name == "registerField" and not path.endswith("/MetadataFields.cpp"):
            continue
        if short in {"TreeNode", "TreeNodeEx"}:
            fmt_slot = 1 if short == "TreeNode" else 2
            if len(args) > fmt_slot:
                slots = (fmt_slot,)
                fmt = fmt_slot
        if fmt is not None and fmt < len(args):
            formats = pieces(*args[fmt])
            if not formats or any(re.search(r"%[^% ]*s", literal) for _, literal in formats):
                slots += tuple(range(fmt + 1, len(args)))
        for slot in slots:
            if slot < len(args):
                collect(f"{name}[{slot}]", match.start(), *args[slot], follow_variable=True)
    for match in _COVERAGE_OBJC_RE.finditer(code):
        close = code.find("]", match.end())
        if close >= 0:
            collect(match[1], match.start(), match.end(), close)
    if include_all:
        covered_refs = {(row["line"], row["id"]) for row in rows if row["kind"] == "ledger"}
        for match in references:
            ident = match[1] or match[2]
            if (bisect_left(line_ends, match.start()) + 1, ident) not in covered_refs:
                emit("resource reference", match.start(), match.start(), "reference", ident)
    return sorted(rows, key=lambda row: (row["line"], row["site"], row.get("text", row.get("id", ""))))


def scan_localization_coverage(root: Path = ROOT, *, include_all: bool = False) -> list[dict[str, Any]]:
    rows = []
    for path in sorted((root / "src").rglob("*")):
        if path.suffix.lower() not in NATIVE_SRC_SUFFIXES or not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if (SKIP_DIR_PARTS.intersection(part.lower() for part in path.relative_to(root).parts)
                or path.name in SKIP_FILE_NAMES or "generated" in path.name.lower()
                or _COVERAGE_FILE_SKIP_RE.match(path.name)):
            continue
        rows.extend(_coverage_sites(rel, path.read_text(encoding="utf-8-sig"), include_all=include_all))
    return rows


def build_coverage_inventory(root: Path = ROOT) -> dict[str, Any]:
    ledger_path = LEDGER_PATH.relative_to(ROOT)
    source = (root / ledger_path).read_text(encoding="utf-8")
    ledger = json.loads(source)
    line_ends = [match.start() for match in re.finditer("\n", source)]
    ledger_lines = {
        match[1]: bisect_left(line_ends, match.start()) + 1
        for match in re.finditer(r'"id"\s*:\s*"(\w+)"', source)
    }
    resources = [
        {"id": entry["id"], "type": entry.get("type"), "path": ledger_path.as_posix(),
         "line": ledger_lines.get(entry["id"]),
         "translations": {lang: entry.get(lang) for lang in LANGUAGES}}
        for entry in ledger["entries"]
    ]
    sites = scan_localization_coverage(root, include_all=True)
    debt = [row for row in sites if row["kind"] == "literal"]
    locations = {(row["path"], row["line"], row["text"]) for row in debt}
    legacy = []
    for item in find_hardcoded_ui(root / "src"):
        path = f"src/{item['path']}"
        line = int(item["line"])
        site = item["func"]
        text = _coverage_caption(item["text"], site, path)
        if text and (path, line, text) not in locations and not _COVERAGE_FILE_SKIP_RE.match(Path(path).name):
            locations.add((path, line, text))
            legacy.append({"path": path, "line": line, "site": site, "text": text,
                           "window": _coverage_window(path, site)})
    for row in sites:
        if row.get("id") in ledger_lines:
            row["resource_line"] = ledger_lines[row["id"]]
    return {
        "schema_version": 1, "languages": LANGUAGES, "resources": resources,
        "visible": [row for row in sites if row["kind"] not in {"literal", "reference"}],
        "resource_references": [row for row in sites if row["kind"] == "reference"],
        "debt": debt,
        "legacy_only_copy": legacy,
        "ledger_purity_findings": find_ledger_mixes(ledger),
    }


def _coverage_signature(row: dict[str, Any]) -> tuple[str, str, str]:
    return row["path"], row["site"], row["text"]


def _coverage_baseline_issues(root: Path, *, allow_stock: bool) -> list[str]:
    stock: Counter[tuple[str, str, str]] = Counter()
    path = root / COVERAGE_BASELINE
    if allow_stock and path.is_file():
        payload = json.loads(path.read_text(encoding="utf-8"))
        if payload.get("schema_version") != 1 or not isinstance(payload.get("entries"), list):
            raise PurityError("Invalid native localization coverage baseline schema")
        for entry in payload["entries"]:
            if not isinstance(entry, list) or len(entry) != 4 or not all(
                isinstance(value, str) and value for value in entry[:3]
            ):
                raise PurityError(f"Invalid native coverage baseline entry: {entry!r}")
            file_name, site, text, count = entry
            key = (file_name, site, text)
            if key in stock or type(count) is not int or count < 1:
                raise PurityError(f"Invalid or duplicate native coverage baseline entry: {key!r}")
            stock[key] = count
    seen: Counter[tuple[str, str, str]] = Counter()
    errors = []
    for row in scan_localization_coverage(root):
        if row["kind"] != "literal":
            continue
        key = _coverage_signature(row)
        seen[key] += 1
        if seen[key] > stock[key]:
            errors.append(f"{row['path']}:{row['line']} {row['site']}: {row['text']!r}")
    for (file_name, site, text), count in sorted((stock - seen).items()):
        errors.append(f"{file_name} {site}: remove {count} stale baseline occurrence(s) of {text!r}")
    return errors


def check_visible_copy_purity(root: Path = ROOT, baseline: set[str] | None = None) -> None:
    baseline_path = root / "locale" / "aris.ui.strings.purity-baseline.json"
    if baseline is None:
        if baseline_path.is_file():
            payload = json.loads(baseline_path.read_text(encoding="utf-8"))
            check_baseline_growth(payload)
        allowed = load_baseline(baseline_path)
    else:
        allowed = baseline
    new_violations = find_new_violations(all_current_violations(root), allowed)
    lines = [describe_violation(item) for item in new_violations]
    lines.extend(_coverage_baseline_issues(root, allow_stock=baseline is None))
    if lines:
        raise PurityError(
            f"{len(lines)} UI language-purity/coverage violation(s):\n"
            + "\n".join(lines)
        )

# ---------------------------------------------------------------------------
# T1 (RQ-20260911-04): private translation-table detection, order-sensitive
# format-specifier check, tech-keep set is shared with the final filter above.
# These channels are SEPARATE from find_hardcoded_ui / the live baseline so the
# no-regression ledger stays untouched (9 live / 10 baseline pinned by tests).

PRIV_REGISTRY_PATH = ROOT / "tools" / "priv-table-registry.json"
_PRIV_PAIR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"\s*,\s*(?:u8)?"((?:[^"\\]|\\.)*)"')
_PRIV_MULTILINE_BRACE_PAIR_RE = re.compile(
    r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*(?:u8)?"((?:[^"\\]|\\.)*)"',
)
_CJK_RANGE = "　-〿぀-ヿ㐀-鿿豈-﫿＀-￯"


def fmt_order_match(zh: str, en: str, ja: str) -> bool:
    """Conversion specifiers must appear in the SAME ORDER in all three
    languages (MSVC has no positional %1$s): Greptile PR#175 P2 lesson."""
    specs = FORMAT_RE
    z = specs.findall(zh)
    return z == specs.findall(en) == specs.findall(ja)


def load_priv_registry(path: Path = PRIV_REGISTRY_PATH) -> set[tuple[str, str, str]]:
    if not path.is_file():
        return set()
    payload = json.loads(path.read_text(encoding="utf-8"))
    return {(e["path"], e["pair"][0], e["pair"][1]) for e in payload.get("entries", [])}


def find_private_tables(src_root: Path = SRC_ROOT,
                        registry: set[tuple[str, str, str]] | None = None,
                        ) -> list[dict[str, str]]:
    """Adjacent {ascii, cjk} string-literal pairs = private translation tables.
    Returns every hit with a 'registered' flag; new (unregistered) pairs are
    debt for the gates / inventory handover — never silently exempted, and a
    registered FILE does not exempt a NEW pair inside it (pair-granularity)."""
    reg = load_priv_registry() if registry is None else registry
    cjk = re.compile(f"[{_CJK_RANGE}]")
    out: list[dict[str, str]] = []
    files = sorted(src_root.rglob("*.cpp")) + sorted(src_root.rglob("*.h"))
    for p in files:
        rel = p.as_posix()
        try:
            text = p.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            text = p.read_text(encoding="utf-8", errors="replace")
        masked = mask_comments(text)
        # Registry keys are repo-relative src/... — never rfind("src/") on an
        # absolute checkout path (parent dirs named src/ would steal the prefix).
        try:
            path_rel = (Path("src") / p.relative_to(src_root)).as_posix()
        except ValueError:
            path_rel = Path(rel).name
        seen: set[tuple[int, str, str]] = set()
        for i, ln in enumerate(masked.splitlines(), 1):
            for m in _PRIV_PAIR_RE.finditer(ln):
                a, b = m.group(1), m.group(2)
                if cjk.search(a) or not cjk.search(b) or not any(ch.isalpha() for ch in a):
                    continue
                key = (i, a, b)
                if key in seen:
                    continue
                seen.add(key)
                out.append({
                    "path": path_rel, "line": str(i), "ascii": a, "cjk": b,
                    "registered": (path_rel, a, b) in reg,
                })
        for m in _PRIV_MULTILINE_BRACE_PAIR_RE.finditer(masked):
            a, b = m.group(1), m.group(2)
            if cjk.search(a) or not cjk.search(b) or not any(ch.isalpha() for ch in a):
                continue
            line = masked.count("\n", 0, m.start()) + 1
            key = (line, a, b)
            if key in seen:
                continue
            seen.add(key)
            out.append({
                "path": path_rel, "line": str(line), "ascii": a, "cjk": b,
                "registered": (path_rel, a, b) in reg,
            })
    return out


def coverage_summary(report: dict[str, Any]) -> str:
    groups: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in report["debt"] + report["legacy_only_copy"]:
        groups[row["window"]].append(row)
    blank = [f"{row['id']}:{lang}" for row in report["resources"] for lang, value
             in row["translations"].items() if not value]
    lines = [
        "# Harken Search 原生窗口三语盘点",
        "",
        f"- 资源键：{len(report['resources'])}；空值：{', '.join(blank) or '无'}",
        f"- 已定位的可见参数：资源键 {sum(row['kind'] == 'ledger' for row in report['visible'])}，"
        f"动态值 {sum(row['kind'] == 'dynamic' for row in report['visible'])}（不能据此判断是否已翻译）",
        f"- 未直接关联到可见参数的资源引用：{len(report['resource_references'])}",
        f"- 硬编码可见文案：{len(report['debt'])} 项；旧纯度规则额外定位到 {len(report['legacy_only_copy'])} 项。",
        f"- 资源表语言混用：{len(report['ledger_purity_findings'])} 项，沿用原纯度基线。",
        "- 传输和导出仅盘点，不在本卡修复；不盘点 Wwise、内部诊断、实验工具及旧 WebUI。",
        "- 这里是静态扫描：运行时生成的文案、外部数据和未登记的渲染入口不等于三语已覆盖。",
        "",
    ]
    for window, rows in sorted(groups.items(), key=lambda item: (-len(item[1]), item[0])):
        lines.extend((f"## {window}（{len(rows)}）", ""))
        for row in sorted(rows, key=lambda item: (item["path"], item["line"], item["site"])):
            copy = json.dumps(row["text"], ensure_ascii=False).replace("<", "&lt;").replace(">", "&gt;")
            lines.append(f"- {row['path']}:{row['line']} · {row['site']} · {copy}")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check or inventory native UI localization coverage")
    parser.add_argument("action", choices=("check", "inventory"), nargs="?", default="check")
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--format", choices=("json", "markdown"), default="json")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.action == "check":
        try:
            check_visible_copy_purity(args.root)
        except PurityError as error:
            print(error, file=sys.stderr)
            return 1
        print("UI language purity and native localization coverage passed.")
    else:
        report = build_coverage_inventory(args.root)
        output = (
            coverage_summary(report) if args.format == "markdown"
            else json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        )
        if args.output:
            args.output.write_text(output, encoding="utf-8", newline="\n")
        else:
            sys.stdout.write(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
