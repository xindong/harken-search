#include "CoverWallPageResultBuilder.h"

#include "CoverCachedKeyItemBuilder.h"
#include "PathUtils.h"

#include <unordered_set>
#include <unordered_map>
#include <algorithm>

namespace aura::cover_wall {

namespace {

std::string BaseNameFromUtf8Path(const std::string& path) {
    if (path.empty()) return {};
    const size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

}  // namespace

CoverWallPageLoadResult LoadGlobalCoverWallPage(const std::string& dbPath,
                                                const std::string& afterCursor,
                                                int limit) {
    Database localDb;
    if (!localDb.openReadOnly(dbPath)) {
        return {};
    }
    auto page = localDb.getGlobalCoverWallEntriesPage(afterCursor, limit, true);
    // Old libraries can mark a folder as covered without storing the artwork
    // key. Several child folders may then display the very same parent image.
    // Resolve those placeholders on this background worker before deduplication.
    for (auto& entry : page.entries) {
        if (entry.isEmbedded || entry.key.rfind("?|", 0) != 0 || !entry.sourcePath.empty()) continue;
        std::string coverPath;
        if (CoverModule::GetFolderCoverImagePathBackground(entry.path, coverPath, true, true)) {
            entry.sourcePath = std::move(coverPath);
        }
    }
    return BuildCoverWallPageLoadResult(page);
}

CoverWallPageLoadResult BuildCoverWallPageFromCurrentFileList(
    const std::vector<AudioRecord>& records) {
    CoverWallPageLoadResult result;
    if (records.empty()) return result;
    BuildCoverWallItemsFromCachedKeys(records, result.items);
    result.hasMore = false;
    result.fromRegistry = false;
    result.largeLibrary = false;
    return result;
}

CoverModule::CoverItem ConvertCoverWallEntryToItem(const Database::CoverWallEntry& entry) {
    CoverModule::CoverItem item;
    item.key = entry.key;
    item.kind = entry.isEmbedded ? CoverModule::CoverKind::EmbeddedCover
                                 : CoverModule::CoverKind::FolderCover;
    if (entry.isEmbedded) {
        item.filePathUtf8 = entry.path;
        if (entry.sourcePath.rfind("PHASH|", 0) == 0) {
            item.folderCoverSrcPathUtf8 = entry.sourcePath;
        }
        item.label = BaseNameFromUtf8Path(item.filePathUtf8);
    } else {
        item.folderPathUtf8 = entry.path;
        item.folderCoverSrcPathUtf8 = entry.sourcePath;
        item.label = BaseNameFromUtf8Path(item.folderPathUtf8);
    }
    item.wallMemberKeys.push_back(item.key);
    return item;
}

std::string BuildCoverWallVisualDedupeToken(const CoverModule::CoverItem& item) {
    if (item.kind == CoverModule::CoverKind::EmbeddedCover &&
        item.folderCoverSrcPathUtf8.rfind("PHASH|", 0) == 0) {
        return item.folderCoverSrcPathUtf8;
    }
    if (item.kind == CoverModule::CoverKind::FolderCover &&
        !item.folderCoverSrcPathUtf8.empty() &&
        item.folderCoverSrcPathUtf8.rfind("PHASH|", 0) != 0) {
        return "F|SRC|" + HarkenSearch::CanonicalFolderPath(item.folderCoverSrcPathUtf8);
    }
    return item.key;
}

void AppendUniqueCoverWallPageItems(std::vector<CoverModule::CoverItem>& existing,
                                    std::vector<CoverModule::CoverItem>& nextPage) {
    std::unordered_map<std::string, size_t> byKey;
    std::unordered_map<std::string, size_t> byToken;
    byKey.reserve(existing.size() + nextPage.size());
    byToken.reserve(existing.size() + nextPage.size());
    std::vector<bool> removed(existing.size(), false);
    auto indexItem = [&](size_t index) {
        auto& item = existing[index];
        if (item.wallMemberKeys.empty()) item.wallMemberKeys.push_back(item.key);
        if (item.wallVisualTokens.empty())
            item.wallVisualTokens.push_back(BuildCoverWallVisualDedupeToken(item));
        for (const auto& key : item.wallMemberKeys) byKey[key] = index;
        for (const auto& token : item.wallVisualTokens) byToken[token] = index;
    };
    for (size_t i = 0; i < existing.size(); ++i) indexItem(i);
    for (auto& item : nextPage) {
        const std::string token = BuildCoverWallVisualDedupeToken(item);
        if (item.key.empty() || token.empty()) continue;
        if (item.wallMemberKeys.empty()) item.wallMemberKeys.push_back(item.key);
        if (item.wallVisualTokens.empty()) item.wallVisualTokens.push_back(token);
        std::unordered_set<size_t> matches;
        for (const auto& key : item.wallMemberKeys) {
            const auto it = byKey.find(key);
            if (it != byKey.end()) matches.insert(it->second);
        }
        for (const auto& visual : item.wallVisualTokens) {
            const auto it = byToken.find(visual);
            if (it != byToken.end()) matches.insert(it->second);
        }
        if (matches.empty()) {
            existing.push_back(std::move(item));
            removed.push_back(false);
            indexItem(existing.size() - 1);
            continue;
        }
        const size_t target = *std::min_element(matches.begin(), matches.end());
        for (const size_t other : matches) {
            if (other == target || removed[other]) continue;
            auto& group = existing[target];
            auto& merged = existing[other];
            group.wallMemberKeys.insert(group.wallMemberKeys.end(),
                                        merged.wallMemberKeys.begin(), merged.wallMemberKeys.end());
            group.wallVisualTokens.insert(group.wallVisualTokens.end(),
                                          merged.wallVisualTokens.begin(), merged.wallVisualTokens.end());
            removed[other] = true;
        }
        auto& group = existing[target];
        for (const auto& key : item.wallMemberKeys) {
            if (byKey.find(key) == byKey.end()) group.wallMemberKeys.push_back(key);
        }
        for (const auto& visual : item.wallVisualTokens) {
            if (byToken.find(visual) == byToken.end()) group.wallVisualTokens.push_back(visual);
        }
        indexItem(target);
    }
    if (std::find(removed.begin(), removed.end(), true) != removed.end()) {
        size_t write = 0;
        for (size_t read = 0; read < existing.size(); ++read) {
            if (!removed[read]) {
                if (write != read) existing[write] = std::move(existing[read]);
                ++write;
            }
        }
        existing.resize(write);
    }
}

CoverWallPageLoadResult BuildCoverWallPageLoadResult(const Database::CoverWallPage& page) {
    CoverWallPageLoadResult result;
    result.nextCursor = page.nextCursor;
    result.hasMore = page.hasMore;
    result.fromRegistry = page.fromRegistry;
    result.largeLibrary = page.largeLibrary;
    result.registryEntries = page.registryEntries;
    result.items.reserve(page.entries.size());
    std::vector<CoverModule::CoverItem> converted;
    converted.reserve(page.entries.size());
    for (const auto& entry : page.entries) {
        if (entry.key.empty() || entry.path.empty()) continue;
        converted.push_back(ConvertCoverWallEntryToItem(entry));
    }
    AppendUniqueCoverWallPageItems(result.items, converted);
    SanitizeCoverWallItemsForCache(result.items);
    return result;
}

CoverWallPageLoadResult BuildCoverWallLoadResultFromEntries(
    const std::vector<Database::CoverWallEntry>& entries) {
    Database::CoverWallPage page;
    page.entries = entries;
    return BuildCoverWallPageLoadResult(page);
}

}  // namespace aura::cover_wall
