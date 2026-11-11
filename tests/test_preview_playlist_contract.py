# -*- coding: utf-8 -*-
from pathlib import Path
import sys
import argparse

ROOT = Path(__file__).resolve().parents[1]
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    BUILD = parser.parse_args().build_dir.resolve()
    failures = []

    def read(rel):
        return (ROOT / rel).read_text(encoding="utf-8")

    def expect(cond, msg):
        print(("PASS: " if cond else "FAIL: ") + msg)
        if not cond:
            failures.append(msg)

    main = read("src/main.cpp")
    worker = read("src/AsyncWorker.cpp")
    db_h = read("src/Database.h")
    db_cpp = read("src/Database.cpp")
    refresh_h = read("src/AsyncSearchRefreshFacetsWorkflow.h")
    refresh_cpp = read("src/AsyncSearchRefreshFacetsWorkflow.cpp")
    refresh_state = read("src/AsyncSearchRefreshFacetsStateAdapter.cpp")
    facet_h = read("src/AsyncSearchFacetResultWorkflow.h")
    facet_cpp = read("src/AsyncSearchFacetResultWorkflow.cpp")
    folder_tree = read("src/PlaylistFolderTree.h")

    expect("LoadCompletePlaylistMembers" in main,
           "main loads complete unfiltered playlist members for the sidebar tree")
    expect(main.count("BuildPlaylistFolderTreeFromCompleteMembers") >= 3,
           "three playlist tree entry points build from complete members")
    expect(main.count("BuildPlaylistFolderTreeFromRecords(state.searchResults)") == 0,
           "playlist tree no longer uses the loaded searchResults page")

    need_idx = main.find("const bool needFacets")
    need_slice = main[need_idx:need_idx + 280]
    expect("ViewMode::Database" in need_slice and "ViewMode::Playlist" in need_slice,
           "needFacets includes Playlist as well as Database")

    title_idx = main.find("ResolvePlaylistResultTitleCount")
    title_slice = main[max(0, title_idx - 220):title_idx + 280]
    expect(title_idx >= 0,
           "playlist title uses complete filtered match count")
    expect("L(Main_LldFmt)" in title_slice,
           "playlist title uses existing Main_LldFmt")
    expect('ImGui::Text("%d", (int)state.searchResults.size());' not in title_slice,
           "playlist title does not use loaded page size")

    expect("searchPlaylistCoverKeys" in db_h and "searchAggregatedPlaylistCoverKeys" in db_h,
           "Database declares complete filtered playlist cover-key queries")
    expect("Database::searchPlaylistCoverKeys" in db_cpp and
           "Database::searchAggregatedPlaylistCoverKeys" in db_cpp,
           "Database implements complete filtered playlist cover-key queries")
    expect("searchPlaylistCoverKeys" in worker and "searchAggregatedPlaylistCoverKeys" in worker,
           "SearchWorker Playlist branch queries complete filtered cover keys")
    expect("BuildPlaylistCoverGroupsFromCompleteMembers" in worker,
           "SearchWorker builds cover groups from complete members")

    pl_idx = worker.find("ViewMode::Playlist")
    pl_slice = worker[pl_idx:pl_idx + 2200]
    expect(pl_slice.count("isRequestStale") >= 2,
           "playlist cover query checks isRequestStale before and after")

    expect("bool playlistView = false;" in refresh_h,
           "refresh workflow adds default-false playlistView")
    expect("bool playlistView = false;" in facet_h,
           "facet-result workflow adds default-false playlistView")
    expect("input.playlistView && input.hasCoverFacetPayload" in refresh_cpp,
           "refresh workflow keeps playlist cover groups instead of clearing non-database")
    expect("persistCoverFacetCache" not in refresh_cpp,
           "refresh workflow itself does not persist covers")
    expect("input.databaseView || input.playlistView" in facet_cpp,
           "facet-result workflow can replace playlist facets")
    expect("viewMode == ViewMode::Playlist" in refresh_state,
           "refresh facets adapter snapshots playlistView from viewMode")
    expect("pendingFacetRequestId = 0" in refresh_state, "state adapter can clear pendingFacetRequestId")
    expect("AsyncSearchKind::Refresh" in refresh_state,
           "pendingFacet clear is gated on Refresh kind")
    expect("pendingFacetRequestId == result.requestId" in refresh_state,
           "pendingFacet clear requires matching requestId")
    expect("BuildPlaylistFolderTreeFromCompleteMembers" in folder_tree,
           "folder tree helper builds from complete members")
    expect("BeginDockablePanel" in main and "wasDragging" in main,
           "745 BeginDockablePanel/wasDragging remain")
    expect("DockController" not in main and "DockingEnable" not in main and "ViewportsEnable" not in main,
           "no new Dock surface in main")
    expect("persistCoverFacetCache" not in worker,
           "SearchWorker does not persist playlist cover caches")


    cmake = read("cmake/harken_search_behavior_tests.cmake")
    start = cmake.find("add_executable(TestPlaylistFolderTree EXCLUDE_FROM_ALL")
    end = cmake.find(")", start)
    block = cmake[start:end + 1] if start >= 0 and end > start else ""
    expect(block != "", "TestPlaylistFolderTree cmake target block exists")
    expect("src/Utils.cpp" not in block, "tree test cmake does not list Utils.cpp")
    expect("harken_user_data_layout.cpp" not in block, "tree test cmake does not list user-data layout")
    expect("harken_legacy_data_migrator.cpp" not in block, "tree test cmake does not list legacy migrator")
    expect("tests/playlist_folder_tree_path_conversion_stub.cpp" in block, "tree test cmake lists the conversion stub")
    expect("src/PathUtils.cpp" in block, "tree test cmake still lists PathUtils.cpp")

    stub = read("tests/playlist_folder_tree_path_conversion_stub.cpp")
    expect("Utf8ToPath" in stub and "PathToUtf8" in stub, "conversion stub defines Utf8ToPath and PathToUtf8")
    for banned, label in [
        ("BuildPlaylistFolderTree", "playlist tree algorithm"),
        ("sqlite", "database"),
        ("RegOpen", "registry"),
        ("CredWrite", "credential"),
        ("Migrate", "migrator"),
        ("CreateFile", "file API"),
        ("std::ifstream", "file stream"),
        ("std::ofstream", "file stream write"),
    ]:
        expect(banned not in stub, "conversion stub does not call " + label)

    tree_vcx = BUILD / "TestPlaylistFolderTree.vcxproj"
    expect(tree_vcx.exists(), "r2 TestPlaylistFolderTree.vcxproj exists after configure")
    if tree_vcx.exists():
        tree_proj = tree_vcx.read_text(encoding="utf-8")
        import re
        tree_names = [Path(item).name for item in re.findall(r'<ClCompile Include="([^"]+)">', tree_proj) + re.findall(r'<ClCompile Include="([^"]+)" />', tree_proj) + re.findall(r'<ClCompile Include="([^"]+)"', tree_proj)]
        tree_names = list(dict.fromkeys(tree_names))
        expect("Utils.cpp" not in tree_names, "generated tree vcxproj does not include Utils.cpp")
        expect("harken_user_data_layout.cpp" not in tree_names, "generated tree vcxproj does not include layout.cpp")
        expect("harken_legacy_data_migrator.cpp" not in tree_names, "generated tree vcxproj does not include migrator.cpp")
        expect("playlist_folder_tree_path_conversion_stub.cpp" in tree_names, "generated tree vcxproj includes conversion stub")
        expect("PathUtils.cpp" in tree_names, "generated tree vcxproj includes PathUtils.cpp")

    db_vcx = BUILD / "TestPreviewPlaylistDatabase.vcxproj"
    expect(db_vcx.exists(), "r2 TestPreviewPlaylistDatabase.vcxproj exists after configure")
    if db_vcx.exists():
        db_proj = db_vcx.read_text(encoding="utf-8")
        expect("ARIS_PREVIEW_ISOLATION=1" in db_proj, "playlist database test keeps ARIS_PREVIEW_ISOLATION=1")
        expect("preview_playlist_isolation_root.h" in db_proj, "playlist database test keeps /FI isolation header")
        expect("BUG-20261029-PLAYLIST-COVER-WALL/test-databases/playlist-scope" in read("tests/preview_playlist_isolation_root.h"),
               "playlist database FI header points at this card's isolated test root")

    print("FAILURES", len(failures))
    for item in failures:
        print(" -", item)
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
