#pragma once

#include <string>
#include <vector>

namespace Platform::HarkenUserData {

enum class LayoutKind {
    FreshInstall,
    Migrated,
    FallbackLegacy,
};

struct Layout {
    LayoutKind kind = LayoutKind::FreshInstall;
    std::wstring localAppData;
    std::wstring roamingAppData;
    std::wstring canonicalRoot;
    std::wstring activeSettingsDir;
    std::wstring activeDatabaseDir;
    std::wstring activeDatabaseFile;
    std::wstring activeUpdatesDir;
    bool readOnly = false;
    std::string noticeUtf8;
};

struct Roots {
    std::wstring localAppData;
    std::wstring roamingAppData;
};

Roots ResolveRoots();
bool PreviewIsolationActive();
std::string PreviewIsolationRootUtf8();
bool PreviewIsolationAllowsDataPathUtf8(const std::string& pathUtf8);
void ApplyPreviewIsolationProcessEnvironment();
std::wstring CanonicalRoot(const Roots& roots);
std::wstring CanonicalDatabaseDir(const Roots& roots);
std::wstring CanonicalSettingsDir(const Roots& roots);
std::wstring CanonicalUpdatesDir(const Roots& roots);
std::string WideToUtf8Path(const std::wstring& value);

Layout EnsureLayout();
void ResetLayoutCacheForTests();
std::wstring LegacyAuroDbFilePath();
std::vector<std::string> ProtectedDataRootsUtf8();

inline const char* CanonicalThesaurusFileName() { return "harken_thesaurus.json"; }
inline const char* LegacyArisThesaurusFileName() { return "aris_thesaurus.json"; }
inline const char* LegacyAuroThesaurusFileName() { return "auro_thesaurus.json"; }
inline const char* ThesaurusMigrationMarkerFileName() { return "harken_thesaurus.migration.marker"; }
inline const char* CanonicalFeaturesJsonFileName() { return "harken_features.json"; }
inline const char* CanonicalFeaturesSigFileName() { return "harken_features.sig"; }
inline const char* LegacyArisFeaturesJsonFileName() { return "aris_features.json"; }
inline const char* LegacyArisFeaturesSigFileName() { return "aris_features.sig"; }
inline const char* LegacyAuroFeaturesJsonFileName() { return "auro_features.json"; }
inline const char* LegacyAuroFeaturesSigFileName() { return "auro_features.sig"; }
inline const char* CanonicalRuntimePairFileName() { return "harken-runtime-pair.json"; }
inline const char* LegacyRuntimePairFileName() { return "aris-runtime-pair.json"; }
#if defined(__APPLE__)
inline const char* CanonicalReaperPluginFileName() { return "reaper_harken.dylib"; }
inline const char* LegacyReaperPluginFileName() { return "reaper_auro.dylib"; }
#else
inline const char* CanonicalReaperPluginFileName() { return "reaper_harken.dll"; }
inline const char* LegacyReaperPluginFileName() { return "reaper_auro.dll"; }
#endif
inline const char* CanonicalProcessManifestFileName() { return "HarkenSearch.manifest"; }
inline const char* LegacyProcessManifestFileName() { return "Aris.manifest"; }
inline const char* CanonicalLocaleLedgerFileName() { return "harken.ui.strings.json"; }
inline const char* LegacyLocaleLedgerFileName() { return "aris.ui.strings.json"; }

struct ThesaurusFile {
    LayoutKind kind = LayoutKind::FreshInstall;
    std::wstring activePath;
    std::wstring canonicalPath;
    bool readOnly = false;
    std::string noticeUtf8;
};

std::wstring Utf8ToWidePath(const std::string& value);
std::string JoinExeFileUtf8(const std::string& exeDir, const char* fileName);
ThesaurusFile EnsureThesaurusFileUtf8(const std::string& exeDirUtf8);
void ResetThesaurusCacheForTests();


}  // namespace Platform::HarkenUserData
