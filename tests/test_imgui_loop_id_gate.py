import subprocess
import sys
from pathlib import Path

from tools.imgui_loop_id_gate import (
    CONTRACT_SITES,
    extract_region,
    find_naked_loop_labels,
    scan_contract_sites,
)


ROOT = Path(__file__).resolve().parents[1]


def test_scanner_flags_variable_label_without_pushid() -> None:
    src = """
    void Draw(const char* const items[], int n) {
        for (int i = 0; i < n; ++i) {
            ImGui::Selectable(items[i], false);
        }
    }
    """
    findings = find_naked_loop_labels(src)
    assert findings
    assert findings[0].widget == "Selectable"
    assert "items[i]" in findings[0].arg


def test_scanner_accepts_pushid_and_hash_suffix() -> None:
    src = """
    void Draw(const std::string& name, int n) {
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            ImGui::Selectable(name.c_str(), false);
            ImGui::PopID();
        }
        for (int i = 0; i < n; ++i) {
            ImGui::Selectable((name + "##row" + std::to_string(i)).c_str(), false);
        }
    }
    """
    assert find_naked_loop_labels(src) == []


def test_scanner_does_not_flag_fixed_enum_labels() -> None:
    src = """
    void Draw(const std::vector<std::pair<std::string, std::string>>& fileTypes, int selected) {
        for (int i = 0; i < (int)fileTypes.size(); i++) {
            ImGui::Selectable(fileTypes[i].first.c_str(), selected == i);
        }
        for (int i = 0; i < 3; ++i) {
            ImGui::Selectable(L(Filter_All), false);
        }
        for (int i = 0; i < 2; ++i) {
            ImGui::Selectable("WAV", false);
        }
    }
    """
    assert find_naked_loop_labels(src) == []


def test_scanner_flags_real_combo_loop_without_pushid() -> None:
    src = """
    bool Combo(const char* label, int* currentIndex, const char* const items[], int itemCount) {
        if (ImGui::BeginCombo(label ? label : "##combo", items[0])) {
            for (int i = 0; i < itemCount; ++i) {
                const bool selected = (i == *currentIndex);
                if (ImGui::Selectable(items[i], selected)) {
                    *currentIndex = i;
                }
            }
            ImGui::EndCombo();
        }
        return false;
    }
    """
    findings = find_naked_loop_labels(src)
    assert findings
    assert findings[0].widget == "Selectable"
    assert "items[i]" in findings[0].arg


def test_required_sites_use_explicit_ids() -> None:
    leftovers = []
    for rel, start, end, needle in CONTRACT_SITES:
        region = extract_region((ROOT / rel).read_text(encoding="utf-8"), start, end)
        assert needle in region, f"{rel} region lost {needle}"
        findings = find_naked_loop_labels("void Site() {\n" + region + "\n}\n")
        if findings:
            leftovers.append(f"{rel}: {needle} @ {findings[0].line} {findings[0].snippet}")
        elif "PushID" not in region and "##" not in region:
            leftovers.append(f"{rel}: {needle} has no PushID/##")
    assert leftovers == [], "naked loop labels:\n" + "\n".join(leftovers)
    assert scan_contract_sites(ROOT) == []


def test_settings_search_has_stable_entry_ids():
    main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    search = (ROOT / "src/SettingsDialog.inl").read_text(encoding="utf-8")
    assert "matchedSectionTitles" not in main
    assert "matchedSettingsEntries" not in main
    assert 'ImGui::PushID(entry->id.c_str())' in search
    assert 'editor.focusId=entry->id' in search


def test_real_fixed_enum_loops_are_not_flagged() -> None:
    main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    transfer = main[
        main.index("static auto fileTypes = TransferEngine::getFileTypeChoices();") :
        main.index("if (ImGui::Checkbox(L(Export_EmbedMeta)")
    ]
    findings = find_naked_loop_labels("void Site() {\n" + transfer + "\n}\n")
    assert findings == []

    module = (ROOT / "src" / "ModuleUI.cpp").read_text(encoding="utf-8")
    folder = module[
        module.index('if (ImGui::BeginCombo("##FolderStrategy"') :
        module.index("ImGui::Separator();", module.index('if (ImGui::BeginCombo("##FolderStrategy"'))
    ]
    findings = find_naked_loop_labels("void Site() {\n" + folder + "\n}\n")
    assert findings == []


def test_gate_script_exists_for_cmake() -> None:
    assert (ROOT / "tools" / "imgui_loop_id_gate.py").is_file()
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "TestImGuiLoopIdGate" in cmake
    assert "tests/test_imgui_loop_id_gate.py" in cmake


def test_gate_cli_exits_zero_on_contract_sites() -> None:
    completed = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "imgui_loop_id_gate.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr
