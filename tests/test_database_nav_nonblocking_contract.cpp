// Source contract for large-library top-nav database activation.
// Browser/playlist -> database must not submit a rows-first search and then
// immediately run a million-row count follow-up on the same worker request.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::cout << "PASS: " << msg << "\n"; } \
    else { std::cerr << "FAIL: " << msg << "\n"; ++failed; } \
} while (0)

static std::string readTextFile(const std::filesystem::path& relativePath) {
    const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream in(repoRoot / relativePath, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int main() {
    const std::string mainCpp = readTextFile("src/main.cpp");
    const std::string appState = readTextFile("src/AppState.h");
    const std::string asyncHeader = readTextFile("src/AsyncWorker.h");
    const std::string asyncWorker = readTextFile("src/AsyncWorker.cpp");
    const std::string countRepairAdapter = readTextFile("src/AsyncSearchRefreshCountRepairStateAdapter.cpp");

    CHECK(!mainCpp.empty(), "can read main.cpp");
    CHECK(!appState.empty(), "can read AppState.h");
    CHECK(!asyncHeader.empty(), "can read AsyncWorker.h");
    CHECK(!asyncWorker.empty(), "can read AsyncWorker.cpp");

    CHECK(mainCpp.find("state.skipDeferredCountFollowUpForNextRefresh = enteringDatabaseFromPeerView;") != std::string::npos,
          "database top-nav sets skip-count flag only when entering from browser/playlist peer views");
    CHECK(mainCpp.find("state.viewMode == ViewMode::Browser ||") != std::string::npos &&
          mainCpp.find("IsPlaylistSystemView(state.viewMode)") != std::string::npos,
          "skip-count source views are Browser and playlist system views");
    CHECK(mainCpp.find("skipDeferredCountFollowUpForThisRefresh") != std::string::npos,
          "skip-count flag is consumed as a one-shot refresh property");
    CHECK(appState.find("skipDeferredCountFollowUpForNextRefresh") != std::string::npos,
          "AppState carries a one-shot skip-count flag");
    CHECK(asyncHeader.find("skipDeferredCountFollowUp") != std::string::npos,
          "AsyncSearchRequest carries skipDeferredCountFollowUp");
    CHECK(asyncHeader.find("countFollowUpSkipped") != std::string::npos,
          "AsyncSearchResult reports skipped count follow-up to UI");
    CHECK(asyncWorker.find("SkipDeferredCountFollowUp") != std::string::npos,
          "SearchWorker has explicit diagnostic for skipped deferred count follow-up");
    CHECK(asyncWorker.find("req.deferCount && req.skipDeferredCountFollowUp && !req.needFacets") != std::string::npos,
          "SearchWorker skips only deferred count follow-up without suppressing required facets");
    CHECK(mainCpp.find("req.needFacets || (req.deferCount && !req.skipDeferredCountFollowUp)") != std::string::npos,
          "UI does not wait for a count follow-up when the request explicitly skipped it");
    CHECK(countRepairAdapter.find("result.totalCount == -1 && !result.countFollowUpSkipped") != std::string::npos,
          "UI does not leave pendingCountFollowUp stuck when count follow-up was skipped");

    CHECK(mainCpp.find("return layoutTinyWidth || !item.isModeBtn;") != std::string::npos,
          "Scan and Refresh are icon-only even at full toolbar width");
    CHECK(mainCpp.find("toolbarFlowInput.expandedItemWidths[i] = navItemIconOnly(navItems[i])") != std::string::npos &&
          mainCpp.find("navAllIconOnly || navItemIconOnly(navItems[i])") != std::string::npos,
          "toolbar measurement and drawing share the icon-only rule");
    CHECK(mainCpp.find(R"(HarkenSearch::SetTooltip("%s", navItems[i].label);)") != std::string::npos,
          "icon-only navigation actions retain their localized hover labels");
    CHECK(mainCpp.find("toolbarResultsRightScreenX - searchCursorScreenX") != std::string::npos,
          "search sizing accounts for the actual Results edge before reserving compact navigation");
    CHECK(mainCpp.find("searchAvailableW, searchMinW, searchFitMaxW") != std::string::npos,
          "navigation pressure never reduces the search input below its existing minimum");
    CHECK(mainCpp.find("toolbarFlowInput.minimumVisibleItems = navCount;") != std::string::npos,
          "toolbar overflow never retains Scan while dropping Refresh");
    CHECK(appState.find("Favorites = 2,") != std::string::npos &&
          appState.find("case 2: return ViewMode::Database;") != std::string::npos,
          "retired Favorites retains its persisted numeric hole and Database fallback");

    const int totalChecks = 21;
    std::cout << "\n" << (totalChecks - failed) << "/" << totalChecks
              << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
