import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/PlaylistDisplayedFileCountPolicy.h"
SOURCE = ROOT / "src/PlaylistDisplayedFileCountPolicy.cpp"
MAIN = ROOT / "src/main.cpp"
QUICK_BAR = ROOT / "src/PlaylistQuickBar.cpp"
INDEX = ROOT / "src/PlaylistCollectionIndex.h"


def test_policy_owns_recursive_playlist_display_count_semantics() -> None:
    assert HEADER.is_file(), f"missing header: {HEADER}"
    assert SOURCE.is_file(), f"missing source: {SOURCE}"

    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    assert "ResolvePlaylistDisplayedFileCount" in header
    assert "ResolvePlaylistDisplayedFileCount" in source
    for semantic in (
        "candidate.id == playlistId",
        "!playlist->is_folder",
        "child.parent_id != playlistId",
        "childCount < 0",
    ):
        assert semantic in source
    for forbidden in ("main.cpp", "WindowsMain", "platform/"):
        assert forbidden not in header
        assert forbidden not in source


def test_product_root_consumes_shared_index_without_owning_recursive_helper() -> None:
    main = MAIN.read_text(encoding="utf-8")
    quick_bar = QUICK_BAR.read_text(encoding="utf-8")
    index = INDEX.read_text(encoding="utf-8")
    assert '#include "PlaylistCollectionIndex.h"' in main
    assert "static int64_t GetPlaylistDisplayedFileCount" not in main
    assert "static int64_t GetPlaylistDisplayedFileCount" not in quick_bar
    # Both playlist surfaces read the cached count from the shared index.
    assert main.count("state.playlistCollectionIndex.DisplayedCount(") == 1
    assert quick_bar.count("state.playlistCollectionIndex.DisplayedCount(") == 1
    assert main.count("EnsurePlaylistCollectionIndex(") == 1
    assert quick_bar.count("EnsurePlaylistCollectionIndex(") == 1
    # The recursive aggregation and its pending (-1) propagation live in the index only.
    for semantic in ("childrenByParent", "displayedCountById", "childCount < 0"):
        assert semantic in index
    assert "ResolvePlaylistDisplayedFileCount" not in index

def test_policy_is_built_tested_and_governed() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    behavior = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(
        encoding="utf-8"
    )
    rules = json.loads(
        (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(
            encoding="utf-8"
        )
    )
    stage7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(
        encoding="utf-8"
    )
    assert "src/PlaylistDisplayedFileCountPolicy.cpp" in cmake
    assert "TestPlaylistDisplayedFileCountPolicy" in behavior
    assert "TestPlaylistDisplayedFileCountPolicyBoundary" in behavior
    for path in (
        "src/PlaylistDisplayedFileCountPolicy.cpp",
        "src/PlaylistDisplayedFileCountPolicy.h",
    ):
        assert path in rules["scope"]
        # This count is presentation state consumed by the sidebar and quick bar.
        assert path in rules["modules"]["ui_orchestration"]["files"]
    assert (
        '"src/PlaylistDisplayedFileCountPolicy.h": '
        '"tests/test_playlist_displayed_file_count_policy_boundary.py"'
    ) in stage7
