#include "CoverWallPageResultBuilder.h"
#include "CoverWallSelectionKey.h"

#include <iostream>
#include <filesystem>

std::filesystem::path Utf8ToPath(const std::string& value) {
    return std::filesystem::u8path(value);
}
std::string PathToUtf8(const std::filesystem::path& value) {
    return value.u8string();
}

namespace {
Database::CoverWallPage nextPage;
}

Database::Database() = default;
Database::~Database() = default;
bool Database::openReadOnly(const std::string&) { return true; }
Database::CoverWallPage Database::getGlobalCoverWallEntriesPage(const std::string&, int, bool) {
    return nextPage;
}

namespace CoverModule {
bool GetFolderCoverImagePathBackground(const std::string& folder,
                                       std::string& out,
                                       bool,
                                       bool) {
    if (folder.find("repeat") != std::string::npos) {
        out = "C:/covers/shared.jpg";
    } else if (folder.find("other") != std::string::npos) {
        out = "C:/covers/other.jpg";
    } else {
        out.clear();
    }
    return !out.empty();
}
}

namespace aura::cover_wall {
void SanitizeCoverWallItemsForCache(std::vector<CoverModule::CoverItem>&) {}
void BuildCoverWallItemsFromCachedKeys(const std::vector<AudioRecord>&,
                                       std::vector<CoverModule::CoverItem>&) {}
}

int main() {
    nextPage.entries = {
        {"?|C:/repeat/a", "C:/repeat/a", "", false},
        {"?|C:/repeat/b", "C:/repeat/b", "", false},
        {"?|C:/other", "C:/other", "", false},
    };
    nextPage.hasMore = true;
    nextPage.nextCursor = "fl2:123";
    const auto first = aura::cover_wall::LoadGlobalCoverWallPage("isolated.db", {}, 512);
    const bool onePerSource = first.items.size() == 2 && first.hasMore &&
                              first.nextCursor == "fl2:123" &&
                              first.items[0].key == "?|C:/repeat/a" &&
                              first.items[0].folderCoverSrcPathUtf8 == "C:/covers/shared.jpg" &&
                              first.items[0].wallMemberKeys.size() == 2;

    nextPage.entries = {{"?|C:/repeat/c", "C:/repeat/c", "", false}};
    auto second = aura::cover_wall::LoadGlobalCoverWallPage("isolated.db", "fl2:123", 512);
    const bool sameAcrossPages = second.items.size() == 1 &&
        aura::cover_wall::BuildCoverWallVisualDedupeToken(first.items[0]) ==
        aura::cover_wall::BuildCoverWallVisualDedupeToken(second.items[0]);

    nextPage.entries = {
        {"F|H|legacy", "C:/repeat/legacy", "C:/covers/shared.jpg", false},
        {"?|C:/repeat/new", "C:/repeat/new", "", false},
    };
    const auto mixed = aura::cover_wall::LoadGlobalCoverWallPage("isolated.db", {}, 512);
    const bool mixedLegacyKeysCollapse = mixed.items.size() == 1;

    auto distinct = aura::cover_wall::BuildCoverWallPageLoadResult({{
        {"?|C:/similar/a", "C:/similar/a", "C:/covers/similar-a.jpg", false},
        {"?|C:/similar/b", "C:/similar/b", "C:/covers/similar-b.jpg", false},
    }});
    const bool distinctFilesRemain = distinct.items.size() == 2;
    auto sameKey = aura::cover_wall::BuildCoverWallPageLoadResult({{
        {"F|H|same|123", "C:/one", "C:/covers/one.jpg", false},
        {"F|H|same|123", "C:/two", "C:/covers/two.jpg", false},
    }});
    const bool sameKeyAcrossSourcesCollapses = sameKey.items.size() == 1;
    auto sameKeyNextPage = aura::cover_wall::BuildCoverWallPageLoadResult({{
        {"F|H|same|123", "C:/three", "C:/covers/three.jpg", false},
    }});
    aura::cover_wall::AppendUniqueCoverWallPageItems(sameKey.items, sameKeyNextPage.items);
    const bool sameKeyAcrossPagesCollapses = sameKey.items.size() == 1;
    auto bridged = aura::cover_wall::BuildCoverWallPageLoadResult({{
        {"F|H|bridge-a|123", "C:/a", "C:/covers/a.jpg", false},
        {"F|H|bridge-b|123", "C:/b", "C:/covers/b.jpg", false},
    }});
    auto bridgePage = aura::cover_wall::BuildCoverWallPageLoadResult({{
        {"F|H|bridge-a|123", "C:/c", "C:/covers/b.jpg", false},
    }});
    aura::cover_wall::AppendUniqueCoverWallPageItems(bridged.items, bridgePage.items);
    const bool transitiveBridgeCollapsed = bridged.items.size() == 1 &&
        bridged.items[0].key == "F|H|bridge-a|123" &&
        bridged.items[0].wallMemberKeys.size() == 2;
    auto appended = first.items;
    aura::cover_wall::AppendUniqueCoverWallPageItems(appended, second.items);
    const bool repeatedPageSkipped = appended.size() == 2 &&
        appended[0].wallMemberKeys.size() == 3;
    const std::string selection = aura::cover_wall::EncodeGroupedCoverWallKeys(
        appended[0].wallMemberKeys);
    std::vector<std::string> selectedKeys;
    const bool allFoldersSelectable =
        aura::cover_wall::DecodeCoverWallSelectionKey(selection, selectedKeys) &&
        selectedKeys.size() == 3 &&
        selectedKeys[0] == "?|C:/repeat/a" &&
        selectedKeys[1] == "?|C:/repeat/b" &&
        selectedKeys[2] == "?|C:/repeat/c";
    aura::cover_wall::AppendUniqueCoverWallPageItems(appended, distinct.items);
    const bool distinctPageAppended = appended.size() == 4;

    std::cout << "same source once=" << onePerSource
              << " across pages=" << sameAcrossPages
              << " mixed legacy keys=" << mixedLegacyKeysCollapse
              << " repeated page skipped=" << repeatedPageSkipped
              << " distinct page appended=" << distinctPageAppended
              << " distinct files=" << distinctFilesRemain << '\n';
    return onePerSource && sameAcrossPages && mixedLegacyKeysCollapse &&
           repeatedPageSkipped && allFoldersSelectable && distinctPageAppended && distinctFilesRemain &&
           sameKeyAcrossSourcesCollapses && sameKeyAcrossPagesCollapses &&
           transitiveBridgeCollapsed ? 0 : 1;
}
