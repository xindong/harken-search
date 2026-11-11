#!/usr/bin/env python3
"""Compile-time localization ledger: JSON is the only copy book."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LEDGER = ROOT / "locale" / "harken.ui.strings.json"
ALLOWED_TYPES = {"ui", "theme", "beta-hidden"}
EMPTY_ALLOWED_IDS = {"Auth_ContactAdmin"}
THEME_IDS = [
    "Set_ThemeDark",
    "Set_ThemeOcean",
    "Set_ThemeRoseCedar",
    "Set_ThemeCustom",
    "Set_ThemeLapisBlue",
    "Set_ThemeJadeMist",
]
PROTECTED_LEDGER_IDS = frozenset(
    THEME_IDS
    + [
        "Set_DevModeEnabled",
        "View_InfoPanel",
        "View_DemoWindow",
        "Panel_InfoDetach",
        "Set_UILanguage",
        "Auth_ContactAdmin",
        "Crash_DlgTitle",
        "Main_CrashTitleText",
        "Main_CrashReportNote",
        "Crash_SendReport",
        "Fb_CrashReportTitle",
        "Main_AttachDiagLog500",
        "Main_DismissBtn",
    ]
)
U8_STRING_RE = re.compile(r'u8"((?:\\.|[^"\\])*)"')


class LedgerError(ValueError):
    pass


def parse_lstr_ids(header_text: str) -> list[str]:
    start = header_text.find("enum class LStr {")
    if start < 0:
        generated = re.search(
            r"enum class LStr \{([\s\S]*?)\n\s*_Count",
            header_text,
        )
        if not generated:
            raise LedgerError("LStr enum not found")
        body = generated.group(1)
    else:
        end = header_text.find("\n    _Count", start)
        if end < 0:
            end = header_text.find("\n\t_Count", start)
        if end < 0:
            raise LedgerError("LStr _Count not found")
        body = header_text[start:end]
        body = body.split("{", 1)[1]
    ids: list[str] = []
    for raw_line in body.splitlines():
        line = raw_line.split("//", 1)[0].strip()
        if not line:
            continue
        for part in line.split(","):
            name = part.strip()
            if name:
                ids.append(name)
    return ids


def load_ledger(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def normalize_ledger(payload: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise LedgerError("ledger must be an object")
    entries = payload.get("entries")
    if not isinstance(entries, list) or not entries:
        raise LedgerError("ledger.entries must be a non-empty list")
    normalized = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise LedgerError(f"entries[{index}] must be an object")
        japanese = entry.get("ja", "")
        if japanese is None:
            japanese = ""
        item = {
            "id": str(entry.get("id", "")),
            "type": str(entry.get("type", "ui")),
            "zh": entry.get("zh"),
            "en": entry.get("en"),
            "ja": japanese,
        }
        if "zh" not in entry or "en" not in entry:
            raise LedgerError(f"{item['id'] or f'entries[{index}]'} missing zh/en")
        if item["zh"] is None or item["en"] is None:
            raise LedgerError(f"{item['id']} zh/en must be strings, not null")
        if not isinstance(item["zh"], str) or not isinstance(item["en"], str):
            raise LedgerError(f"{item['id']} zh/en must be strings")
        if not isinstance(item["ja"], str):
            raise LedgerError(f"{item['id']} ja must be a string")
        normalized.append(item)
    return {"schema_version": payload.get("schema_version", 1), "entries": normalized}


def validate_ledger(ledger: dict[str, Any]) -> None:
    if ledger.get("schema_version") != 1:
        raise LedgerError("schema_version must be 1")
    entries = ledger["entries"]
    ids = [entry["id"] for entry in entries]
    if any(not item for item in ids):
        raise LedgerError("every entry needs an id")
    if len(ids) != len(set(ids)):
        raise LedgerError("duplicate localization ids")
    for entry in entries:
        if entry["type"] not in ALLOWED_TYPES:
            raise LedgerError(f"{entry['id']} has unknown type {entry['type']}")
        for lang in ("zh", "en"):
            if entry[lang] == "" and entry["id"] not in EMPTY_ALLOWED_IDS:
                raise LedgerError(f"{entry['id']} missing {lang} translation")
    theme_ids = [entry["id"] for entry in entries if entry["type"] == "theme"]
    if theme_ids and theme_ids != THEME_IDS:
        raise LedgerError("theme entries must be the six settings theme names")
    if "Auth_ContactAdmin" in ids:
        auth = next(entry for entry in entries if entry["id"] == "Auth_ContactAdmin")
        if auth["zh"] != "" or auth["en"] != "":
            raise LedgerError("Auth_ContactAdmin must be an explicit empty string in zh and en")


def cpp_u8(text: str) -> str:
    escaped = (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f'u8"{escaped}"'


def render_generated_header(entries: list[dict[str, Any]]) -> str:
    ids = [entry["id"] for entry in entries]
    theme = [entry for entry in entries if entry["type"] == "theme"]
    lines = [
        "// Generated by tools/generate_localization.py. Do not edit.",
        "#pragma once",
        "",
        "enum class LStr {",
    ]
    for offset in range(0, len(ids), 4):
        chunk = ", ".join(ids[offset : offset + 4])
        lines.append(f"    {chunk},")
    lines.append("    _Count")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr int kArisSettingsThemeSlotCount = {len(theme)};")
    lines.append("inline constexpr const char* kArisSettingsThemeDisplayNamesZh[] = {")
    for entry in theme:
        lines.append(f"    {cpp_u8(entry['zh'])},")
    lines.append("};")
    lines.append("inline constexpr const char* kArisSettingsThemeDisplayNamesEn[] = {")
    for entry in theme:
        lines.append(f"    {cpp_u8(entry['en'])},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def render_generated_strings(entries: list[dict[str, Any]]) -> str:
    lines = [
        "// Generated by tools/generate_localization.py. Do not edit.",
        "static const char* g_strings_zh[] = {",
    ]
    for entry in entries:
        literal = cpp_u8(entry["zh"])
        if entry["type"] == "beta-hidden":
            literal = f"AURA_DEV_LOC_ZH({literal})"
        lines.append(f"    {literal},")
    lines.append("};")
    lines.append("static const char* g_strings_en[] = {")
    for entry in entries:
        literal = cpp_u8(entry["en"])
        if entry["type"] == "beta-hidden":
            literal = f"AURA_DEV_LOC_EN({literal})"
        lines.append(f"    {literal},")
    lines.append("};")
    lines.append("static const char* g_strings_ja[] = {")
    for entry in entries:
        literal = cpp_u8(entry.get("ja", ""))
        if entry["type"] == "beta-hidden":
            literal = f"AURA_DEV_LOC_EN({literal})"
        lines.append(f"    {literal},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def check_keys(ledger: dict[str, Any], expected_ids: list[str]) -> None:
    actual = [entry["id"] for entry in ledger["entries"]]
    missing = [ident for ident in expected_ids if ident not in actual]
    extra = [ident for ident in actual if ident not in expected_ids]
    if missing or extra:
        parts = []
        if missing:
            parts.append("missing keys: " + ", ".join(missing[:20]))
        if extra:
            parts.append("wrong keys: " + ", ".join(extra[:20]))
        raise LedgerError("; ".join(parts))
    if actual != expected_ids:
        raise LedgerError("ledger key order drifted from expected LStr order")


L_CALL_RE = re.compile(r"\bL\(([A-Za-z_][A-Za-z0-9_]*)\)")
LW_CALL_RE = re.compile(r"\bLw\(([A-Za-z_][A-Za-z0-9_]*)\)")
LSTR_ENUM_RE = re.compile(r"\bLStr::([A-Za-z_][A-Za-z0-9_]*)")
GENERATED_HEADER_NAMES = {
    "LocalizationGenerated.h",
    "LocalizationStrings.generated.h",
    "WebUiEmbedded.generated.h",
}
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".mm", ".inl"}


def collect_used_lstr_ids(root: Path) -> list[str]:
    found: set[str] = set()
    for base in (root / "src", root / "tests"):
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if path.name in GENERATED_HEADER_NAMES:
                continue
            text = path.read_text(encoding="utf-8")
            for ident in L_CALL_RE.findall(text):
                if ident != "id":
                    found.add(ident)
            for ident in LW_CALL_RE.findall(text):
                if ident != "id":
                    found.add(ident)
            for ident in LSTR_ENUM_RE.findall(text):
                if ident not in {"_Count", "id"}:
                    found.add(ident)
    return sorted(found)


def check_call_sites(ledger: dict[str, Any], used_ids: list[str]) -> None:
    actual = {entry["id"] for entry in ledger["entries"]}
    missing = [ident for ident in used_ids if ident not in actual]
    if missing:
        raise LedgerError("missing keys: " + ", ".join(missing[:20]))


def check_unreferenced_ids(ledger: dict[str, Any], used_ids: list[str]) -> None:
    used = set(used_ids)
    extra = [
        entry["id"]
        for entry in ledger["entries"]
        if entry["id"] not in used and entry["id"] not in PROTECTED_LEDGER_IDS
    ]
    if extra:
        raise LedgerError("unreferenced keys: " + ", ".join(extra[:20]))


def unescape_u8(raw: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(raw):
        if raw[i] != "\\" or i + 1 >= len(raw):
            out.append(raw[i])
            i += 1
            continue
        nxt = raw[i + 1]
        mapping = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}
        if nxt in mapping:
            out.append(mapping[nxt])
            i += 2
            continue
        out.append(raw[i])
        i += 1
    return "".join(out)


def parse_generated_string_table(text: str, array_name: str) -> list[str]:
    marker = f"static const char* {array_name}[] = {{"
    start = text.find(marker)
    if start < 0:
        raise LedgerError(f"{array_name} not found")
    end = text.find("};", start)
    if end < 0:
        raise LedgerError(f"{array_name} is unclosed")
    body = text[start:end]
    values = [unescape_u8(match.group(1)) for match in U8_STRING_RE.finditer(body)]
    return values


def check_generated_matches_ledger(ledger: dict[str, Any], output_dir: Path) -> None:
    header = (output_dir / "LocalizationGenerated.h").read_text(encoding="utf-8")
    strings = (output_dir / "LocalizationStrings.generated.h").read_text(encoding="utf-8")
    expected = render_generated_header(ledger["entries"])
    expected_strings = render_generated_strings(ledger["entries"])
    if header != expected or strings != expected_strings:
        raise LedgerError("generated localization tables drifted from the JSON ledger")
    ids = parse_lstr_ids(header)
    ledger_ids = [entry["id"] for entry in ledger["entries"]]
    if ids != ledger_ids:
        raise LedgerError("generated LStr ids drifted from the JSON ledger")
    zh = parse_generated_string_table(strings, "g_strings_zh")
    en = parse_generated_string_table(strings, "g_strings_en")
    ja = parse_generated_string_table(strings, "g_strings_ja")
    if (
        zh != [entry["zh"] for entry in ledger["entries"]]
        or en != [entry["en"] for entry in ledger["entries"]]
        or ja != [entry.get("ja", "") for entry in ledger["entries"]]
    ):
        raise LedgerError("generated localization tables drifted from the JSON ledger")


def write_generated(ledger: dict[str, Any], output_dir: Path) -> None:
    validate_ledger(ledger)
    if not any(entry["type"] == "theme" for entry in ledger["entries"]):
        raise LedgerError("generated product ledger needs theme entries")
    if not any(entry["type"] == "beta-hidden" for entry in ledger["entries"]):
        raise LedgerError("generated product ledger needs beta-hidden entries")
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "LocalizationGenerated.h").write_text(
        render_generated_header(ledger["entries"]),
        encoding="utf-8",
        newline="\n",
    )
    (output_dir / "LocalizationStrings.generated.h").write_text(
        render_generated_strings(ledger["entries"]),
        encoding="utf-8",
        newline="\n",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    if not args.ledger.is_file():
        raise LedgerError(f"missing ledger {args.ledger}")
    ledger = normalize_ledger(load_ledger(args.ledger))
    validate_ledger(ledger)
    if args.generate:
        if args.output_dir is None:
            raise LedgerError("--output-dir is required with --generate")
        write_generated(ledger, args.output_dir)
        check_generated_matches_ledger(ledger, args.output_dir)
    elif args.check:
        if args.output_dir is None:
            raise LedgerError("--output-dir is required with --check")
        check_generated_matches_ledger(ledger, args.output_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except LedgerError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
