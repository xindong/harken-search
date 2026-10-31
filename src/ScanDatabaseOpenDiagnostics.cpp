#include "ScanDatabaseOpenDiagnostics.h"
namespace aura {
namespace {
std::string TrimForDiagnostic(const std::string& value, size_t limit) {
    if (value.size() <= limit) return value;
    size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
    if (end < value.size()) {
        const unsigned char lead = static_cast<unsigned char>(value[end]);
        size_t width = 1;
        if ((lead & 0x80) == 0) width = 1;
        else if ((lead & 0xE0) == 0xC0) width = 2;
        else if ((lead & 0xF0) == 0xE0) width = 3;
        else if ((lead & 0xF8) == 0xF0) width = 4;
        else width = 0;
        if (width > 0 && end + width <= limit) end += width;
    }
    return value.substr(0, end);
}
}
std::string FormatScanDatabaseOpenFailure(const std::string& databasePath, const std::string& error, const std::vector<std::string>& folders) {
    const std::string shownError = TrimForDiagnostic(error.empty() ? "open scan database failed" : error, 80);
    const std::string shownFolder = TrimForDiagnostic(folders.empty() ? "empty" : folders.front(), 60);
    const std::string prefix = "ScanDatabaseOpenFailed: error=" + shownError + " folder=" + shownFolder + " path=";
    const size_t pathLimit = prefix.size() >= 191 ? 0 : 191 - prefix.size();
    return prefix + TrimForDiagnostic(databasePath.empty() ? "empty" : databasePath, pathLimit);
}
}
