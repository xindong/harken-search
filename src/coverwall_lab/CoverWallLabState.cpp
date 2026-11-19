#include "CoverWallLabState.h"
#include "platform/filesystem.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace aura::coverwall_lab {
namespace {

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

std::string fallbackString(const std::filesystem::path& path) {
    return path.u8string();
}

} // namespace

CoverWallLabState::CoverWallLabState() {
    filters_.tileSizePx = kDefaultTileSizePx;
    loadFixture();
}

void CoverWallLabState::loadFixture() {
    sourceLabel_ = "Fixture library";
    allItems_ = {
        {"cover:foley-room", "Footstep gravel close", "Fixture/Foley/Footsteps", "Fixture/Foley/Footsteps/gravel_close.wav", "", 1},
        {"cover:foley-room", "Footstep wood porch", "Fixture/Foley/Footsteps", "Fixture/Foley/Footsteps/wood_porch.wav", "", 1},
        {"cover:impact-metal", "Metal impact bright", "Fixture/Impacts/Metal", "Fixture/Impacts/Metal/bright_hit.wav", "", 1},
        {"cover:impact-metal", "Metal scrape long", "Fixture/Impacts/Metal", "Fixture/Impacts/Metal/scrape_long.wav", "", 1},
        {"cover:wind-night", "Night wind soft loop", "Fixture/Ambience/Wind", "Fixture/Ambience/Wind/night_soft_loop.wav", "", 1},
        {"cover:water-small", "Water drip ceramic", "Fixture/Water/Small", "Fixture/Water/Small/drip_ceramic.wav", "", 1},
        {"cover:whoosh-ui", "UI whoosh airy", "Fixture/Designed/Whooshes", "Fixture/Designed/Whooshes/ui_whoosh_airy.wav", "", 1},
        {"cover:servo-small", "Small servo open", "Fixture/Machines/Servo", "Fixture/Machines/Servo/small_servo_open.wav", "", 1},
    };
    rebuildHitCounts();
    rebuildFacets();
    rebuildResults();
    results_.isFromFixture = true;
}

bool CoverWallLabState::importFolder(const std::filesystem::path& root, std::string* errorMessage) {
    const std::string rootPath = root.u8string();
    if (!Platform::FileSystem::Exists(rootPath) ||
        !Platform::FileSystem::IsDirectory(rootPath)) {
        if (errorMessage) {
            *errorMessage = "Folder does not exist or is not a directory.";
        }
        return false;
    }

    int visitedFiles = 0;
    std::vector<LabCoverItem> imported;
    std::map<std::string, std::string> coverByFolder;
    Platform::FileSystem::DirectoryIterator iterator;
    bool iteratorFailed = !iterator.Open(rootPath, true, true);
    while (!iteratorFailed) {
        Platform::FileSystem::DirectoryEntry entry;
        std::string iteratorError;
        const auto status = iterator.Next(entry, &iteratorError);
        if (status == Platform::FileSystem::DirectoryReadStatus::End) break;
        if (status == Platform::FileSystem::DirectoryReadStatus::Error) {
            iteratorFailed = true;
            break;
        }
        if (!entry.isRegularFile) continue;
        if (++visitedFiles > kMaxImportVisitedFiles) {
            break;
        }

        const std::filesystem::path path = std::filesystem::u8path(entry.pathUtf8);
        const std::string folder = toUtf8Path(path.parent_path());
        if (isCoverImageFile(path)) {
            if (coverByFolder.find(folder) == coverByFolder.end()) {
                coverByFolder[folder] = toUtf8Path(path);
            }
            continue;
        }

        if (!isAudioFile(path)) {
            continue;
        }
        const auto coverIt = coverByFolder.find(folder);
        const std::string coverSource = coverIt == coverByFolder.end() ? std::string() : coverIt->second;
        LabCoverItem item;
        item.coverKey = coverSource.empty() ? ("folder:" + folder) : ("image:" + coverSource);
        item.displayLabel = path.stem().u8string();
        item.folderPath = folder;
        item.representativeFilePath = toUtf8Path(path);
        item.coverSourcePath = coverSource;
        imported.push_back(std::move(item));
        if (static_cast<int>(imported.size()) >= kMaxImportAudioFiles) {
            break;
        }
    }

    if (iteratorFailed) {
        if (errorMessage) {
            *errorMessage = "Import stopped because the folder could not be read completely.";
        }
        return false;
    }

    for (LabCoverItem& item : imported) {
        const auto coverIt = coverByFolder.find(item.folderPath);
        if (coverIt != coverByFolder.end()) {
            item.coverSourcePath = coverIt->second;
            item.coverKey = "image:" + item.coverSourcePath;
        }
    }

    if (imported.empty()) {
        if (errorMessage) {
            *errorMessage = "No supported audio files were found.";
        }
        return false;
    }

    allItems_ = std::move(imported);
    sourceLabel_ = toUtf8Path(root);
    filters_.selectedPath.clear();
    filters_.selectedCoverKey.clear();
    rebuildHitCounts();
    rebuildFacets();
    rebuildResults();
    results_.isFromFixture = false;
    return true;
}

void CoverWallLabState::setKeyword(std::string keyword) {
    filters_.keyword = std::move(keyword);
    rebuildResults();
}

void CoverWallLabState::selectPath(std::string path) {
    filters_.selectedPath = std::move(path);
    rebuildResults();
}

void CoverWallLabState::selectCover(std::string coverKey) {
    filters_.selectedCoverKey = std::move(coverKey);
    rebuildResults();
}

void CoverWallLabState::clearKeyword() {
    filters_.keyword.clear();
    rebuildResults();
}

void CoverWallLabState::clearPath() {
    filters_.selectedPath.clear();
    rebuildResults();
}

void CoverWallLabState::clearCover() {
    filters_.selectedCoverKey.clear();
    rebuildResults();
}

void CoverWallLabState::clearAllFilters() {
    filters_.keyword.clear();
    filters_.selectedPath.clear();
    filters_.selectedCoverKey.clear();
    rebuildResults();
}

int CoverWallLabState::setTileSizePx(int requestedPx) {
    filters_.tileSizePx = aura::coverwall::ClampTileSizePx(
        requestedPx,
        kMinTileSizePx,
        kMaxTileSizePx);
    return filters_.tileSizePx;
}

int CoverWallLabState::applyPreset(TileSizePreset preset) {
    return aura::coverwall::ApplyTileSizePreset(
        preset,
        filters_.tileSizePx,
        kMinTileSizePx,
        kMaxTileSizePx);
}

int CoverWallLabState::calculateColumnCount(float availableWidth, float gapPx) const {
    const float tile = static_cast<float>(std::max(kMinTileSizePx, filters_.tileSizePx));
    const float safeWidth = std::max(1.0f, availableWidth);
    const float safeGap = std::max(0.0f, gapPx);
    const int columns = static_cast<int>((safeWidth + safeGap) / (tile + safeGap));
    return std::max(1, std::min(12, columns));
}

LabVisibleRange CoverWallLabState::calculateVisibleRange(int rowStart, int rowEnd, int columns, int totalItems) {
    const int safeColumns = std::max(1, columns);
    const int safeTotal = std::max(0, totalItems);
    const int safeRowStart = std::max(0, rowStart);
    const int safeRowEnd = std::max(safeRowStart, rowEnd);
    LabVisibleRange range;
    range.startIndex = std::min(safeTotal, safeRowStart * safeColumns);
    range.endIndex = std::min(safeTotal, safeRowEnd * safeColumns);
    return range;
}

bool CoverWallLabState::saveSettings(const std::filesystem::path& path) const {
    const std::string content = "tile_size=" + std::to_string(filters_.tileSizePx) + "\n";
    return Platform::FileSystem::WriteFileText(path.u8string(), content);
}

bool CoverWallLabState::loadSettings(const std::filesystem::path& path) {
    std::string content;
    if (!Platform::FileSystem::ReadFileText(path.u8string(), content)) {
        return false;
    }
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        constexpr char kPrefix[] = "tile_size=";
        if (line.rfind(kPrefix, 0) == 0) {
            const int parsed = std::atoi(line.c_str() + sizeof(kPrefix) - 1);
            setTileSizePx(parsed);
            return true;
        }
    }
    return false;
}

std::string CoverWallLabState::normalizeForSearch(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

bool CoverWallLabState::containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return normalizeForSearch(haystack).find(normalizeForSearch(needle)) != std::string::npos;
}

bool CoverWallLabState::pathMatches(const std::string& itemPath, const std::string& selectedPath) {
    if (selectedPath.empty()) {
        return true;
    }
    return itemPath == selectedPath || itemPath.rfind(selectedPath + "/", 0) == 0 || itemPath.rfind(selectedPath + "\\", 0) == 0;
}

bool CoverWallLabState::isAudioFile(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg" ||
           ext == ".aif" || ext == ".aiff" || ext == ".wma" || ext == ".m4a" || ext == ".wv";
}

bool CoverWallLabState::isCoverImageFile(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    if (ext != ".jpg" && ext != ".jpeg" && ext != ".png" && ext != ".bmp") {
        return false;
    }
    const std::string stem = normalizeForSearch(path.stem().string());
    return stem == "cover" || stem == "folder" || stem.find("artwork") != std::string::npos;
}

std::string CoverWallLabState::toUtf8Path(const std::filesystem::path& path) {
    return fallbackString(path);
}

void CoverWallLabState::rebuildFacets() {
    pathFacets_.clear();
    for (const LabCoverItem& item : allItems_) {
        if (std::find(pathFacets_.begin(), pathFacets_.end(), item.folderPath) == pathFacets_.end()) {
            pathFacets_.push_back(item.folderPath);
        }
    }
    std::sort(pathFacets_.begin(), pathFacets_.end());
}

void CoverWallLabState::rebuildResults() {
    results_.items.clear();
    for (const LabCoverItem& item : allItems_) {
        const bool keywordMatch = containsInsensitive(item.displayLabel, filters_.keyword) ||
                                  containsInsensitive(item.folderPath, filters_.keyword) ||
                                  containsInsensitive(item.representativeFilePath, filters_.keyword);
        if (!keywordMatch) {
            continue;
        }
        if (!pathMatches(item.folderPath, filters_.selectedPath)) {
            continue;
        }
        if (!filters_.selectedCoverKey.empty() && item.coverKey != filters_.selectedCoverKey) {
            continue;
        }
        results_.items.push_back(item);
    }
    results_.totalCount = static_cast<std::int64_t>(results_.items.size());
    results_.isStaleWhileRefreshing = false;
}

void CoverWallLabState::rebuildHitCounts() {
    std::map<std::string, int> counts;
    for (const LabCoverItem& item : allItems_) {
        ++counts[item.coverKey];
    }
    for (LabCoverItem& item : allItems_) {
        item.hitCount = counts[item.coverKey];
    }
}

}
