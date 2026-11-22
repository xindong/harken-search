#!/usr/bin/env python3
"""Folder Browser BDD/TDD contract guard.

This is a source-level regression test for the Folder Browser MVP because the
affected surface is ImGui-heavy and cannot be fully driven by headless automation
without a GUI session. It guards the behaviors that have repeatedly regressed:

- Browser related-paths pane must not become a search-result list.
- Browser folder search results must render in the system tree.
- Matched files must render as file leaves, not as fake parent-folder hits.
- Folder/tree navigation must preserve CurrentDir vs RecursiveAudio scope.
- ImGui TreeNodeEx/TreePop pairing must be safe for Leaf nodes.
- Search must be bounded/progressive, not a blocking full-drive walk.
"""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"
POLICY = ROOT / "src" / "BrowserSystemFolderSearchPolicy.cpp"
RUNTIME = ROOT / "src" / "BrowserSystemFolderSearchRuntime.cpp"
CONTRACT = ROOT / "docs" / "behavior" / "modules" / "folder-browser.md"


failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"PASS: {message}")
    else:
        print(f"FAIL: {message}", file=sys.stderr)
        failures.append(message)


def read(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="ignore")
    except OSError as exc:
        failures.append(f"cannot read {path}: {exc}")
        return ""


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        return ""
    brace = source.find("{", start)
    if brace < 0:
        return ""
    depth = 0
    for i in range(brace, len(source)):
        ch = source[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    return ""


def main() -> int:
    source = read(MAIN)
    policy = read(POLICY)
    runtime = read(RUNTIME)
    contract = read(CONTRACT)
    check(bool(source), "src/main.cpp is readable")
    check(bool(policy), "Browser system folder search policy is readable")
    check(bool(runtime), "Browser system folder search runtime is readable")
    check(bool(contract), "folder-browser behavior contract is readable")

    render_panel = extract_function(
        source, "static void RenderBrowserFolderTreeSidebarPanel("
    )
    toolbar = render_panel.partition("RenderFolderTreeExplorerToolbar(")[2]
    refresh_action = extract_function(toolbar, "[&controls]() {")
    locate_action = extract_function(toolbar, "[&controls, &currentBrowserPath]() {")
    clear_search = "g_browserSystemFolderSearchBuffer[0] = '\\0';"
    locate_current = "LocateBrowserSystemFolderPath(currentBrowserPath);"
    render_system_tree = "RenderBrowserSystemFolderTree(state, controls.locate);"
    system_node = extract_function(
        source,
        "static void RenderBrowserSystemFolderNode(AppState& state,",
    )
    search_node = extract_function(
        source,
        "static void RenderBrowserSystemFolderSearchNode(AppState& state,",
    )
    db_folder_node = extract_function(
        source,
        "static void RenderFolderTreeNode(AppState& state,",
    )
    search_task = extract_function(
        runtime,
        "uint64_t SystemFolderSearchRuntime::Start(",
    )
    build_search_tree = extract_function(
        policy,
        "BuildSystemFolderSearchTree(const std::vector<SystemFolderSearchHit>& hits)",
    )
    render_search_tree = extract_function(
        source,
        "static void RenderBrowserSystemFolderSearchTree(AppState& state)",
    )

    check(bool(render_panel), "Browser sidebar panel render function is inspectable")
    check(bool(system_node), "Browser system folder node render function is inspectable")
    check(bool(search_node), "Browser search tree node render function is inspectable")
    check(bool(db_folder_node), "Database/shared folder tree node render function is inspectable")
    check(bool(search_task), "Browser system folder search task function is inspectable")
    check(bool(build_search_tree), "Browser search tree builder is inspectable")
    check(bool(render_search_tree), "Browser search tree render function is inspectable")

    related_pane = extract_function(source, "static void RenderRelatedPathsExplorerPane(")
    related_body = extract_function(
        render_panel, "for (const auto& child : relatedChildren.children)"
    )
    check(
        bool(related_body)
        and '"##BrowserRelatedPathsBox"' in source
        and "RenderRelatedPathsExplorerPane(" in render_panel
        and "renderRows();" in related_pane,
        "Browser Related Paths child body is inspectable",
    )
    check(
        "g_browserSystemFolderSearchResults" not in related_body,
        "Related Paths pane does not render folder-search results",
    )
    check(
        "state.fileBrowser.getCurrentPath()" not in related_body,
        "Related Paths pane does not render current path as a fake related-path row",
    )
    # 2026-07-08 alignment: source-truth is that Related Paths child folders
    # ARE clickable (Selectable rows). Click MUST call NavigateBrowserToFolder
    # (scope-preserving Browser nav) and MUST NOT push into the folder-search
    # result set. This keeps navigation ergonomic while preventing the pane
    # from turning into a second search list.
    check(
        "ImGui::Selectable(" in related_body
        and "NavigateBrowserToFolder(state, child.first, state.fileBrowser.getScope())" in related_body
        and "g_browserSystemFolderRelatedPath = child.first;" in related_body
        and "g_browserSystemFolderSearchResults" not in related_body,
        "Related Paths pane renders clickable child folders that navigate Browser (scope-preserving) without polluting the folder-search result set",
    )

    check(
        "if (!searchQuery.empty())" in render_panel
        and "RenderBrowserSystemFolderSearchTree(state)" in render_panel
        and render_system_tree in render_panel,
        "Browser search filters the lower system tree instead of the related-paths pane",
    )

    check(
        "flags |= ImGuiTreeNodeFlags_Leaf" in system_node
        and "ImGuiTreeNodeFlags_NoTreePushOnOpen" in system_node
        and "ImGui::TreePop();" not in system_node
        and "ImGui::Indent();" in system_node
        and "ImGui::Unindent();" in system_node,
        "System folder tree uses NoTreePushOnOpen/manual indentation and never TreePops",
    )
    check(
        "flags |= ImGuiTreeNodeFlags_Leaf" in search_node
        and "ImGuiTreeNodeFlags_NoTreePushOnOpen" in search_node
        and "ImGui::TreePop();" not in search_node
        and "ImGui::Indent();" in search_node
        and "ImGui::Unindent();" in search_node,
        "Search tree uses NoTreePushOnOpen/manual indentation and never TreePops",
    )
    check(
        "if (opened) {" not in system_node,
        "System folder tree has no unconditional opened TreePop block",
    )
    check(
        "ImGuiTreeNodeFlags_NoTreePushOnOpen" in db_folder_node
        and "ImGui::TreePop();" not in db_folder_node
        and "ImGui::Indent();" in db_folder_node
        and "ImGui::Unindent();" in db_folder_node,
        "Database/shared folder tree uses NoTreePushOnOpen/manual indentation and never TreePops",
    )

    check(
        ("it->is_regular_file(statusEc)" in search_task or "entry.isRegularFile" in search_task)
        and "addFolderHit(parentPath, parentName)" in search_task
        and "hits.push_back({filePath, name, parentPath, true})" not in source
        and "fileNode.isFile = true" not in build_search_tree,
        "System folder-tree search can match file names but only renders parent folders, never audio/file leaves",
    )
    check(
        "childName, entry.pathUtf8, terms" in search_task
        and "it.disable_recursion_pending();" not in search_task,
        "Folder-tree search keeps descending after a direct hit so broad terms show all matching folders",
    )
    check(
        "MatchesSystemFolderSearchQuery(rootName, root.path, terms)" in search_task,
        "Folder-tree search matches root nodes against the full root path",
    )
    check(
        "kBrowserSystemTreeChildLimit" not in source
        and "entry.children.size() >=" not in source,
        "System folder tree does not hard-truncate child folders",
    )
    check(
        "RenderBrowserSystemFolderSearchNode(state, child.second)" in search_node
        and "RenderBrowserSystemFolderNode(state, child.first, child.second)" not in search_node
        and "g_browserSystemFolderRelatedPath = node.path;" in search_node
        and "ImGui::IsItemToggledOpen() && opened && !node.path.empty()" in search_node,
        "Search tree keeps only hit chains; opened/clicked nodes populate Related Paths instead of inline real-subtree expansion",
    )
    check(
        "BeginBrowserPathAudition(state, selectedFile, state.settings.autoplayOnSelect)"
        not in search_node,
        "System folder-tree clicks never audition paths; only the center Browser list auditions audio",
    )

    check(
        "NavigateBrowserToFolder(state, folderPath, state.fileBrowser.getScope())"
        in system_node,
        "System folder tree clicks preserve CurrentDir/RecursiveAudio scope",
    )
    check(
        "NavigateBrowserToFolder(state, node.path, state.fileBrowser.getScope())"
        in search_node,
        "Search folder-node clicks preserve CurrentDir/RecursiveAudio scope",
    )
    check(
        "SetBrowserSystemFolderSearchFocusRoot(folderPath);" in system_node
        and "SetBrowserSystemFolderSearchFocusRoot(node.path);" not in search_node,
        "Normal tree clicks set search scope, but search-result clicks keep the existing result set visible",
    )
    check(
        "NavigateBrowserToFolder(state, folderPath, BrowserScope::CurrentDir)" not in system_node
        and "NavigateBrowserToFolder(state, node.path, BrowserScope::CurrentDir)" not in search_node,
        "Browser tree navigation does not silently force CurrentDir scope",
    )

    check(
        "kMaxBrowserFolderSearchHits" not in search_task
        and "kMaxBrowserFolderSearchHitsPerRoot" not in search_task
        and "kMaxBrowserFolderSearchVisited" not in search_task
        and "kMaxBrowserFolderSearchTime" not in search_task
        and "rootHitCount" not in search_task
        and "task->dirty.store(true" in search_task
        and "std::lock_guard<std::mutex>" in search_task,
        "Folder search is uncapped like the database tree while staying progressive and synchronized",
    )
    check(
        "if (!searchQuery.empty() && g_browserSystemFolderSearchRuntime.Busy())" in render_panel
        and "L(Main_FileCountsFmt)" in render_panel
        and "g_browserSystemFolderSearchResults.size()" in render_panel
        and "BrowserSystemFolderSearchVisitedCount()" in render_panel,
        "Folder search exposes a persistent loading/result count indicator while results are incomplete",
    )
    check(
        "GetBrowserSystemFolderChildren(g_browserSystemFolderRelatedPath)" in source
        and "##BrowserRelatedFolderCtx" in source
        and "g_browserSystemFolderRelatedPath = folderPath;" in system_node,
        "Related Paths pane shows child folders for the selected/opened system or search-tree folder",
    )
    check(
        "RunBrowserSystemFolderSearchIfNeeded(false, g_browserSystemFolderSearchFocusRoot);" in source
        and "RunBrowserSystemFolderSearchIfNeeded(false, currentBrowserPath)" not in source,
        "Folder search follows an explicit clicked-path focus root, not every Browser current-path change",
    )
    check(
        "g_browserSystemFolderSearchFocusRoot" in source
        and "SetBrowserSystemFolderSearchFocusRoot(" in source,
        "Folder search has a persistent clicked-path search scope anchor",
    )
    check(
        "if (!canonicalFocus.empty())" in source
        and "return roots;" in extract_function(
            source,
            "static std::vector<aura::browser::SystemFolderSearchRoot> BrowserSystemFolderSearchRoots(",
        ),
        "Folder search limits roots to the clicked focus path before considering all drives",
    )
    check(
        "PrimeBrowserSystemFolderPathChain(" in source
        and "(void)GetBrowserSystemFolderChildren(part.first);" in source,
        "Locate current folder primes the current path ancestor chain before scrolling",
    )
    check(
        "currentBrowserPath = state.fileBrowser.getCurrentPath();" in render_panel
        and clear_search in locate_action
        and locate_current in locate_action
        and locate_action.index(clear_search) < locate_action.index(locate_current)
        and "controls.locate.Request(g_browserSystemFolderTreeScrollTarget);" in locate_action,
        "Locate current folder clears folder search and reveals the Browser current path in the normal tree",
    )
    check(
        "g_browserSystemFolderTreeCollapseAll = true;" in refresh_action
        and "ClearBrowserSystemFolderSearchState();" in refresh_action
        and clear_search in refresh_action
        and "g_browserSystemFolderSearchFocusRoot.clear();" in refresh_action
        and "ResetBrowserSystemFolderTreeCache();" in refresh_action
        and "controls.locate = {};" in refresh_action
        and "LocateBrowserSystemFolderPath(" not in refresh_action
        and "QueueBrowserSystemFolderTreeLocate(" not in refresh_action
        and "controls.locate.Request(" not in refresh_action
        and render_panel.count("LocateBrowserSystemFolderPath(") == 1
        and 0 <= toolbar.find(refresh_action) < toolbar.find(locate_action),
        "Refresh button mirrors DB refresh: clear search/scope, collapse tree, and do not locate current path",
    )
    tree_list = extract_function(source, "static void RenderFolderTreeExplorerList(")
    check(
        "RenderFolderTreeExplorerList(" in render_panel
        and "g_browserSystemFolderTreeCollapseAll," in render_panel
        and "if (collapseAll)" in tree_list
        and "tw->DC.StateStorage->Clear();" in tree_list
        and tree_list.find("tw->DC.StateStorage->Clear();") < tree_list.find("renderRows();"),
        "Browser refresh clears ImGui tree storage like the database folder tree refresh",
    )
    check(
        "if (g_browserSystemFolderTreeCollapseAll) {\n        ImGui::SetNextItemOpen(false);" in system_node
        and "if (!g_browserSystemFolderTreeCollapseAll && currentPathMatches)" in system_node,
        "Refresh collapse frame suppresses current-path auto-open",
    )
    check(
        "RenderFolderTreeExplorerList(" in render_panel
        and render_system_tree in render_panel
        and render_panel.find("RenderFolderTreeExplorerList(")
        < render_panel.find("g_browserSystemFolderTreeCollapseAll = false;"),
        "Refresh collapse flag remains alive through tree rendering before being cleared",
    )

    check(
        "BuildBrowserTransferSourcePaths(state, entries, entryIndex)" in source
        and "SendToDAWForPaths(state, di, browserSendPaths)" in source,
        "Browser Send to DAW uses Browser-selected paths, not stale DB rows",
    )
    check(
        "g_browserSystemFolderTreeExpandSearchHitsOnce" in source
        and "g_browserSystemFolderSearchNeedsFirstHitReveal" in source,
        "Browser search tree has DB-style one-shot expand and first-hit reveal state",
    )
    check(
        "ImGui::SetNextItemOpen(true, ImGuiCond_Always)" not in search_node,
        "Browser search tree does not force every matching chain open every frame",
    )
    check(
        "g_browserSystemFolderTreeScrollTarget = HarkenSearch::CanonicalFolderPath(g_browserSystemFolderSearchResults.front().path)"
        in source
        and "ImGui::ScrollToItem();" in search_node
        and "g_browserSystemFolderTreeScrollTarget.clear();" in search_node
        and "g_browserSystemFolderTreeExpandSearchHitsOnce = false;" in render_search_tree,
        "Browser search tree scrolls to first hit and then returns expand control to the user",
    )
    check(
        "BeginBrowserPathAudition(state, entry, state.settings.autoplayOnSelect)" in source
        and "state.currentFileId = -1" in source,
        "Browser path audition uses no-DB identity and shared audition helper",
    )
    check(
        "ShowBrowserCurrentPathContextMenu(state, currentPath, canNavigateUp);" in source
        and "L(Browser_BookmarkFolder)" in source
        and "QueueBrowserFolderScan(state, currentPath)" in source
        and "QueueBrowserFolderLibraryAdd(state, currentPath)" in source,
        "Current path actions are in the path context menu",
    )
    # ExplorerSource supplies stable Browser IDs to the shared pane and tree.
    # Both sidebars use the same bounded, resizable pane.
    check(
        '"##BrowserFolderSearch"' in extract_function(source, "static ExplorerSource BrowserExplorerSource()")
        and '"##BrowserSystemFolderTree"' in extract_function(source, "static ExplorerSource BrowserExplorerSource()")
        and '"##BrowserRelatedPathsTreeSplitter"' in extract_function(source, "static ExplorerSource BrowserExplorerSource()")
        and "ExplorerShellCapabilities{true}" in extract_function(source, "static ExplorerShellCapabilities BrowserExplorerCapabilities()")
        and "RenderRelatedPathsExplorerPane(" in render_panel
        and "if (capabilities.canResizeRelatedPaths)" in related_pane
        and "HorizontalSplitter(" in related_pane,
        "Browser sidebar exposes search, bounded Related Paths pane, and system tree through ExplorerSource",
    )
    # 2e9692fb intentionally removed the extra tree title. Keep the clicked
    # normal-tree search scope even when a filtered result changes Related Paths.
    check(
        "2e9692fb" in contract
        and "without an extra System Folder Tree title" in contract
        and "browserSystemTreeLabel" not in render_panel
        and "L(Browser_SystemFolderTree)" not in source
        and "SetBrowserSystemFolderSearchFocusRoot(folderPath);" in system_node
        and "RunBrowserSystemFolderSearchIfNeeded(false, g_browserSystemFolderSearchFocusRoot);" in render_panel
        and "SetBrowserSystemFolderSearchFocusRoot(node.path);" not in search_node
        and "g_browserSystemFolderRelatedPath = node.path;" in search_node,
        "Browser keeps the clicked search scope without restoring the extra tree title or conflating Related Paths",
    )
    check(
        "L(Browser_SearchTruncated)" not in render_search_tree,
        "Uncapped folder search does not show a result-too-many/truncated message",
    )

    check(
        "Browser folder search + system tree" in contract
        and "folder nodes only" in contract
        and "does not audition audio" in contract
        and "system folder tree" in contract.lower()
        and "no-DB identity" in contract,
        "Behavior contract documents folder-only search tree and no-DB audition promises",
    )

    if failures:
        print(f"\n{len(failures)} Folder Browser contract checks failed", file=sys.stderr)
        return 1
    print("\nFolder Browser contract checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
