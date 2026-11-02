#pragma once

#include <array>
#include <string>

namespace HarkenSearch {

enum class ScanFailureCategory { Path, Read, Empty, Save, Other };
using ScanFailureCategoryCounts = std::array<int, 5>;

inline ScanFailureCategory ClassifyScanFailure(const std::string& error) {
    const auto starts = [&](const char* prefix) { return error.rfind(prefix, 0) == 0; };
    if (starts("Scan_NetworkLibraryUnreachable") || starts("Cannot read directory") ||
        starts("Failed to scan directory") || starts("Path not found") || starts("File not found"))
        return ScanFailureCategory::Path;
    if (starts("No audio samples") || starts("No usable audio") || starts("Empty audio") || starts("Audio file too small to decode"))
        return ScanFailureCategory::Empty;
    if (starts("Failed to decode") || starts("Failed to read audio") || starts("Cannot decode") ||
        starts("Cannot read audio") || starts("Failed to open audio"))
        return ScanFailureCategory::Read;
    if (starts("Database write failed") || starts("Scan aborted after database write failure") ||
        starts("Failed to save") || starts("Failed to seed") ||
        starts("Failed to snapshot rollback") || starts("Scan cancelled but rollback failed"))
        return ScanFailureCategory::Save;
    return ScanFailureCategory::Other;
}

} // namespace HarkenSearch
