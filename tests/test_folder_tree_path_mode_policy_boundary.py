from pathlib import Path
import json


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")
LOCALIZED = {
    entry["id"]: entry
    for entry in json.loads(
        (ROOT / "locale" / "harken.ui.strings.json").read_text(encoding="utf-8")
    )["entries"]
}
POLICY = (ROOT / "src" / "FolderTreePathModePolicy.h").read_text(encoding="utf-8")
DATABASE_H = (ROOT / "src" / "Database.h").read_text(encoding="utf-8")
DATABASE_CPP = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
BOOTSTRAP = (ROOT / "src" / "DeferredSidebarBootstrapWorkflow.cpp").read_text(
    encoding="utf-8"
)
MODULE_UI = (ROOT / "src" / "ModuleUI.cpp").read_text(encoding="utf-8")
PLAYLIST_TREE = (ROOT / "src" / "PlaylistFolderTree.h").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
    encoding="utf-8"
)


def test_preference_defaults_to_absolute_and_persists():
    assert "folderTreePathMode = FolderTreePathMode::Absolute" in APP_STATE or (
        "folderTreePathMode = aura::folder_tree::PathMode::Absolute" in APP_STATE
    )
    assert 'kv["folderTreePathMode"]' in MAIN
    assert 'items.push_back({"folderTreePathMode"' in MAIN
    assert "Set_FolderTreePathMode" in LOCALIZED
    assert LOCALIZED["Set_FolderTreePathAbsolute"]["zh"] == "绝对路径"
    assert LOCALIZED["Set_FolderTreePathPilot"]["zh"] == "领航路径"
    assert LOCALIZED["Set_FolderTreePathAbsolute"]["en"] == "Absolute path"
    assert LOCALIZED["Set_FolderTreePathPilot"]["en"] == "Pilot path"
    assert "L(Set_FolderTreePathMode)" in MAIN


def test_database_tree_builders_use_path_mode_policy():
    assert "BuildFolderTreeLayers(" in POLICY
    assert "aura::folder_tree::BuildFolderTreeLayers" in DATABASE_CPP
    assert "getFolderHierarchy(" in DATABASE_H
    assert "FolderTreePathMode" in DATABASE_H or "PathMode" in DATABASE_H
    assert "getFolderHierarchyFromIndex(" in DATABASE_H
    assert "getFolderHierarchyWithProgress(" in DATABASE_H
    assert "StartDeferredSidebarBootstrap" in BOOTSTRAP
    assert "getFolderHierarchyFromIndex(" in BOOTSTRAP
    assert "folderTreePathMode" in BOOTSTRAP or "PathMode" in BOOTSTRAP
    assert "getFolderHierarchy(" in MODULE_UI
    assert "folderTreePathMode" in MAIN


def test_playlist_and_browser_trees_keep_current_structure():
    assert "BuildPlaylistFolderTreeFromRecords" in PLAYLIST_TREE
    assert "aura::folder_tree::BuildFolderTreeLayers" not in PLAYLIST_TREE
    assert "RenderBrowserSystemFolderTree" in MAIN
    browser = MAIN[MAIN.index("RenderBrowserSystemFolderTree") :]
    browser = browser[:8000]
    assert "BuildFolderTreeLayers" not in browser


def test_folder_tree_path_mode_policy_is_built_tested_and_wired():
    assert BEHAVIOR.count("tests/test_folder_tree_path_mode_policy.cpp") == 1
    assert BEHAVIOR.count(
        "tests/test_folder_tree_path_mode_policy_boundary.py"
    ) == 1


def test_personal_preference_saves_and_rebuilds_database_tree():
    combo = MAIN[MAIN.index('ImGui::Combo("##FolderTreePathMode"') :]
    combo = combo[:800]
    assert "MarkSettingsDirty(state)" in combo
    assert "StartDeferredSidebarBootstrap(state, true, false)" in combo
    assert "SaveAppSettings(state)" not in combo

    assert 'items.push_back({"folderTreePathMode"' in MAIN
    assert 'kv["folderTreePathMode"]' in MAIN
    load = MAIN[MAIN.index('kv["folderTreePathMode"]') :]
    load = load[:500]
    assert "rawMode == 1" in load
    assert "PathMode::Pilot" in load
    assert "PathMode::Absolute" in load
    assert (
        "folderTreePathMode = aura::folder_tree::PathMode::Absolute" in APP_STATE
    )

    assert "void FlushSettingsDirty(AppState& state)" in MAIN
    assert "FlushSettingsDirty(state);" in WINDOWS_MAIN
    exit_fn = WINDOWS_MAIN[WINDOWS_MAIN.index("void RequestHarkenSearchExit(") :]
    exit_fn = exit_fn.split("\n}", 1)[0]
    assert "TrySaveAppSettings(*g_appState)" in exit_fn
    assert "StartDeferredSidebarBootstrap(state, true, true)" in WINDOWS_MAIN
    assert "StartDeferredSidebarBootstrap(state, true, true)" in MAIN
