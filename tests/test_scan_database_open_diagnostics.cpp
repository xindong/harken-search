#include "ScanDatabaseOpenDiagnostics.h"
#include <iostream>
#include <string>
#include <vector>
static int g_failed = 0;
#define ASSERT(cond, msg) do { if (!(cond)) { std::cerr << "FAIL: " << (msg) << "\n"; ++g_failed; } else { std::cout << "PASS: " << (msg) << "\n"; } } while (0)
int main() {
    const std::string detail = aura::FormatScanDatabaseOpenFailure("", "open scan database failed", {"H:/Khron Studio"});
    ASSERT(detail.find("path=empty") != std::string::npos, "empty database path is visible");
    ASSERT(detail.find("error=open scan database failed") != std::string::npos, "failure text is visible");
    ASSERT(detail.find("folder=H:/Khron Studio") != std::string::npos, "scan folder is visible");
    const std::string longPath(180, 'D');
    const std::string longDetail = aura::FormatScanDatabaseOpenFailure(longPath, "open scan database failed", {"H:/Khron Studio"});
    ASSERT(longDetail.size() <= 191, "diagnostic detail stays within logger capacity");
    ASSERT(longDetail.find("error=open scan database failed") != std::string::npos, "long path keeps failure text");
    ASSERT(longDetail.find("folder=H:/Khron Studio") != std::string::npos, "long path keeps scan folder");
    const std::string chineseFolder = std::string("C:/a") + "音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频";
    const std::string chineseDetail = aura::FormatScanDatabaseOpenFailure("D:/音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频音频", "open scan database failed", {chineseFolder});
    ASSERT(chineseDetail.find("error=open scan database failed") != std::string::npos, "Chinese path keeps failure text");
    ASSERT(chineseDetail.find("folder=C:/a") != std::string::npos, "Chinese path keeps folder start");
    auto validUtf8 = [](const std::string& value) {
        for (size_t i = 0; i < value.size();) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            const size_t width = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (width == 0 || i + width > value.size()) return false;
            for (size_t n = 1; n < width; ++n) {
                if ((static_cast<unsigned char>(value[i + n]) >> 6) != 0x2) return false;
            }
            i += width;
        }
        return true;
    };
    ASSERT(validUtf8(chineseDetail), "Chinese path truncation keeps valid UTF-8");
    return g_failed == 0 ? 0 : 1;
}
