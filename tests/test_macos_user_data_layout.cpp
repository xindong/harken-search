#include "platform/harken_user_data_layout.h"
#include "platform/theme_custom_file.h"
#include "platform/database_paths.inl"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace data = Platform::HarkenUserData;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
int main() {
    try {
        const fs::path root(data::PreviewIsolationRootUtf8());
        require(data::PreviewIsolationActive() && root.is_absolute(), "explicit QA root is active");
        require(data::WideToUtf8Path(data::Utf8ToWidePath(u8"素材/音效😀")) == u8"素材/音效😀", "UTF-8 paths round trip");
        require(data::Utf8ToWidePath(std::string(1, char(0xff))).empty(), "invalid UTF-8 is rejected");
        require(!data::PreviewIsolationAllowsDataPathUtf8(""), "empty path is denied");
        require(!data::PreviewIsolationAllowsDataPathUtf8("relative/settings.ini"), "relative path is denied");
        require(!data::PreviewIsolationAllowsDataPathUtf8(root.string() + "-other/settings.ini"), "sibling prefix is denied");
        require(!data::PreviewIsolationAllowsDataPathUtf8((root / ".." / "outside.ini").string()), "parent escape is denied");
        require(data::PreviewIsolationAllowsDataPathUtf8((root / u8"中文目录" / "settings.ini").string()), "new child path is allowed");
        const auto first = data::EnsureLayout();
        require(!first.readOnly && first.noticeUtf8.empty(), "fresh data layout is writable");
        require(data::WideToUtf8Path(first.canonicalRoot) == (root / "profile" / "Application Support" / "HarkenSearch").string(), "settings root stays in QA profile");
        const auto themePath = Platform::ThemeFiles::CustomThemePathUtf8();
        require(themePath == (root / "profile" / "Application Support" / "HarkenSearch" / "theme_custom.json").string(),
                "custom theme uses the isolated writable settings directory");
        require(data::PreviewIsolationAllowsDataPathUtf8(themePath), "custom theme path cannot escape QA isolation");
        require(fs::is_directory(data::WideToUtf8Path(first.activeDatabaseDir)), "database directory is created");
        require(Platform::DatabasePaths::GetDefaultDbDirWide() == first.activeDatabaseDir,
                "new-library UI uses the same isolated database directory");
        require(first.activeDatabaseFile.empty(), "fresh launch does not invent a library");
        std::ofstream(data::WideToUtf8Path(first.activeSettingsDir) + "/retained.ini") << "library=中文素材库";
        std::ofstream(themePath) << "{\"saved\":true}";
        data::ResetLayoutCacheForTests();
        require(data::EnsureLayout().canonicalRoot == first.canonicalRoot &&
                fs::exists(data::WideToUtf8Path(first.activeSettingsDir) + "/retained.ini"), "reopening keeps layout and existing settings");
        require(Platform::ThemeFiles::CustomThemePathUtf8() == themePath && fs::exists(themePath),
                "reopening resolves the same saved custom theme");
        data::ApplyPreviewIsolationProcessEnvironment();
        require(std::getenv("TMPDIR") && data::PreviewIsolationAllowsDataPathUtf8(std::getenv("TMPDIR")), "temporary files are isolated");
        const auto thes = data::EnsureThesaurusFileUtf8("/Applications/HarkenSearch.app/Contents/MacOS");
        require(!thes.readOnly && data::PreviewIsolationAllowsDataPathUtf8(data::WideToUtf8Path(thes.activePath)), "editable thesaurus stays outside the app bundle");
        const auto evidence = root / "evidence" / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(evidence);
        fs::create_directory_symlink(root.parent_path(), evidence / "escape-link");
        require(!data::PreviewIsolationAllowsDataPathUtf8((evidence / "escape-link" / "outside.ini").string()), "symbolic link escape is denied");
        fs::rename(themePath, evidence / "saved-theme.json");
        fs::create_symlink(root.parent_path() / "outside-theme.json", themePath);
        require(Platform::ThemeFiles::CustomThemePathUtf8().empty(), "custom theme symlink cannot escape QA isolation");
        fs::rename(themePath, evidence / "rejected-theme-link");
        fs::rename(evidence / "saved-theme.json", themePath);
        const auto savedProfile = evidence / "saved-profile";
        fs::rename(root / "profile", savedProfile);
        fs::create_directory_symlink(savedProfile, root / "profile");
        data::ResetLayoutCacheForTests();
        const auto denied = data::EnsureLayout();
        fs::rename(root / "profile", evidence / "rejected-profile-link");
        fs::rename(savedProfile, root / "profile");
        require(denied.readOnly && !denied.noticeUtf8.empty() && denied.activeSettingsDir.empty(), "unsafe profile fails closed without daily-data fallback");
        require(Platform::ThemeFiles::CustomThemePathUtf8().empty(), "unsafe profile cannot save a custom theme elsewhere");
        const fs::path settingsRoot(data::WideToUtf8Path(first.activeSettingsDir));
        const auto originalPermissions = fs::status(settingsRoot).permissions();
        fs::permissions(settingsRoot, fs::perms::owner_read | fs::perms::owner_exec);
        data::ResetLayoutCacheForTests();
        const auto unwritable = data::EnsureLayout();
        fs::permissions(settingsRoot, originalPermissions);
        require(unwritable.readOnly && !unwritable.noticeUtf8.empty(), "unwritable existing profile is reported as read-only");
        require(Platform::ThemeFiles::CustomThemePathUtf8().empty(), "unwritable settings directory cannot save a custom theme elsewhere");
        require(Platform::DatabasePaths::GetDefaultDbDirWide().empty(),
                "unwritable data directory does not fall back to the app bundle");
        std::cout << "Evidence retained at " << root << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
