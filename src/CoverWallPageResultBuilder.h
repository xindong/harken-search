#pragma once

#include "AppState.h"
#include "Database.h"

#include <string>
#include <vector>

namespace aura::cover_wall {

CoverWallPageLoadResult LoadGlobalCoverWallPage(const std::string& dbPath,
                                                const std::string& afterCursor,
                                                int limit);
CoverWallPageLoadResult BuildCoverWallPageFromCurrentFileList(
    const std::vector<AudioRecord>& records);
CoverModule::CoverItem ConvertCoverWallEntryToItem(const Database::CoverWallEntry& entry);
std::string BuildCoverWallVisualDedupeToken(const CoverModule::CoverItem& item);
void AppendUniqueCoverWallPageItems(std::vector<CoverModule::CoverItem>& existing,
                                    std::vector<CoverModule::CoverItem>& nextPage);
CoverWallPageLoadResult BuildCoverWallPageLoadResult(const Database::CoverWallPage& page);
CoverWallPageLoadResult BuildCoverWallLoadResultFromEntries(
    const std::vector<Database::CoverWallEntry>& entries);

}  // namespace aura::cover_wall
