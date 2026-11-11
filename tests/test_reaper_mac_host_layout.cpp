#include "src/ReaperMacHostLayout.h"
#include "src/platform/harken_user_data_layout.h"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
int main() {
    using HarkenSearch::DawBridge::ResolveMacReaperHostLayout;
    int failures = 0;
    auto check = [&](bool condition, const char* label) {
        std::cout << (condition ? "PASS: " : "FAIL: ") << label << '\n';
        if (!condition) ++failures;
    };
#ifdef __APPLE__
    check(std::string(Platform::HarkenUserData::CanonicalReaperPluginFileName()) == "reaper_harken.dylib",
          "Mac extension lookup uses its real dylib filename");
    check(std::string(Platform::HarkenUserData::LegacyReaperPluginFileName()) == "reaper_auro.dylib",
          "Mac legacy plugin lookup stays on the native file type");
#else
    check(std::string(Platform::HarkenUserData::CanonicalReaperPluginFileName()) == "reaper_harken.dll",
          "Windows extension filename remains dll");
    check(fs::path(Platform::HarkenUserData::LegacyReaperPluginFileName()).extension() == ".dll",
          "Windows legacy filename remains dll");
#endif
    const auto parent = fs::temp_directory_path() / fs::u8path(u8"Harken QA 中文 space");
    const auto app = parent / "REAPER.app";
    const auto executable = app / "Contents" / "MacOS" / "REAPER";
    auto layout = ResolveMacReaperHostLayout(app.u8string());
    check(layout.bundle == app && layout.executable == executable,
          "selected app resolves its real Mac executable");
    layout = ResolveMacReaperHostLayout(executable.u8string());
    check(layout.bundle == app && layout.executable == executable,
          "selected inner executable resolves the same app");
    layout = ResolveMacReaperHostLayout((app / ".").u8string());
    check(layout.bundle == app, "dot and trailing separator preserve app identity");
    const auto renamed = parent / "REAPER QA.app";
    layout = ResolveMacReaperHostLayout(renamed.u8string());
    check(layout.bundle == renamed && layout.executable == renamed / "Contents/MacOS/REAPER",
          "renamed app keeps REAPER executable identity");
    for (const auto& path : {parent / "reaper.exe", parent / "REAPER",
                            parent / "Other.app/Contents/MacOS/Other",
                            parent / "REAPER.app/Resources/REAPER"}) {
        check(ResolveMacReaperHostLayout(path.u8string()).bundle.empty(),
              "unrecognized host layout is not accepted as a Mac REAPER app");
    }
    check(ResolveMacReaperHostLayout("").bundle.empty(), "empty selection stays unconfigured");
    check(ResolveMacReaperHostLayout("REAPER.app").bundle.empty(),
          "relative host path is rejected");
    check(ResolveMacReaperHostLayout(std::string("/REAPER.app\0tail", 16)).bundle.empty(),
          "embedded NUL cannot truncate configured host path");
    check(!fs::exists(parent), "layout resolution never creates a host or plugin directory");
    return failures == 0 ? 0 : 1;
}
