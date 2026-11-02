from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIDGETS_H = (ROOT / "src" / "HarkenSearchWidgets.h").read_text(encoding="utf-8")
WIDGETS_CPP = (ROOT / "src" / "HarkenSearchWidgets.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def _between(text: str, start: str, end: str) -> str:
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]




def test_settings_row_api_exists() -> None:
    assert "struct SettingsRowOptions" in WIDGETS_H
    assert "bool SettingsToggleRow(" in WIDGETS_H
    assert "bool SettingsSliderRow(" in WIDGETS_H
    assert "bool SettingsButtonRow(" in WIDGETS_H
    assert "bool BeginSettingsGroup(" in WIDGETS_H



def test_settings_row_uses_main_tooltip() -> None:
    assert "void Tooltip(const char* text);" in WIDGETS_H
    body = _between(WIDGETS_CPP, "bool SettingsToggleRow(", "bool SettingsButtonRow(")
    assert "aura::ui::Tooltip" in WIDGETS_CPP or "Tooltip(" in body
    assert "ImGui::SetTooltip" not in body
    assert "HelpMarker" not in WIDGETS_H



def test_settings_group_uses_gap12() -> None:
    assert "kGap12" in WIDGETS_CPP
    assert "BeginSettingsGroup(" in WIDGETS_H
    assert "BeginSettingsGroup" in WIDGETS_CPP



def test_guided_tooltip_flag_links_to_global_symbol() -> None:
    in_aura_ui = False
    depth = 0
    namespaced_extern = False
    for raw in WIDGETS_CPP.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not in_aura_ui and line.startswith("namespace aura::ui"):
            in_aura_ui = True
            depth = line.count("{") - line.count("}")
            if depth <= 0:
                depth = 1
            continue
        if in_aura_ui:
            depth += line.count("{") - line.count("}")
            if "extern bool g_showGuidedTooltips" in line:
                namespaced_extern = True
            if depth <= 0:
                in_aura_ui = False
    assert namespaced_extern is False
    assert "::g_showGuidedTooltips" in WIDGETS_CPP




# The legacy pixel/source-shape assertions described the retired eight-page
# immediate-save implementation. Runtime equivalences now live in
# tools/qa_settings_redesign.py: real clicks, dropdowns, clipping and persistence.
SHELL = (ROOT / "src/SettingsDialog.inl").read_text(encoding="utf-8")
FIELDS = (ROOT / "src/SettingsFields.inl").read_text(encoding="utf-8")

def test_regular_preference_rows_cannot_write_live_settings():
    assert "SaveAppSettings(" not in FIELDS
    assert "MarkSettingsDirty(" not in FIELDS
    import re
    assert not re.search(r"(?<!&)&state[.]settings[.]", FIELDS)
    assert "switchOutputDevice(" not in FIELDS

def test_full_text_search_does_not_reuse_translated_label_as_id():
    assert 'ImGui::PushID(entry->id.c_str())' in SHELL
    assert 'ImGui::PushID(id.c_str())' in SHELL

def test_native_specialty_operations_are_retained():
    expected = {
        "SettingsAvatarTools.inl": ("SaveCroppedAvatarPng", "AvatarCropTitle"),
        "SettingsMaintenanceTools.inl": ("getRepairCenterSnapshot", "RenderRelinkTool"),
        "SettingsThesaurusTools.inl": ("SaveThesaurusJson", "ThesaurusRulesTable"),
        "SettingsDiagnosticTools.inl": ("exportJson", "Set_DiagClearLog"),
    }
    for file, operations in expected.items():
        code = (ROOT / "src" / file).read_text(encoding="utf-8")
        for operation in operations: assert operation in code

def test_thesaurus_rules_list_keeps_its_own_scroll_bounds():
    code = (ROOT / "src/SettingsThesaurusTools.inl").read_text(encoding="utf-8")
    rules = _between(code, "rulesListHeight", "ThesaurusRulesTable")
    assert "220.0f" in rules
    assert "ImVec2(0, 0)" not in rules

def test_qa_driver_does_not_control_the_users_mouse():
    script=(ROOT / "tools/qa_settings_redesign.py").read_text(encoding="utf-8")
    for forbidden in ("SetCursorPos(","SendInput(","mouse_event(","SwitchDesktop("):
        assert forbidden not in script
    assert "PrivateDesktop()" in script
    assert "identity['isolation_root']" in script
