#ifdef __APPLE__
#include "harken_user_data_layout.h"
#include "macos_file_io.h"
#import <Foundation/Foundation.h>

#include <algorithm>
#include <codecvt>
#include <cstdlib>
#include <filesystem>
#include <locale>
#include <mutex>
#include <optional>
#include <sys/stat.h>

namespace Platform::HarkenUserData {
namespace {
namespace fs = std::filesystem;
std::mutex cacheMutex;
std::optional<Layout> layoutCache;
std::optional<ThesaurusFile> thesaurusCache;

bool SafeAbsolutePath(const std::string& text, fs::path& result) {
    if (!MacFileIO::ValidPath(text) || Utf8ToWidePath(text).empty()) return false;
    result = fs::path(text);
    if (!result.is_absolute()) return false;
    fs::path current = result.root_path();
    for (const auto& component : result.relative_path()) {
        if (component == "..") return false;
        if (component.empty() || component == ".") continue;
        current /= component;
        struct stat info {};
        if (::lstat(current.c_str(), &info) == 0) {
            if (S_ISLNK(info.st_mode)) return false;
        } else if (errno != ENOENT) {
            return false;
        }
    }
    result = result.lexically_normal();
    return true;
}

bool IsWithin(const fs::path& child, const fs::path& root) {
    auto childPart = child.begin();
    for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart, ++childPart) {
        if (childPart == child.end() || *childPart != *rootPart) return false;
    }
    return true;
}

std::wstring Join(const std::wstring& parent, const char* child) {
    if (parent.empty()) return {};
    return Utf8ToWidePath((fs::path(WideToUtf8Path(parent)) / child).string());
}

Layout ComputeLayout() {
    const auto roots = ResolveRoots();
    Layout layout;
    layout.localAppData = roots.localAppData;
    layout.roamingAppData = roots.roamingAppData;
    const auto canonical = CanonicalRoot(roots);
    for (const auto& path : {canonical, CanonicalDatabaseDir(roots), CanonicalUpdatesDir(roots)}) {
        fs::path checked;
        const auto utf8 = WideToUtf8Path(path);
        MacFileIO::Descriptor directory;
        if (SafeAbsolutePath(utf8, checked) && PreviewIsolationAllowsDataPathUtf8(utf8)) {
            directory = MacFileIO::OpenDirectory(checked, true);
        }
        if (!directory || !MacFileIO::WritableDirectory(directory.get())) {
            layout.readOnly = true;
            layout.noticeUtf8 = "无法安全创建 Harken Search 数据目录，请检查路径与权限。没有改用其他目录。";
            return layout;
        }
    }
    layout.canonicalRoot = canonical;
    layout.activeSettingsDir = canonical;
    layout.activeDatabaseDir = CanonicalDatabaseDir(roots);
    layout.activeUpdatesDir = CanonicalUpdatesDir(roots);
    return layout;
}
}

bool PreviewIsolationActive() {
#if defined(ARIS_PREVIEW_ISOLATION) && ARIS_PREVIEW_ISOLATION
    return true;
#else
    const char* flag = std::getenv("ARIS_PREVIEW_ISOLATION");
    if (!flag) return false;
    std::string value(flag);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
#endif
}

std::string PreviewIsolationRootUtf8() {
#if defined(ARIS_PREVIEW_ISOLATION) && ARIS_PREVIEW_ISOLATION
#ifndef ARIS_PREVIEW_ISOLATION_ROOT_UTF8
#error "Preview isolation requires an explicit ARIS_PREVIEW_ISOLATION_ROOT_UTF8"
#endif
    return ARIS_PREVIEW_ISOLATION_ROOT_UTF8;
#else
    return {};
#endif
}

bool PreviewIsolationAllowsDataPathUtf8(const std::string& path) {
    if (!PreviewIsolationActive()) return true;
    fs::path root, child;
    return SafeAbsolutePath(PreviewIsolationRootUtf8(), root) && root != root.root_path() &&
           SafeAbsolutePath(path, child) && IsWithin(child, root);
}

void ApplyPreviewIsolationProcessEnvironment() {
    if (!PreviewIsolationActive()) return;
    fs::path root;
    if (!SafeAbsolutePath(PreviewIsolationRootUtf8(), root) || root == root.root_path()) return;
    const auto temp = root / "temp", runtime = root / "profile" / "runtime";
    for (const auto& path : {temp, runtime}) {
        if (!PreviewIsolationAllowsDataPathUtf8(path.string()) || !MacFileIO::OpenDirectory(path, true)) return;
    }
    ::setenv("TMPDIR", temp.c_str(), 1);
    ::setenv("TMP", temp.c_str(), 1);
    ::setenv("TEMP", temp.c_str(), 1);
    ::setenv("AURA_RUNTIME_ROOT", runtime.c_str(), 1);
    ::setenv("ARIS_PREVIEW_ISOLATION", "1", 1);
}

Roots ResolveRoots() {
    std::string support;
    if (PreviewIsolationActive()) {
        fs::path root;
        if (!SafeAbsolutePath(PreviewIsolationRootUtf8(), root) || root == root.root_path()) return {};
        support = (root / "profile" / "Application Support").string();
        if (!PreviewIsolationAllowsDataPathUtf8(support)) return {};
    } else {
        @autoreleasepool {
            NSURL* url = [[[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask] firstObject];
            const char* path = [[url path] UTF8String];
            if (path) support = path;
        }
    }
    fs::path checked;
    if (!SafeAbsolutePath(support, checked)) return {};
    const auto wide = Utf8ToWidePath(checked.string());
    return {wide, wide};
}

std::wstring CanonicalRoot(const Roots& roots) { return Join(roots.localAppData, "HarkenSearch"); }
std::wstring CanonicalSettingsDir(const Roots& roots) { return CanonicalRoot(roots); }
std::wstring CanonicalDatabaseDir(const Roots& roots) { return Join(CanonicalRoot(roots), "Database"); }
std::wstring CanonicalUpdatesDir(const Roots& roots) { return Join(CanonicalRoot(roots), "updates"); }

std::string WideToUtf8Path(const std::wstring& value) {
    try { return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(value); }
    catch (const std::range_error&) { return {}; }
}
std::wstring Utf8ToWidePath(const std::string& value) {
    try { return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(value); }
    catch (const std::range_error&) { return {}; }
}
std::string JoinExeFileUtf8(const std::string& directory, const char* name) {
    if (!name) return {};
    return (fs::path(directory) / name).string();
}
Layout EnsureLayout() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (!layoutCache) layoutCache = ComputeLayout();
    return *layoutCache;
}
void ResetLayoutCacheForTests() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    layoutCache.reset();
    thesaurusCache.reset();
}
std::wstring LegacyAuroDbFilePath() { return {}; } // macOS never had the Windows legacy layout.
std::vector<std::string> ProtectedDataRootsUtf8() {
    const auto root = CanonicalRoot(ResolveRoots());
    return root.empty() ? std::vector<std::string>{} : std::vector<std::string>{WideToUtf8Path(root)};
}
ThesaurusFile EnsureThesaurusFileUtf8(const std::string&) {
    const auto layout = EnsureLayout();
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (!thesaurusCache) {
        ThesaurusFile file;
        file.activePath = file.canonicalPath = Join(layout.activeSettingsDir, CanonicalThesaurusFileName());
        file.readOnly = layout.readOnly;
        file.noticeUtf8 = layout.noticeUtf8;
        thesaurusCache = file;
    }
    return *thesaurusCache;
}
void ResetThesaurusCacheForTests() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    thesaurusCache.reset();
}
}
#endif
