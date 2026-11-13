from pathlib import Path
import json


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")
LOCALIZED = {
    entry["id"]: entry
    for entry in json.loads(
        (ROOT / "locale" / "harken.ui.strings.json").read_text(encoding="utf-8")
    )["entries"]
}
POLICY = (ROOT / "src" / "DragPauseOnStartPolicy.h").read_text(encoding="utf-8")
SETTINGS_CATALOG = (ROOT / "src" / "SettingsCatalog.h").read_text(encoding="utf-8")
SETTINGS_FIELDS = (ROOT / "src" / "SettingsFields.inl").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
    encoding="utf-8"
)


IN_SCOPE_MARKERS = (
    "void StartFileDrag(const std::vector<std::string>& files)",
    "if (ImGui::BeginDragDropSource",
    "g_playlistQuickDrag.active = true",
)


def test_preference_defaults_off_and_persists_with_existing_settings():
    assert "bool pausePlaybackOnDragStart = false;" in APP_STATE
    assert 'kv["pausePlaybackOnDragStart"]' in MAIN
    assert '{"pausePlaybackOnDragStart"' in MAIN or (
        'items.push_back({"pausePlaybackOnDragStart"' in MAIN
    )
    assert "Set_PauseOnDrag" in LOCALIZED
    assert 'u8"pauseDrag",Page::Interaction' in SETTINGS_CATALOG
    assert 'LStr::Set_PauseOnDrag' in SETTINGS_CATALOG
    assert 'if(id=="pauseDrag")ImGui::Checkbox("##value",&draft.pausePlaybackOnDragStart)' in SETTINGS_FIELDS
    assert "state.settings.pausePlaybackOnDragStart" in MAIN
    assert LOCALIZED["Set_PauseOnDrag"]["zh"] == "拖拽时自动暂停正在播放的音频"
    assert LOCALIZED["Set_PauseOnDrag"]["en"] == "Pause audio when a drag starts"


def test_in_scope_drag_starts_share_one_pause_decision():
    assert "DecidePauseOnDragStart(" in POLICY
    assert "aura::drag_pause::DecidePauseOnDragStart" in MAIN
    assert "NotifyInScopeDragStarted" in MAIN or (
        "MaybePausePlaybackOnInScopeDragStart" in MAIN
    )
    assert "StartFileDrag(" in MAIN
    assert "g_playlistQuickDrag.active = true" in MAIN
    assert MAIN.count("BeginDragDropSource") >= 6
    for marker in IN_SCOPE_MARKERS:
        assert marker in MAIN


def test_non_drag_and_resume_paths_stay_out_of_scope():
    assert "audioEngine.play(" not in POLICY
    assert "play()" not in POLICY
    start_file_drag = MAIN[MAIN.index("void StartFileDrag(") :]
    start_file_drag = start_file_drag[: start_file_drag.index("\nvoid StartRegionDrag")]
    assert "audioEngine.play(" not in start_file_drag
    assert "MaybePausePlaybackOnInScopeDragStart" in start_file_drag


def test_drag_pause_policy_is_built_tested_and_wired():
    assert CMAKE.count("src/DragPauseOnStartPolicy.h") == 0
    assert BEHAVIOR.count("tests/test_drag_pause_on_start_policy.cpp") == 1
    assert BEHAVIOR.count(
        "tests/test_drag_pause_on_start_policy_boundary.py"
    ) == 1
