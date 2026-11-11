// tests/test_browser_search.cpp
// Tests the multi-word browser filename filter algorithm.
// Logic is defined inline here — keep in sync with BrowserEntryMatchesQuery in main.cpp.
#include <string>
#include <vector>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <fstream>

#include "../src/FolderTreeContextActions.h"

static std::vector<std::string> SplitBrowserQuery(const std::string& raw) {
    std::vector<std::string> terms;
    std::string cur;
    for (char c : raw) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { terms.push_back(cur); cur.clear(); }
        } else {
            cur += (char)std::tolower((unsigned char)c);
        }
    }
    if (!cur.empty()) terms.push_back(cur);
    return terms;
}

static bool BrowserEntryMatchesQuery(
    const std::string& nameLower,
    const std::string& relPathLower,
    const std::vector<std::string>& terms)
{
    for (const auto& term : terms) {
        if (nameLower.find(term) == std::string::npos &&
            relPathLower.find(term) == std::string::npos)
            return false;
    }
    return true;
}

static int failed = 0;
static int totalChecks = 0;

#define CHECK(cond, msg) do { \
    ++totalChecks; \
    if (cond) { std::cout << "PASS: " << msg << "\n"; } \
    else { std::cerr << "FAIL: " << msg << "\n"; ++failed; } \
} while(0)

struct QuickScanDialogTestState {
    std::atomic<bool> isScanning{false};
    std::vector<std::string> pendingScanFolders;
    bool scanDialogAutoStart = false;
    bool showScanDialog = false;
};

static void TestQuickScanDialogRejectsRetriggersWhileScanning() {
    const std::string activeFolder = R"(C:\\Audio\\Kicks)";
    const std::vector<std::string> retriggers = {
        activeFolder,
        R"(c:\\audio\\kicks)",
        R"(C:\\Audio\\Kicks\\.)",
    };

    for (const auto& retrigger : retriggers) {
        QuickScanDialogTestState state;
        state.isScanning.store(true);
        state.pendingScanFolders = {activeFolder};
        state.scanDialogAutoStart = true;
        state.showScanDialog = false;

        PrepareQuickScanDialogForPath(state, retrigger);

        CHECK(state.pendingScanFolders == std::vector<std::string>{activeFolder},
              "active scan keeps the existing folder request when the same resource retriggers");
        CHECK(state.scanDialogAutoStart,
              "active scan does not change the existing request's start intent");
        CHECK(!state.showScanDialog,
              "active scan does not open a second scan dialog");
    }
}

static std::string readTextFile(const std::filesystem::path& relativePath) {
    const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream in(repoRoot / relativePath, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int main() {
    TestQuickScanDialogRejectsRetriggersWhileScanning();
    {
        struct Shortcut { std::string name, path; bool openInBrowser = false; };
        struct State { std::vector<Shortcut> folderShortcuts; } state;
        state.folderShortcuts.push_back({"Custom name", "C:/Audio", true});
        CHECK(AddFolderShortcutIfMissing(state, "Audio", "C:/Audio"),
              "re-bookmarking from database updates an existing browser shortcut");
        CHECK(state.folderShortcuts.size() == 1 && !state.folderShortcuts[0].openInBrowser,
              "re-bookmark updates mode without creating a duplicate");
        CHECK(state.folderShortcuts[0].name == "Custom name", "re-bookmark preserves shortcut name");
        CHECK(AddFolderShortcutIfMissing(state, "Audio", "C:/Audio", true),
              "browser re-bookmark upgrades a legacy database shortcut");
        CHECK(state.folderShortcuts.size() == 1 && state.folderShortcuts[0].openInBrowser,
              "browser re-bookmark stores destination without duplicating the path");
        CHECK(!AddFolderShortcutIfMissing(state, "Audio", "C:/Audio", true),
              "same path and mode is a no-op");
        CHECK(AddFolderShortcutIfMissing(state, "Other", "C:/Other", true) &&
              state.folderShortcuts.back().openInBrowser, "new browser shortcut stores browser destination");
        CHECK(AddFolderShortcutIfMissing(state, "Database", "C:/Database") &&
              !state.folderShortcuts.back().openInBrowser, "database shortcuts retain database destination");
        CHECK(!AddFolderShortcutIfMissing(state, "Empty", "", true), "empty path is rejected");
#ifdef _WIN32
        CHECK(AddFolderShortcutIfMissing(state, "Audio", "c:\\audio\\", false),
              "same Windows folder with different spelling updates the existing mode");
        CHECK(state.folderShortcuts.size() == 3 && !state.folderShortcuts[0].openInBrowser,
              "slash and case differences do not duplicate shortcuts");
#endif
    }

    CHECK(BrowserEntryMatchesQuery("kick_01.wav", "", {}), "empty query matches");
    CHECK(BrowserEntryMatchesQuery("kick_01.wav", "", SplitBrowserQuery("kick")), "single term matches name");
    CHECK(!BrowserEntryMatchesQuery("snare_01.wav", "", SplitBrowserQuery("kick")), "single term no match");
    CHECK(BrowserEntryMatchesQuery("kick_01.wav", "", SplitBrowserQuery("kick 01")), "multi-word both in name");
    CHECK(!BrowserEntryMatchesQuery("kick_01.wav", "", SplitBrowserQuery("kick drum")), "multi-word partial miss");
    CHECK(BrowserEntryMatchesQuery("kick_01.wav", "drums/hard", SplitBrowserQuery("drums")), "term matches relPath");
    CHECK(BrowserEntryMatchesQuery("kick_01.wav", "", SplitBrowserQuery("KICK")), "case insensitive");
    CHECK(BrowserEntryMatchesQuery("kick_01.wav", "drums/hard", SplitBrowserQuery("drums kick")), "terms across name+relPath");
    CHECK(BrowserEntryMatchesQuery("kick_01.wav", "", SplitBrowserQuery("kick  01")), "double space between terms");

    const std::string mainCpp = readTextFile("src/main.cpp");
    const std::string browserPolicy = readTextFile("src/BrowserSystemFolderSearchPolicy.cpp");
    const std::string browserRuntime = readTextFile("src/BrowserSystemFolderSearchRuntime.cpp");
    CHECK(!mainCpp.empty(), "source contract can read main.cpp");
    CHECK(!browserPolicy.empty(), "source contract can read browser system folder search policy");
    CHECK(!browserRuntime.empty(), "source contract can read browser system folder search runtime");
    if (!mainCpp.empty() && !browserPolicy.empty() && !browserRuntime.empty()) {
        {
            const auto begin = mainCpp.find("const bool needsDbSetup =");            const auto end = mainCpp.find(';', begin);
            CHECK(begin != std::string::npos &&
                  mainCpp.substr(begin, end - begin).find("state.viewMode != ViewMode::Browser") != std::string::npos,
                  "database empty-state surfaces never hide browser files");
        }
        CHECK(mainCpp.find("showEmptyResultsSurface") == std::string::npos &&
              mainCpp.find("showStandaloneEmptyResultsSurface") == std::string::npos &&
              mainCpp.find("\"##EmptyResultsSurface\"") == std::string::npos,
              "TASK-20261028-NO-EMPTY-SURFACE: zero-result lists reserve no standalone empty box");        const auto shortcutStart = mainCpp.find("if (activeSectionId == 5 && state.settings.showFolderShortcuts)");
        const auto shortcutEnd = mainCpp.find("// Section 1:", shortcutStart);
        const std::string shortcuts = mainCpp.substr(shortcutStart, shortcutEnd - shortcutStart);
        CHECK(shortcuts.find("OpenFolderShortcut(state, sc)") != std::string::npos &&
              mainCpp.find("NavigateBrowserToFolder(state, shortcut.path, state.fileBrowser.getScope())") != std::string::npos,
              "shortcut click routes browser paths through real scope-preserving navigation");
        CHECK(shortcuts.find("state.viewMode == ViewMode::Browser") != std::string::npos &&
              shortcuts.find("state.viewMode == ViewMode::Database") != std::string::npos,
              "shortcut selection is scoped to the active mode");
        CHECK(mainCpp.find("sc.openInBrowser = ParseBool(kv[prefix + \"openInBrowser\"], false)") != std::string::npos &&
              mainCpp.find("{prefix + \"openInBrowser\", sc.openInBrowser ? \"1\" : \"0\"}") != std::string::npos,
              "shortcut mode persists and old settings default to database");
        CHECK(mainCpp.find("AddFolderShortcutIfMissing(state, folderName, folderPath, true)") != std::string::npos,
              "all browser bookmark entry points explicitly save browser mode");
        CHECK(mainCpp.find("state.fileBrowser.tickDirScanResult();") != std::string::npos,
              "browser top strip pumps async current-directory scan results");
        CHECK(mainCpp.find("state.fileBrowser.tickScanResult();") != std::string::npos,
              "browser top strip pumps async recursive-audio scan results");
        CHECK(mainCpp.find("const auto& entries = state.fileBrowser.activeEntries();") != std::string::npos,
              "browser list rows are driven directly from FileBrowser active entries");
        CHECK(mainCpp.find("clipper.Begin((int)filteredRows.size(), actualBrowserRowH);") != std::string::npos,
              "browser clipper consumes filtered browser rows");
        CHECK(mainCpp.find("state.fileBrowser.activeAudioFileCount()") != std::string::npos,
              "browser header count uses FileBrowser cached audio count");
        CHECK(mainCpp.find("state.database.getFilesByExactFolder(curDir, 5000)") != std::string::npos,
              "browser current-folder metadata enrichment uses an exact-folder DB preload instead of descendant subtree enumeration");
        CHECK(mainCpp.find("recursiveBrowserView") != std::string::npos &&
              mainCpp.find("Do not synchronously preload descendant DB metadata on the UI thread.") != std::string::npos,
              "browser recursive mode avoids synchronous subtree metadata preload on the UI thread");
        CHECK(mainCpp.find("state.addPendingWaveform(entry.fullPath, true);") != std::string::npos,
              "browser visible rows still enqueue progressive mini-wave backfill");
        CHECK(mainCpp.find("const std::string browserMetaCacheKey =") != std::string::npos &&
              mainCpp.find("curDir + \"|scope:\" + std::to_string((int)state.fileBrowser.getScope())") != std::string::npos,
              "browser metadata cache keys itself by directory plus Browser scope");
        CHECK(mainCpp.find("state.browserMetaCache.clear();") != std::string::npos,
              "browser metadata cache resets when the current directory changes");
        CHECK(mainCpp.find("BeginBrowserPathAudition(state, entry, true)") != std::string::npos,
              "browser right-click play and double-click path audition share the unified no-DB audition helper");
        CHECK(mainCpp.find("BeginBrowserPathAudition(state, entry, state.settings.autoplayOnSelect)") != std::string::npos,
              "browser single-click and deferred-select audition use the unified no-DB audition helper");
        CHECK(mainCpp.find("BuildBrowserTransferSourcePaths(state, entries, entryIndex)") != std::string::npos,
              "browser send path builder derives DAW payload from browser selection instead of stale DB rows");
        CHECK(mainCpp.find("SendToDAWForPaths(state, di, browserSendPaths)") != std::string::npos,
              "browser Send to DAW routes through browser-selected paths");
        CHECK(mainCpp.find("RenderBrowserSystemFolderTree(state)") != std::string::npos,
              "browser sidebar exposes a dedicated system folder tree surface");
        CHECK(mainCpp.find("Browser top strip should align with the database results lane.") != std::string::npos &&
              mainCpp.find("ImGui::Indent(0.0f);") != std::string::npos,
              "browser top strip does not add a left indent that would shift it away from the database list lane");
        CHECK(mainCpp.find("RenderBrowserFolderTreeSidebarPanel(state);") != std::string::npos,
              "browser system folder tree is mounted from the real folder-tree sidebar section");
        CHECK(mainCpp.find("RunBrowserSystemFolderSearchIfNeeded(") != std::string::npos,
              "browser sidebar runs an explicit folder-search workflow instead of relying on DB folder filters");
        CHECK(mainCpp.find("RunBrowserSystemFolderSearchIfNeeded(false, g_browserSystemFolderSearchFocusRoot);") != std::string::npos &&
              mainCpp.find("RunBrowserSystemFolderSearchIfNeeded(false, currentBrowserPath)") == std::string::npos,
              "browser folder search uses an explicit clicked-path focus root instead of following every current-path change");
        CHECK(mainCpp.find("g_browserSystemFolderSearchFocusRoot") != std::string::npos &&
              mainCpp.find("SetBrowserSystemFolderSearchFocusRoot(") != std::string::npos,
              "browser folder search has a persistent clicked-path search scope anchor");
        {
            const size_t searchTask = mainCpp.find("static void RunBrowserSystemFolderSearchIfNeeded(");
            const size_t searchTaskEnd = mainCpp.find("static void PrimeBrowserSystemFolderPathChain", searchTask);
            const std::string taskBody =
                (searchTask != std::string::npos && searchTaskEnd != std::string::npos)
                    ? mainCpp.substr(searchTask, searchTaskEnd - searchTask)
                    : std::string();
            CHECK(taskBody.find("kMaxBrowserFolderSearchHits") == std::string::npos &&
                  taskBody.find("kMaxBrowserFolderSearchHitsPerRoot") == std::string::npos &&
                  taskBody.find("kMaxBrowserFolderSearchVisited") == std::string::npos &&
                  taskBody.find("kMaxBrowserFolderSearchTime") == std::string::npos &&
                  taskBody.find("rootHitCount") == std::string::npos,
                  "browser folder search has no result/time/visited/per-root cap; it runs until roots finish or query is cancelled");
        }
        CHECK(browserRuntime.find("task->dirty.store(true") != std::string::npos &&
              mainCpp.find("RenderBrowserSystemFolderSearchTree(state)") != std::string::npos,
              "browser system tree can show progressive partial hits while search is still running");
        {
            const size_t searchTreeFn = mainCpp.find("static void RenderBrowserSystemFolderSearchTree");
            const size_t rootsDecl = mainCpp.find(
                "aura::browser::BuildSystemFolderSearchTree(", searchTreeFn);
            CHECK(searchTreeFn != std::string::npos &&
                  rootsDecl != std::string::npos,
                  "browser folder search renders progressive results without a hidden bottom-only truncation status");
        }
        CHECK(browserRuntime.find("entry.isRegularFile") != std::string::npos &&
              browserRuntime.find("addFolderHit(parentPath, parentName)") != std::string::npos &&
              browserRuntime.find("hits.push_back({filePath, name, parentPath, true})") == std::string::npos,
              "browser system folder tree search can match file names but only returns parent folder hits");
        CHECK(browserRuntime.find("childName, entry.pathUtf8, terms") != std::string::npos &&
              browserRuntime.find("it.disable_recursion_pending();") == std::string::npos,
              "browser folder-tree search keeps descending after a direct hit so broad terms show all matching folders");
        CHECK(browserRuntime.find("MatchesSystemFolderSearchQuery(rootName, root.path, terms)") != std::string::npos,
              "browser folder-tree search matches root nodes against the full root path");
        CHECK(mainCpp.find("kBrowserSystemTreeChildLimit") == std::string::npos &&
              mainCpp.find("entry.children.size() >=") == std::string::npos,
              "browser system folder tree does not hard-truncate child folders");
        CHECK(mainCpp.find("RenderBrowserSystemFolderSearchNode(state, child.second)") != std::string::npos &&
              mainCpp.find("g_browserSystemFolderRelatedPath = node.path;") != std::string::npos &&
              mainCpp.find("ImGui::IsItemToggledOpen() && opened && !node.path.empty()") != std::string::npos,
              "browser search tree keeps only hit chains; opened/clicked nodes populate Related Paths instead of inline real-subtree expansion");
        CHECK(mainCpp.find("fileNode.isFile = true") == std::string::npos &&
              mainCpp.find("BeginBrowserPathAudition(state, selectedFile, state.settings.autoplayOnSelect)") == std::string::npos,
              "browser system folder tree clicks never audition paths; only the center file list auditions audio");
        CHECK(mainCpp.find("aura::browser::BuildSystemFolderSearchTree(") != std::string::npos &&
              browserPolicy.find("BuildSystemFolderPathChain(hit.path)") != std::string::npos,
              "browser folder search renders folder hits as ancestor chains inside the system tree");
        CHECK(mainCpp.find("g_browserSystemFolderTreeExpandSearchHitsOnce") != std::string::npos &&
              mainCpp.find("g_browserSystemFolderSearchNeedsFirstHitReveal") != std::string::npos,
              "browser folder search mirrors DB tree one-shot expand plus first-hit reveal state");
        CHECK(mainCpp.find("ImGui::SetNextItemOpen(true, ImGuiCond_Always)") == std::string::npos,
              "browser folder search does not force every hit chain open on every frame");
        CHECK(mainCpp.find("g_browserSystemFolderTreeScrollTarget = HarkenSearch::CanonicalFolderPath(g_browserSystemFolderSearchResults.front().path)") != std::string::npos,
              "browser folder search scrolls to the first direct hit like the database folder tree");
        CHECK(mainCpp.find("g_browserSystemFolderTreeExpandSearchHitsOnce = false;") != std::string::npos,
              "browser folder search gives tree expand control back after the first reveal frame");
        CHECK(mainCpp.find("if (!canonicalFocus.empty())") != std::string::npos &&
              mainCpp.find("return roots;") != std::string::npos,
              "browser folder search limits roots to the clicked focus path instead of falling back to all drives");
        {
            const size_t paneFn = mainCpp.find("static void RenderRelatedPathsExplorerPane(");
            const size_t browserCall = mainCpp.find("\"##BrowserRelatedPathsBox\"");
            const size_t treeList = mainCpp.find("\"##BrowserSystemFolderTree\"");
            const size_t relatedChildren = mainCpp.find("GetBrowserSystemFolderChildren(g_browserSystemFolderRelatedPath)");
            const size_t relatedEnd =
                relatedChildren == std::string::npos ? std::string::npos
                                                     : mainCpp.find("});", relatedChildren);
            const std::string relatedScope =
                relatedChildren == std::string::npos || relatedEnd == std::string::npos
                    ? std::string{}
                    : mainCpp.substr(relatedChildren, relatedEnd - relatedChildren);
            CHECK(paneFn != std::string::npos &&
                  browserCall != std::string::npos &&
                  treeList != std::string::npos &&
                  relatedScope.find("g_browserSystemFolderSearchResults") == std::string::npos,
                  "browser Related Paths pane does not render folder-search hits directly");
            CHECK(paneFn != std::string::npos &&
                  browserCall != std::string::npos &&
                  relatedScope.find("state.fileBrowser.getCurrentPath()") == std::string::npos,
                  "browser Related Paths pane does not render the current path as a fake related-path row");
        }
        CHECK(mainCpp.find("\"##BrowserFolderSearch\"") != std::string::npos,
              "browser sidebar exposes a dedicated search field for system folders");
        {
            const size_t browserToolbar = mainCpp.find("browserSource.searchId");
            const size_t browserHintEnd = mainCpp.find("g_browserSystemFolderSearchBuffer", browserToolbar);
            const std::string browserHintScope =
                browserToolbar == std::string::npos || browserHintEnd == std::string::npos || browserHintEnd <= browserToolbar
                    ? std::string{}
                    : mainCpp.substr(browserToolbar, browserHintEnd - browserToolbar);
            CHECK(!browserHintScope.empty() && browserHintScope.find("L(Filter_SearchFolder)") == std::string::npos,
                  "browser folder search uses the same empty hint as the database folder tree");
        }
        {
            const size_t browserList = mainCpp.find("browserSource.treeListChildId");
            const size_t nextCall = mainCpp.find("RenderFolderTreeExplorerList(", browserList);
            const std::string browserListCall =
                browserList == std::string::npos || nextCall == std::string::npos
                    ? std::string{}
                    : mainCpp.substr(browserList, 280);
            CHECK(!browserListCall.empty() &&
                  browserListCall.find("g_browserSystemFolderTreeCollapseAll") != std::string::npos &&
                  browserListCall.find("false") != std::string::npos,
                  "browser folder tree uses the same non-rounded list chrome as the database tree");
        }
        CHECK(mainCpp.find("ImGui::TextDisabled(\"%s\", browserSystemTreeLabel.c_str());") == std::string::npos,
              "browser folder tree does not draw an extra System Folder Tree title above the shared list");
        CHECK(mainCpp.find("compactTreePadY") != std::string::npos &&
              mainCpp.find("RenderBrowserSystemFolderTree(state)") != std::string::npos,
              "browser folder tree uses the same compact row and indent spacing as the database tree");
        CHECK(mainCpp.find("BeginTransientScrollbarChild(\"##SidebarContent\"") != std::string::npos &&
              mainCpp.find("ImGuiWindowFlags_NoScrollbar") != std::string::npos,
              "sidebar content does not keep an extra outer scrollbar over the folder Filter");
        CHECK(mainCpp.find("static void RenderFolderTreeExplorerToolbar(") != std::string::npos,
              "folder-tree search/refresh/locate chrome lives in one shared toolbar helper");
        CHECK(mainCpp.find("RenderFolderTreeExplorerToolbar(") != std::string::npos &&
              mainCpp.find("\"##BrowserFolderSearch\"") != std::string::npos &&
              mainCpp.find("\"##BrowserSidebarRefreshBtn\"") != std::string::npos &&
              mainCpp.find("\"##BrowserLocateCurrentFolderBtn\"") != std::string::npos,
              "browser folder-tree toolbar uses the shared helper with its own search/refresh/locate ids");
        CHECK(mainCpp.find("RenderFolderTreeExplorerToolbar(") != std::string::npos &&
              mainCpp.find("\"##FolderSearch\"") != std::string::npos &&
              mainCpp.find("\"##SidebarRefreshBtn\"") != std::string::npos &&
              mainCpp.find("\"##LocateBtn\"") != std::string::npos,
              "database folder-tree toolbar uses the same shared helper with its own search/refresh/locate ids");
        CHECK(mainCpp.find("static void RenderFolderTreeExplorerList(") != std::string::npos,
              "folder-tree list chrome lives in one shared explorer list helper");
        CHECK(mainCpp.find("RenderFolderTreeExplorerList(") != std::string::npos &&
              mainCpp.find("\"##BrowserSystemFolderTree\"") != std::string::npos &&
              mainCpp.find("RenderBrowserSystemFolderTree(state)") != std::string::npos &&
              mainCpp.find("RenderBrowserSystemFolderSearchTree(state)") != std::string::npos,
              "browser folder tree uses the shared list helper and still renders system/search trees");
        CHECK(mainCpp.find("RenderFolderTreeExplorerList(") != std::string::npos &&
              mainCpp.find("\"FolderTree\"") != std::string::npos,
              "database folder tree uses the same shared list helper with its own child id");
        CHECK(mainCpp.find("static void RenderExplorerShell(") != std::string::npos,
              "folder explorer chrome is composed through one Explorer Shell entry");
        CHECK(mainCpp.find("RenderExplorerShell(") != std::string::npos &&
              mainCpp.find("static void RenderRelatedPathsExplorerPane(") != std::string::npos &&
              mainCpp.find("static void RenderFolderTreeExplorerToolbar(") != std::string::npos &&
              mainCpp.find("static void RenderFolderTreeExplorerList(") != std::string::npos,
              "Explorer Shell keeps the related-paths, toolbar, and tree-list helpers");
        CHECK(mainCpp.find("struct ExplorerShellCapabilities") != std::string::npos &&
              mainCpp.find("bool canResizeRelatedPaths") != std::string::npos,
              "Explorer Shell exposes a capability flag for Related Paths resizing");
        CHECK(mainCpp.find("BrowserExplorerCapabilities(") != std::string::npos &&
              mainCpp.find("DatabaseExplorerCapabilities(") != std::string::npos,
              "Browser and Database each declare Explorer Shell capabilities");
        CHECK(mainCpp.find("BrowserExplorerCapabilities()") != std::string::npos &&
              mainCpp.find("DatabaseExplorerCapabilities()") != std::string::npos,
              "Browser and Database capability helpers are called from the shared explorer chrome");
        {
            const size_t browserCapsFn = mainCpp.find("static ExplorerShellCapabilities BrowserExplorerCapabilities(");
            const size_t databaseCapsFn = mainCpp.find("static ExplorerShellCapabilities DatabaseExplorerCapabilities(");
            const size_t browserSrc = mainCpp.find("static ExplorerSource BrowserExplorerSource(");
            const std::string browserCapsBody =
                browserCapsFn == std::string::npos || databaseCapsFn == std::string::npos || databaseCapsFn <= browserCapsFn
                    ? std::string{}
                    : mainCpp.substr(browserCapsFn, databaseCapsFn - browserCapsFn);
            const std::string databaseCapsBody =
                databaseCapsFn == std::string::npos || browserSrc == std::string::npos || browserSrc <= databaseCapsFn
                    ? std::string{}
                    : mainCpp.substr(databaseCapsFn, browserSrc - databaseCapsFn);
            CHECK(browserCapsBody.find("ExplorerShellCapabilities{true}") != std::string::npos,
                  "BrowserExplorerCapabilities returns canResizeRelatedPaths enabled");
            CHECK(databaseCapsBody.find("ExplorerShellCapabilities{true}") != std::string::npos,
                  "DatabaseExplorerCapabilities returns canResizeRelatedPaths enabled");
        }
        {
            const size_t browserShell = mainCpp.find("const ExplorerSource browserSource = BrowserExplorerSource(");
            const size_t databaseShell = mainCpp.find("const ExplorerSource databaseSource = DatabaseExplorerSource(");
            const std::string browserShellBody =
                browserShell == std::string::npos || databaseShell == std::string::npos || databaseShell <= browserShell
                    ? std::string{}
                    : mainCpp.substr(browserShell, databaseShell - browserShell);
            const std::string databaseShellBody =
                databaseShell == std::string::npos ? std::string{} : mainCpp.substr(databaseShell);
            CHECK(browserShellBody.find("browserSource.capabilities") != std::string::npos &&
                  browserShellBody.find("RenderRelatedPathsExplorerPane(") != std::string::npos &&
                  browserShellBody.find("browserCaps,") != std::string::npos,
                  "browser Related Paths pane receives BrowserExplorerCapabilities instead of a discarded flag");
            CHECK(databaseShellBody.find("databaseSource.capabilities") != std::string::npos &&
                  databaseShellBody.find("RenderRelatedPathsExplorerPane(") != std::string::npos &&
                  databaseShellBody.find("databaseCaps,") != std::string::npos,
                  "database Related Paths pane receives DatabaseExplorerCapabilities instead of a discarded flag");
        }
        CHECK(mainCpp.find("struct ExplorerSource") != std::string::npos &&
              mainCpp.find("ExplorerShellCapabilities capabilities") != std::string::npos,
              "Explorer data entry wraps existing capabilities without a full adapter");
        CHECK(mainCpp.find("static ExplorerSource BrowserExplorerSource(") != std::string::npos &&
              mainCpp.find("static ExplorerSource DatabaseExplorerSource(") != std::string::npos &&
              mainCpp.find("static ExplorerSource FavoritesExplorerSource(") == std::string::npos,
              "Browser and Database each expose a minimal Explorer data entry; Favorites is retired");
        CHECK(mainCpp.find("IExplorerAdapter") == std::string::npos,
              "this slice does not extract a full IExplorerAdapter");
        CHECK(mainCpp.find("BrowserExplorerSource()") != std::string::npos &&
              mainCpp.find("DatabaseExplorerSource()") != std::string::npos,
              "Browser and Database sidebars take capabilities from their Explorer data entries");
        {
            const size_t section = mainCpp.find("// Section 1:");
            const size_t browserSrc = mainCpp.find("const ExplorerSource browserSource = BrowserExplorerSource(", section);
            const size_t databaseSrc = mainCpp.find("const ExplorerSource databaseSource = DatabaseExplorerSource(", section);
            const size_t favoritesSrc = mainCpp.find("const ExplorerSource favoritesSource = FavoritesExplorerSource(", section);
            const size_t browserCall = mainCpp.find("RenderBrowserFolderTreeSidebarPanel(state)", section);
            CHECK(section != std::string::npos &&
                  browserSrc != std::string::npos &&
                  databaseSrc != std::string::npos &&
                  favoritesSrc == std::string::npos &&
                  browserCall != std::string::npos &&
                  browserSrc < browserCall &&
                  databaseSrc < browserCall,
                  "File Browser and Database sources are chosen before rendering the Filter shell");
        }
        CHECK(mainCpp.find("GetBrowserSystemFolderChildren(g_browserSystemFolderRelatedPath)") != std::string::npos &&
              mainCpp.find("NavigateBrowserToFolder(state, child.first, state.fileBrowser.getScope())") != std::string::npos &&
              mainCpp.find("LocateBrowserSystemFolderPath(state.fileBrowser.getCurrentPath())") != std::string::npos &&
              mainCpp.find("RenderBrowserSystemFolderTree(state)") != std::string::npos,
              "File Browser still uses disk Related Paths, disk locate, and the system folder tree");
        CHECK(mainCpp.find("const char* relatedPathsChildId") != std::string::npos &&
              mainCpp.find("const char* relatedPathsSplitterId") != std::string::npos,
              "Explorer data entry carries Related Paths child and splitter ids");
        {
            const size_t browserSrc = mainCpp.find("static ExplorerSource BrowserExplorerSource(");
            const size_t databaseSrc = mainCpp.find("static ExplorerSource DatabaseExplorerSource(");
            const size_t paneFn = mainCpp.find("static void RenderRelatedPathsExplorerPane(");
            const std::string browserSrcBody =
                browserSrc == std::string::npos || databaseSrc == std::string::npos || databaseSrc <= browserSrc
                    ? std::string{}
                    : mainCpp.substr(browserSrc, databaseSrc - browserSrc);
            const std::string databaseSrcBody =
                databaseSrc == std::string::npos || paneFn == std::string::npos || paneFn <= databaseSrc
                    ? std::string{}
                    : mainCpp.substr(databaseSrc, paneFn - databaseSrc);
            CHECK(browserSrcBody.find("\"##BrowserRelatedPathsBox\"") != std::string::npos &&
                  browserSrcBody.find("\"##BrowserRelatedPathsTreeSplitter\"") != std::string::npos,
                  "Browser Explorer source owns Related Paths child and splitter ids");
            CHECK(databaseSrcBody.find("\"##RelatedPathsBox\"") != std::string::npos &&
                  databaseSrcBody.find("\"##RelatedPathsTreeSplitter\"") != std::string::npos,
                  "Database Explorer source owns Related Paths child and splitter ids");
        }
        CHECK(mainCpp.find("browserSource.relatedPathsChildId") != std::string::npos &&
              mainCpp.find("browserSource.relatedPathsSplitterId") != std::string::npos,
              "browser Related Paths pane uses child and splitter ids from the Explorer source");
        CHECK(mainCpp.find("databaseSource.relatedPathsChildId") != std::string::npos &&
              mainCpp.find("databaseSource.relatedPathsSplitterId") != std::string::npos,
              "database Related Paths pane uses child and splitter ids from the Explorer source");
        CHECK(mainCpp.find("const char* searchId") != std::string::npos &&
              mainCpp.find("const char* refreshId") != std::string::npos &&
              mainCpp.find("const char* locateId") != std::string::npos,
              "Explorer data entry carries search, refresh, and locate ids");
        {
            const size_t browserSrc = mainCpp.find("static ExplorerSource BrowserExplorerSource(");
            const size_t databaseSrc = mainCpp.find("static ExplorerSource DatabaseExplorerSource(");
            const size_t paneFn = mainCpp.find("static void RenderRelatedPathsExplorerPane(");
            const std::string browserSrcBody =
                browserSrc == std::string::npos || databaseSrc == std::string::npos || databaseSrc <= browserSrc
                    ? std::string{}
                    : mainCpp.substr(browserSrc, databaseSrc - browserSrc);
            const std::string databaseSrcBody =
                databaseSrc == std::string::npos || paneFn == std::string::npos || paneFn <= databaseSrc
                    ? std::string{}
                    : mainCpp.substr(databaseSrc, paneFn - databaseSrc);
            CHECK(browserSrcBody.find("\"##BrowserFolderSearch\"") != std::string::npos &&
                  browserSrcBody.find("\"##BrowserSidebarRefreshBtn\"") != std::string::npos &&
                  browserSrcBody.find("\"##BrowserLocateCurrentFolderBtn\"") != std::string::npos,
                  "Browser Explorer source owns search, refresh, and locate ids");
            CHECK(databaseSrcBody.find("\"##FolderSearch\"") != std::string::npos &&
                  databaseSrcBody.find("\"##SidebarRefreshBtn\"") != std::string::npos &&
                  databaseSrcBody.find("\"##LocateBtn\"") != std::string::npos,
                  "Database Explorer source owns search, refresh, and locate ids");
        }
        CHECK(mainCpp.find("browserSource.searchId") != std::string::npos &&
              mainCpp.find("browserSource.refreshId") != std::string::npos &&
              mainCpp.find("browserSource.locateId") != std::string::npos,
              "browser folder-tree toolbar uses search/refresh/locate ids from the Explorer source");
        CHECK(mainCpp.find("databaseSource.searchId") != std::string::npos &&
              mainCpp.find("databaseSource.refreshId") != std::string::npos &&
              mainCpp.find("databaseSource.locateId") != std::string::npos,
              "database folder-tree toolbar uses search/refresh/locate ids from the Explorer source");
        CHECK(mainCpp.find("const char* treeListChildId") != std::string::npos,
              "Explorer data entry carries the folder-tree list child id");
        {
            const size_t browserSrc = mainCpp.find("static ExplorerSource BrowserExplorerSource(");
            const size_t databaseSrc = mainCpp.find("static ExplorerSource DatabaseExplorerSource(");
            const size_t paneFn = mainCpp.find("static void RenderRelatedPathsExplorerPane(");
            const std::string browserSrcBody =
                browserSrc == std::string::npos || databaseSrc == std::string::npos || databaseSrc <= browserSrc
                    ? std::string{}
                    : mainCpp.substr(browserSrc, databaseSrc - browserSrc);
            const std::string databaseSrcBody =
                databaseSrc == std::string::npos || paneFn == std::string::npos || paneFn <= databaseSrc
                    ? std::string{}
                    : mainCpp.substr(databaseSrc, paneFn - databaseSrc);
            CHECK(browserSrcBody.find("\"##BrowserSystemFolderTree\"") != std::string::npos,
                  "Browser Explorer source owns the folder-tree list child id");
            CHECK(databaseSrcBody.find("\"FolderTree\"") != std::string::npos,
                  "Database Explorer source owns the folder-tree list child id");
        }
        CHECK(mainCpp.find("browserSource.treeListChildId") != std::string::npos,
              "browser folder tree uses the list child id from the Explorer source");
        CHECK(mainCpp.find("databaseSource.treeListChildId") != std::string::npos,
              "database folder tree uses the list child id from the Explorer source");
        CHECK(mainCpp.find("FavoritesExplorerSource()") == std::string::npos &&
              mainCpp.find("favoritesSource.relatedPathsChildId") == std::string::npos &&
              mainCpp.find("favoritesSource.searchId") == std::string::npos &&
              mainCpp.find("favoritesSource.treeListChildId") == std::string::npos,
              "Favorites Filter source is retired from the shared Explorer Shell");
        CHECK(mainCpp.find("if (state.viewMode != ViewMode::Database)") == std::string::npos,
              "Related Paths chrome is not hidden just because the view is Favorites");
        CHECK(mainCpp.find("GetBrowserSystemFolderChildren(g_browserSystemFolderRelatedPath)") != std::string::npos &&
              mainCpp.find("state.currentPathFacets") != std::string::npos,
              "Browser Related Paths still lists child folders while Database still lists facet counts");
        {
            const size_t paneFn = mainCpp.find("static void RenderRelatedPathsExplorerPane(");
            const size_t nextFn = mainCpp.find("static void RenderFolderTreeExplorerToolbar(", paneFn);
            const std::string paneBody =
                paneFn == std::string::npos || nextFn == std::string::npos || nextFn <= paneFn
                    ? std::string{}
                    : mainCpp.substr(paneFn, nextFn - paneFn);
            const size_t capGate = paneBody.find("canResizeRelatedPaths");
            const size_t splitter = paneBody.find("HorizontalSplitter(");
            const size_t dummy = paneBody.find("ImGui::Dummy(");
            CHECK(!paneBody.empty() && paneBody.find("ExplorerShellCapabilities") != std::string::npos,
                  "Related Paths pane receives ExplorerShellCapabilities so the splitter can follow the capability flag");
            CHECK(capGate != std::string::npos && splitter != std::string::npos && splitter > capGate,
                  "Related Paths draws HorizontalSplitter only after checking canResizeRelatedPaths");
            CHECK(capGate != std::string::npos && dummy != std::string::npos && dummy > capGate,
                  "Related Paths keeps a fixed-height Dummy when canResizeRelatedPaths is false");
            CHECK(paneBody.find("RelatedPathsTreeGrip") == std::string::npos,
                  "disabled Related Paths resize does not introduce a new grip id");
        }
        CHECK(mainCpp.find("(void)browserCaps") == std::string::npos &&
              mainCpp.find("(void)databaseCaps") == std::string::npos,
              "Browser and Database pass explorer capabilities into the Related Paths pane instead of discarding them");
        CHECK(mainCpp.find("##BrowserRelatedPathsTreeSplitter") != std::string::npos,
              "browser Related Paths and the system folder tree share a draggable splitter");
        CHECK(mainCpp.find("static void RenderRelatedPathsExplorerPane(") != std::string::npos,
              "Related Paths chrome lives in one shared explorer pane helper");
        CHECK(mainCpp.find("RenderRelatedPathsExplorerPane(") != std::string::npos &&
              mainCpp.find("\"##BrowserRelatedPathsBox\"") != std::string::npos &&
              mainCpp.find("\"##BrowserRelatedPathsTreeSplitter\"") != std::string::npos,
              "browser Related Paths uses the shared explorer pane helper with its own child and splitter ids");
        CHECK(mainCpp.find("RenderRelatedPathsExplorerPane(") != std::string::npos &&
              mainCpp.find("\"##RelatedPathsBox\"") != std::string::npos &&
              mainCpp.find("\"##RelatedPathsTreeSplitter\"") != std::string::npos,
              "database Related Paths uses the same shared explorer pane helper with its own child and splitter ids");
        CHECK(mainCpp.find("##BrowserRelatedPathsTreeGrip") == std::string::npos,
              "browser sidebar does not add an extra horizontal three-dot seam between Related Paths and the system folder tree");
        CHECK(mainCpp.find("const bool hideLeftSplitterInBrowser = false;") != std::string::npos,
              "browser layout keeps the global left splitter visible so the audio list has the expected left drag handle");
        CHECK(mainCpp.find("(leftDockSlotRequired && !hideLeftSplitterInBrowser) ? splitterWidth : 0.0f") != std::string::npos &&
              mainCpp.find("VerticalSplitter(\"##LeftSplitter\"") != std::string::npos,
              "browser mode renders the left dock splitter between folder navigator and audio list");
        CHECK(mainCpp.find("g_browserSystemFolderSearchBuffer") != std::string::npos,
              "browser folder search keeps a dedicated query buffer");
        CHECK(mainCpp.find("ImGuiTreeNodeFlags_Selected") != std::string::npos,
              "browser system tree highlights the exact current folder");
        CHECK(mainCpp.find("NavigateBrowserToFolder(state, folderPath, state.fileBrowser.getScope())") != std::string::npos,
              "browser system tree folder clicks preserve the current recursive/current-directory scope");
        CHECK(mainCpp.find("NavigateBrowserToFolder(state, node.path, state.fileBrowser.getScope())") != std::string::npos,
              "browser search-tree clicks preserve the current recursive/current-directory scope");
        CHECK(mainCpp.find("SetBrowserSystemFolderSearchFocusRoot(folderPath);") != std::string::npos &&
              mainCpp.find("SetBrowserSystemFolderSearchFocusRoot(node.path);") == std::string::npos,
              "browser normal-tree clicks set search scope, but search-result clicks keep the existing result set visible");
        CHECK(mainCpp.find("ImGuiTreeNodeFlags_NoTreePushOnOpen") != std::string::npos &&
              mainCpp.find("ImGui::Indent();") != std::string::npos &&
              mainCpp.find("ImGui::Unindent();") != std::string::npos,
              "folder trees use NoTreePushOnOpen with manual indentation instead of fragile TreePop pairing");
        CHECK(mainCpp.find("ShowBrowserCurrentPathContextMenu(state, currentPath, canNavigateUp);") != std::string::npos,
              "browser current-path actions are mounted from the path context menu instead of a persistent button strip");
        CHECK(mainCpp.find("BeginPopupContextItem(\"##BrowserCurrentPathCtx\")") != std::string::npos,
              "browser path field exposes a dedicated right-click context menu");
        CHECK(mainCpp.find("PrimeBrowserSystemFolderPathChain(") != std::string::npos &&
              mainCpp.find("(void)GetBrowserSystemFolderChildren(part.first);") != std::string::npos,
              "browser locate primes the current path ancestor chain before scrolling");
        CHECK(mainCpp.find("LocateBrowserSystemFolderPath(state.fileBrowser.getCurrentPath())") != std::string::npos,
              "browser locate-current button uses the robust path reveal helper");
        CHECK(mainCpp.find("g_browserSystemFolderSearchBuffer[0] = '\\0';") != std::string::npos &&
              mainCpp.find("LocateBrowserSystemFolderPath(state.fileBrowser.getCurrentPath())") != std::string::npos,
              "browser locate-current clears folder search so the current path is visible in the normal tree");
        CHECK(mainCpp.find("g_browserSystemFolderTreeCollapseAll = true;") != std::string::npos &&
              mainCpp.find("ClearBrowserSystemFolderSearchState();") != std::string::npos,
              "browser refresh mirrors DB refresh by clearing search state and collapsing the tree");
        CHECK(mainCpp.find("g_browserSystemFolderSearchBuffer[0] = '\\0';") != std::string::npos &&
              mainCpp.find("g_browserSystemFolderSearchFocusRoot.clear();") != std::string::npos,
              "browser refresh clears search text and clicked-path search scope like DB refresh clears folder search");
        CHECK(mainCpp.find("if (g_browserSystemFolderTreeCollapseAll)") != std::string::npos &&
              mainCpp.find("tw->DC.StateStorage->Clear();") != std::string::npos,
              "browser refresh clears ImGui tree storage so expanded nodes collapse like the database tree");
        CHECK(mainCpp.find("if (g_browserSystemFolderTreeCollapseAll) {") != std::string::npos &&
              mainCpp.find("ImGui::SetNextItemOpen(false);") != std::string::npos &&
              mainCpp.find("if (!g_browserSystemFolderTreeCollapseAll && currentPathMatches)") != std::string::npos,
              "browser refresh collapse frame suppresses current-path auto-open so refresh stays collapsed like DB tree");
        CHECK(mainCpp.find("RenderBrowserSystemFolderTree(state);") != std::string::npos &&
              mainCpp.find("g_browserSystemFolderTreeCollapseAll = false;") != std::string::npos,
              "browser refresh keeps collapse flag alive through tree rendering before clearing it");
        CHECK(mainCpp.find("LocateBrowserSystemFolderPath(currentBrowserPath)") == std::string::npos,
              "browser refresh does not locate the current Browser path; locate remains the separate target button");
        CHECK(mainCpp.find("L(Browser_AddFolderToLibrary)") != std::string::npos,
              "browser add-folder action uses localized Chinese-ready text");
        CHECK(mainCpp.find("L(Browser_SystemFolderTree)") == std::string::npos,
              "browser Filter no longer draws the extra System Folder Tree title");
        CHECK(mainCpp.find("ImGui::TextDisabled(\"%s\", browserSystemTreeLabel.c_str());") == std::string::npos,
              "browser folder tree no longer draws a System Folder Tree title above the list");
        CHECK(mainCpp.find("L(Nav_RelatedPaths)") != std::string::npos,
              "browser sidebar reuses the related-paths header for top result styling");
        CHECK(mainCpp.find("GetBrowserSystemFolderChildren(g_browserSystemFolderRelatedPath)") != std::string::npos &&
              mainCpp.find("##BrowserRelatedFolderCtx") != std::string::npos &&
              mainCpp.find("g_browserSystemFolderRelatedPath = folderPath;") != std::string::npos,
              "browser Related Paths pane shows child folders for the selected/opened system or search-tree folder");
        {
            const size_t paneFn = mainCpp.find("static void RenderRelatedPathsExplorerPane(");
            const size_t browserCall = mainCpp.find("\"##BrowserRelatedPathsBox\"");
            const size_t relatedChildren = mainCpp.find("GetBrowserSystemFolderChildren(g_browserSystemFolderRelatedPath)");
            CHECK(paneFn != std::string::npos &&
                  browserCall != std::string::npos &&
                  relatedChildren != std::string::npos &&
                  relatedChildren > browserCall,
                  "browser Related Paths children are rendered inside the Related Paths child box");
        }
        CHECK(mainCpp.find("L(Msg_Loading)") != std::string::npos,
              "browser uncapped folder search shows localized progressive loading status");
        CHECK(mainCpp.find("BrowserSystemFolderSearchVisitedCount()") != std::string::npos &&
              mainCpp.find("L(Main_FileCountsFmt)") != std::string::npos &&
              mainCpp.find("g_browserSystemFolderSearchRuntime.Busy()") != std::string::npos,
              "browser folder search exposes a persistent loading/result count indicator while results are incomplete");
        CHECK(mainCpp.find("items.push_back({\"browserScope\", std::to_string((int)state.fileBrowser.getScope())})") != std::string::npos,
              "browser scope persists into app settings");
        CHECK(mainCpp.find("tab.browserScope = (savedBrowserScope == (int)BrowserScope::RecursiveAudio)") != std::string::npos,
              "browser scope restores from saved tab state");
        CHECK(mainCpp.find("QueueBrowserFolderScan(state, folderTarget)") != std::string::npos,
              "browser row folder actions expose explicit Scan This Folder");
        CHECK(mainCpp.find("QueueBrowserFolderLibraryAdd(state, folderTarget)") != std::string::npos,
              "browser row folder actions expose explicit Add Folder to Library");
        CHECK(mainCpp.find("L(Browser_BookmarkFolder)") != std::string::npos,
              "browser path context menu exposes localized Bookmark Folder text");
        CHECK(mainCpp.find("L(Edit_RevealInExplorer)") != std::string::npos,
              "browser folder-action surfaces use localized Reveal in Explorer text");
        CHECK(mainCpp.find("label = entry.relativePath + \"/\" + displayName;") == std::string::npos,
              "browser filename column no longer prefixes recursive rows with relative folder paths");
        {
            const size_t dbArrow = mainCpp.find("Up/Down arrows - Navigate and play files in database view");
            const size_t dbArrowEnd = mainCpp.find("BPM Detection shortcuts", dbArrow);
            const std::string dbArrowBody =
                (dbArrow != std::string::npos && dbArrowEnd != std::string::npos)
                    ? mainCpp.substr(dbArrow, dbArrowEnd - dbArrow)
                    : std::string();
            CHECK(dbArrowBody.find("state.viewMode != ViewMode::Browser") != std::string::npos,
                  "database list still owns the existing up/down selection path");
            const size_t browserArrow = mainCpp.find("static void MoveBrowserAudioListSelectionByArrow");
            CHECK(browserArrow != std::string::npos,
                  "browser audio list has a dedicated up/down selection helper instead of remaining mouse-only");
            const size_t browserArrowEnd = mainCpp.find("\nstatic ", browserArrow + 1);
            const std::string browserArrowBody =
                (browserArrow != std::string::npos && browserArrowEnd != std::string::npos)
                    ? mainCpp.substr(browserArrow, browserArrowEnd - browserArrow)
                    : std::string();
            CHECK(browserArrowBody.find("filteredRows") != std::string::npos &&
                  browserArrowBody.find("BeginBrowserPathAudition") != std::string::npos &&
                  browserArrowBody.find("entry.isDirectory") != std::string::npos,
                  "browser up/down walks the visible audio-list order, skips directory rows, and auditions the selected file");
            CHECK(mainCpp.find("MoveBrowserAudioListSelectionByArrow(state, entries, filteredRows, -1)") != std::string::npos &&
                  mainCpp.find("MoveBrowserAudioListSelectionByArrow(state, entries, filteredRows, 1)") != std::string::npos,
                  "browser view wires Up/Down into the shared audio-list selection helper");
        }
        {
            const size_t browserMenu = mainCpp.find("if (ImGui::BeginPopupContextItem(\"##BrowserRowCtx\"))");
            const size_t browserMenuEnd = mainCpp.find("ImGui::EndPopup();", browserMenu);
            const std::string browserMenuBody =
                (browserMenu != std::string::npos && browserMenuEnd != std::string::npos)
                    ? mainCpp.substr(browserMenu, browserMenuEnd - browserMenu)
                    : std::string();
            CHECK(!browserMenuBody.empty(), "browser audio-row context menu remains mounted on the file list");
            CHECK(browserMenuBody.find("L(Edit_EditFilename)") != std::string::npos,
                  "browser audio-row menu exposes rename using the same filename-edit action as the database list");
            CHECK(browserMenuBody.find("L(Ctx_DeleteFromFS)") != std::string::npos,
                  "browser audio-row menu exposes filesystem delete with the same confirmation path as the database list");
            CHECK(browserMenuBody.find("Ctx_AddFavorite") == std::string::npos,
                  "browser audio-row menu no longer exposes the retired favorite action");
            CHECK(browserMenuBody.find("canFavorite") == std::string::npos &&
                  browserMenuBody.find("favLabel") == std::string::npos,
                  "browser audio-row menu does not keep leftover favorite enablement state");
            CHECK(browserMenuBody.find("L(Edit_RevealInExplorer)") != std::string::npos,
                  "browser audio-row menu uses the same reveal-in-explorer action as the database list");
            CHECK(browserMenuBody.find("L(Ctx_OpenFolder)") == std::string::npos,
                  "browser audio-row menu no longer uses a different open-folder label than the database list");
            CHECK(browserMenuBody.find("GetShortcutString(ShortcutAction::RevealInExplorer)") != std::string::npos,
                  "browser reveal item shows the same shortcut hint as the database list");
            CHECK(browserMenuBody.find("L(Ctx_CopyFilePath)") != std::string::npos,
                  "browser audio-row menu uses the same copy-file-path action as the database list");
            CHECK(browserMenuBody.find("Ctrl+Alt+C") != std::string::npos,
                  "browser copy-path shows the same shortcut hint as the database list");
            CHECK(browserMenuBody.find("L(Ctx_CopyFullPath)") == std::string::npos,
                  "browser audio-row menu does not keep a second inconsistent copy-path item");
        }
        {
            const size_t copyShortcut = mainCpp.find("// Ctrl+Alt+C = copy file path(s) to clipboard");
            const size_t copyShortcutEnd = mainCpp.find("// Ctrl+Shift+C = copy cover", copyShortcut);
            const std::string copyShortcutBody =
                (copyShortcut != std::string::npos && copyShortcutEnd != std::string::npos)
                    ? mainCpp.substr(copyShortcut, copyShortcutEnd - copyShortcut)
                    : std::string();
            CHECK(!copyShortcutBody.empty(), "global Ctrl+Alt+C copy-path shortcut remains mounted");
            CHECK(copyShortcutBody.find("state.viewMode == ViewMode::Browser") != std::string::npos,
                  "Ctrl+Alt+C copy-path is available in Browser, not only Database/Favorites/Playlist");
            CHECK(copyShortcutBody.find("BuildBrowserSelectedAudioPaths") != std::string::npos,
                  "Browser Ctrl+Alt+C copies selected browser audio paths instead of database searchResults");
            const size_t helper = mainCpp.find("static std::string BuildBrowserSelectedAudioPaths(AppState& state) {");
            const size_t helperEnd = mainCpp.find("\nstatic ", helper + 1);
            const std::string helperBody =
                (helper != std::string::npos && helperEnd != std::string::npos)
                    ? mainCpp.substr(helper, helperEnd - helper)
                    : std::string();
            CHECK(helperBody.find("browserSelectedIndicesSet") != std::string::npos &&
                  helperBody.find("activeEntries()") != std::string::npos,
                  "Browser copy-path helper reads selected browser audio entries");
        }
        CHECK(mainCpp.find("RQ-20260817-01 other-differences-audit") != std::string::npos &&
              mainCpp.find("keyboard-selection-parity") != std::string::npos &&
              mainCpp.find("本卡不实现，需产品治理总管另开卡") != std::string::npos,
              "candidate contains a structured Browser/Database list-parity difference ledger");
    }

    const std::string fileBrowserHeader = readTextFile("src/FileBrowser.h");
    CHECK(!fileBrowserHeader.empty(), "source contract can read FileBrowser.h");
    if (!fileBrowserHeader.empty()) {
        CHECK(fileBrowserHeader.find("activeAudioFileCount()") != std::string::npos,
              "FileBrowser exposes cached active audio count");
    }

    const std::string appStateCpp = readTextFile("src/AppState.cpp");
    CHECK(!appStateCpp.empty(), "source contract can read AppState.cpp");
    if (!appStateCpp.empty()) {
        CHECK(appStateCpp.find("fileBrowser.setScope(static_cast<BrowserScope>(tab.browserScope));") != std::string::npos,
              "browser scope restores when switching back to a browser tab");
        CHECK(appStateCpp.find("tab.browserScope = (int)fileBrowser.getScope();") != std::string::npos,
              "browser scope is synced into the active tab snapshot");
        CHECK(appStateCpp.find("snap.browserScope = (int)fileBrowser.getScope();") != std::string::npos,
              "browser scope participates in view-state persistence");
    }

    const std::string databaseHeader = readTextFile("src/Database.h");
    CHECK(!databaseHeader.empty(), "source contract can read Database.h");
    if (!databaseHeader.empty()) {
        CHECK(databaseHeader.find("getFilesByExactFolder") != std::string::npos,
              "Database exposes an exact-folder query for Browser metadata preload");
    }

    const std::string appStateHeader = readTextFile("src/AppState.h");
    CHECK(!appStateHeader.empty(), "source contract can read AppState.h");
    if (!appStateHeader.empty()) {
        CHECK(appStateHeader.find("std::unordered_map<std::string, AudioRecord> browserMetaCache") != std::string::npos &&
              appStateHeader.find("struct BrowserMetaMini") == std::string::npos,
              "Browser metadata cache reuses the complete database AudioRecord model");
    }
    CHECK(mainCpp.find("state.browserMetaCache[r.filePath] = std::move(r);") != std::string::npos,
          "Browser metadata preload preserves every database field without a second projection model");
    CHECK(mainCpp.find("const std::string& mood = metaIt->second.getMeta(\"mood\")") != std::string::npos &&
          mainCpp.find("metaIt->second.dateAdded > 0") != std::string::npos,
          "Browser rows render database Mood and Date Added instead of permanent placeholders");

    CHECK(databaseHeader.find("readAudioRecordMetadataSnapshot") != std::string::npos,
          "Browser and database scans share one AudioRecord metadata extraction contract");
    CHECK(mainCpp.find("std::future<BrowserMetadataLoadResult>") != std::string::npos &&
          mainCpp.find("Database::readAudioRecordMetadataSnapshot") != std::string::npos,
          "unindexed Browser metadata is loaded asynchronously instead of blocking the UI thread");
    CHECK(appStateHeader.find("BrowserMetadataLoadState") != std::string::npos &&
          mainCpp.find("DrawBrowserMetadataFallback") != std::string::npos &&
          mainCpp.find("BrowserMetadataLoadState::Loading") != std::string::npos,
          "Browser distinguishes metadata loading from unavailable values");

    const std::string databaseCpp = readTextFile("src/Database.cpp");
    CHECK(!databaseCpp.empty(), "source contract can read Database.cpp");
    if (!databaseCpp.empty()) {
        const size_t exactFolderStart = databaseCpp.find(
            "std::vector<AudioRecord> Database::getFilesByExactFolder");
        const size_t exactFolderEnd = exactFolderStart == std::string::npos
            ? std::string::npos
            : databaseCpp.find("std::vector<AudioRecord> Database::getFilesByIds", exactFolderStart);
        const std::string exactFolderBody =
            exactFolderStart == std::string::npos || exactFolderEnd == std::string::npos
                ? std::string()
                : databaseCpp.substr(exactFolderStart, exactFolderEnd - exactFolderStart);
        CHECK(exactFolderBody.find("WHERE folder_path = ?") != std::string::npos,
              "exact-folder Browser metadata preload stays bound to the current folder only");
        CHECK(exactFolderBody.find("HarkenSearch::CanonicalFolderPath(folderPath)") != std::string::npos &&
              exactFolderBody.find("sqlite3_bind_text(stmt, 1, canonicalFolderPath.c_str()") != std::string::npos,
              "exact-folder Browser metadata preload uses the same canonical folder identity as database writes");
    }

    std::cout << "\n" << (totalChecks - failed) << "/" << totalChecks
              << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
