from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
LEDGER = (ROOT / "locale" / "harken.ui.strings.json").read_text(encoding="utf-8")
HELPER_PATH = ROOT / "src" / "OutputDeviceComboItem.h"


def _device_combo_body() -> str:
    start = MAIN.index('BeginTransientScrollbarCombo("##Device"')
    end = MAIN.index("ImGui::EndCombo();", start)
    return MAIN[start:end]


def test_output_device_combo_helper_exists() -> None:
    assert HELPER_PATH.is_file(), "device combo label/id helper must be extracted for focused tests"


def test_same_named_devices_use_distinct_selectable_ids() -> None:
    helper = HELPER_PATH.read_text(encoding="utf-8")
    assert "MakeOutputDeviceComboItem" in helper
    assert "selectableId" in helper
    combo = _device_combo_body()
    assert "MakeOutputDeviceComboItem" in combo
    assert "devices[i].name.c_str()" not in combo
    assert "PushID" in combo or "selectableId" in combo


def test_default_device_label_lives_in_stable_string() -> None:
    combo = _device_combo_body()
    leftover = combo.replace("selectableId.c_str()", "").replace("displayLabel.c_str()", "")
    assert ".c_str()" not in leftover
    assert "(std::string(" not in combo
    assert '+" (默认)"' not in combo
    assert '+ " (默认)"' not in combo
    helper = HELPER_PATH.read_text(encoding="utf-8")
    assert "displayLabel" in helper
    assert "isDefault" in helper


def test_default_suffix_comes_from_trilingual_ledger() -> None:
    combo = _device_combo_body()
    assert "L(Set_OutputDeviceDefaultSuffix)" in combo
    assert '" (默认)"' not in combo
    for required in (
        '"id": "Set_OutputDeviceDefaultSuffix"',
        '"zh": " (默认)"',
        '"en": " (Default)"',
        '"ja": " (既定)"',
    ):
        assert required in LEDGER


def test_avatar_picker_buttons_keep_copy_but_use_distinct_ids() -> None:
    assert MAIN.count("L(Set_AvatarPickerOpenLabelZh)") == 2
    assert "##settings-avatar-picker-open" in MAIN
    assert "##settings-avatar-picker-open-small" in MAIN
    first = MAIN.index("##settings-avatar-picker-open")
    second = MAIN.index("##settings-avatar-picker-open-small")
    first_stmt = MAIN[max(0, first - 220) : MAIN.find(";", MAIN.find("aura::ui::ActionButton(", first)) + 1]
    second_stmt = MAIN[max(0, second - 220) : MAIN.find(";", MAIN.find("aura::ui::SmallActionButton(", second)) + 1]
    assert "L(Set_AvatarPickerOpenLabelZh)" in first_stmt
    assert "L(Set_AvatarPickerOpenLabelZh)" in second_stmt
    assert "aura::ui::ActionButton(" in first_stmt
    assert "aura::ui::SmallActionButton(" in second_stmt
    assert "(std::string(L(Set_AvatarPickerOpenLabelZh)) + \"##settings-avatar-picker-open\").c_str()" not in first_stmt
    assert "(std::string(L(Set_AvatarPickerOpenLabelZh)) + \"##settings-avatar-picker-open-small\").c_str()" not in second_stmt
