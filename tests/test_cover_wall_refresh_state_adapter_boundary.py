from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
FACTORY = (ROOT / "src" / "AsyncSearchResultEffectFactory.cpp").read_text(encoding="utf-8")
HEADER_PATH = ROOT / "src" / "CoverWallRefreshStateAdapter.h"
SOURCE_PATH = ROOT / "src" / "CoverWallRefreshStateAdapter.cpp"
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
    encoding="utf-8"
)
RULES = (
    ROOT / "docs" / "architecture" / "stage-4-dependency-rules.json"
).read_text(encoding="utf-8")
STAGE7 = (ROOT / "tools" / "stage7_api_contract_audit.py").read_text(
    encoding="utf-8"
)


def test_cover_wall_refresh_state_adapter_owns_refresh_semantics():
    assert HEADER_PATH.exists()
    assert SOURCE_PATH.exists()

    header = HEADER_PATH.read_text(encoding="utf-8")
    source = SOURCE_PATH.read_text(encoding="utf-8")

    assert "void RequestDisplayRefresh(AppState& state);" in header
    assert "void RequestDisplayRefreshForExplicitRetry(AppState& state);" in header
    assert "void RequestDatabaseRefreshForExplicitRetry(AppState& state);" in header
    assert "void RequestDatabaseInvalidation(AppState& state);" in header
    assert "void ForceCoverWallReload(AppState& state);" in header

    assert "state.coverWallDisplayRefreshRequested = true;" in source
    assert "state.needsCoverWallReload = true;" in source
    assert "state.coverWall.preRequestCursor = 0;" in source
    assert "state.coverWall.lastPreRequestTotalItems = -1;" in source
    assert "state.coverWall.lastFirstScreenStart =" in source
    assert "state.coverWall.lastFirstScreenEnd = 0;" in source
    assert "state.invalidateCoverWallAfterDatabaseContentChange();" in source
    assert "state.coverWall.failedTexCache.clear();" in source
    assert "state.coverWall.failCountPeriod = 0;" in source
    assert "CoverModule::ResetFailedCache();" in source
    assert "ExplicitRetryRefreshRequested" in source
    assert "ExplicitDatabaseRetryRefreshRequested" in source

    for forbidden in ("main.cpp", "WindowsMain", "platform/"):
        assert forbidden not in header
        assert forbidden not in source


def test_root_delegates_cover_wall_refresh_without_owning_state_machine():
    assert '#include "CoverWallRefreshStateAdapter.h"' in MAIN
    assert "static void RequestCoverWallDisplayRefresh(" not in MAIN
    assert "static void RequestCoverWallDisplayRefreshForExplicitRetry(" not in MAIN
    assert "static void RequestCoverWallDatabaseInvalidation(" not in MAIN
    assert "\nvoid ForceCoverWallReload(AppState& state) {" not in MAIN

    assert "aura::cover_wall::RequestDisplayRefresh," in FACTORY
    assert "aura::cover_wall::RequestDisplayRefresh(state);" in MAIN
    assert (
        "aura::cover_wall::RequestDatabaseRefreshForExplicitRetry(state);" in MAIN
    )
    assert "aura::cover_wall::RequestDatabaseInvalidation(state);" in MAIN


def test_cover_wall_refresh_state_adapter_is_built_tested_and_governed():
    assert CMAKE.count("src/CoverWallRefreshStateAdapter.cpp") == 1
    assert BEHAVIOR.count(
        "tests/test_cover_wall_refresh_state_adapter_boundary.py"
    ) == 1
    assert RULES.count("src/CoverWallRefreshStateAdapter.cpp") >= 2
    assert RULES.count("src/CoverWallRefreshStateAdapter.h") >= 2
    assert (
        '"src/CoverWallRefreshStateAdapter.h": '
        '"tests/test_cover_wall_refresh_state_adapter_boundary.py"'
        in STAGE7
    )


def test_menu_refresh_uses_the_same_explicit_cover_retry_as_f5_and_toolbar():
    menu_start = MAIN.index("// Refresh\n", MAIN.index("ImGui::BeginMenu(L(Menu_Edit))"))
    menu_end = MAIN.index("ImGui::EndMenu();", menu_start)
    menu_refresh = MAIN[menu_start:menu_end]

    assert "ClearAllFiltersForShowAll(state);" not in menu_refresh
    assert "ClearSpecificCoverKeyFilter(state);" not in menu_refresh
    assert "aura::cover_wall::RequestDatabaseRefreshForExplicitRetry(state);" in menu_refresh
    assert "state.suppressFacetsForNextRefresh = true;" in menu_refresh
    assert menu_refresh.index(
        "aura::cover_wall::RequestDatabaseRefreshForExplicitRetry(state);"
    ) < menu_refresh.index("RefreshSearchResults(state, true);")


def test_database_refresh_invalidates_the_cover_wall_before_reloading_all_entries():
    keyboard_start = MAIN.index("if (IsShortcutPressedSnapshot(ShortcutAction::Refresh, ks))")
    toolbar_start = MAIN.index("if (navClicked[4])")
    menu_start = MAIN.index("// Refresh\n", MAIN.index("ImGui::BeginMenu(L(Menu_Edit))"))
    refresh_scopes = (
        MAIN[keyboard_start : keyboard_start + 800],
        MAIN[toolbar_start : toolbar_start + 800],
        MAIN[menu_start : MAIN.index("ImGui::EndMenu();", menu_start)],
    )

    for scope in refresh_scopes:
        assert "aura::cover_wall::RequestDatabaseRefreshForExplicitRetry(state);" in scope
        assert "aura::cover_wall::RequestDisplayRefreshForExplicitRetry(state);" not in scope

    source = SOURCE_PATH.read_text(encoding="utf-8")
    retry_body_start = source.index("void RequestDisplayRefreshForExplicitRetry(AppState& state)")
    retry_body_end = source.index("void RequestDatabaseRefreshForExplicitRetry(AppState& state)", retry_body_start)
    retry_body = source[retry_body_start:retry_body_end]

    assert "RequestDisplayRefresh(state);" in retry_body
    assert "RequestDatabaseInvalidation(state);" in source
    database_retry_start = source.index("void RequestDatabaseRefreshForExplicitRetry(AppState& state)")
    database_retry_end = source.index("void RequestDatabaseInvalidation(AppState& state)", database_retry_start)
    database_retry_body = source[database_retry_start:database_retry_end]
    assert "RequestDatabaseInvalidation(state);" in database_retry_body
    assert database_retry_body.index("RequestDatabaseInvalidation(state);") < database_retry_body.index(
        "state.coverWall.failedTexCache.clear();"
    )
