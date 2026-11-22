#pragma once
// Folder subtree match-path helpers.
//
// One library folder can be stored under several textual spellings: plain UNC,
// the extended long-path form, a local drive path, or the legacy
// single-backslash UNC form produced by an older folding bug. Prefix matching
// for "remove path" must cover every spelling of one root, otherwise removal
// reports failure and leaves rows behind.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace HarkenSearch {

inline const char* LongUncPrefix() { return R"(\\?\UNC\)"; }
inline const char* LongLocalPrefix() { return R"(\\?\)"; }

inline bool StartsWithAsciiCaseInsensitive(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(text[i]);
        unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline std::string NormalizeFolderPathPreserveCaseForDbMatch(std::string_view rawPath) {
    std::string s(rawPath);
#ifdef _WIN32
    if (StartsWithAsciiCaseInsensitive(s, LongUncPrefix())) {
        s = std::string(R"(\\)") + s.substr(std::strlen(LongUncPrefix()));
    } else if (StartsWithAsciiCaseInsensitive(s, LongLocalPrefix())) {
        s = s.substr(std::strlen(LongLocalPrefix()));
    }
    std::replace(s.begin(), s.end(), '/', '\\');
    if (s.size() > 1 && s[0] == '\\' && s[1] != '\\') {
        // Legacy folding of the extended UNC prefix dropped one leading backslash.
        s.insert(s.begin(), '\\');
    }
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
#else
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
#endif
    return s;
}

// Exclusive upper bound for a folder-prefix range. Appending 0xFF covers every
// valid UTF-8 suffix without incrementing a multibyte code unit.
inline std::string MakeFolderPathUpperBound(const std::string& folderPath) {
    if (folderPath.empty()) return folderPath;
    return folderPath + '\xff';
}

inline std::string MakeFolderChildPrefix(const std::string& folderPath) {
    if (folderPath.empty()) return folderPath;
    std::string prefix = folderPath;
    if (prefix.back() != '\\' && prefix.back() != '/') {
#ifdef _WIN32
        prefix.push_back('\\');
#else
        prefix.push_back('/');
#endif
    }
    return prefix;
}

inline void AddUniqueFolderMatchPath(std::vector<std::string>& paths, std::string path) {
    if (path.empty()) return;
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(std::move(path));
    }
}

inline void AddWindowsPathSpellings(std::vector<std::string>& paths, const std::string& plainPath) {
#ifdef _WIN32
    if (plainPath.size() >= 2 && plainPath[1] == ':') {
        std::string driveUpper = plainPath;
        driveUpper[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(driveUpper[0])));
        std::string driveLower = plainPath;
        driveLower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(driveLower[0])));
        AddUniqueFolderMatchPath(paths, driveUpper);
        AddUniqueFolderMatchPath(paths, driveLower);
        AddUniqueFolderMatchPath(paths, std::string(LongLocalPrefix()) + driveUpper);
        AddUniqueFolderMatchPath(paths, std::string(LongLocalPrefix()) + driveLower);
    } else if (plainPath.size() >= 2 && plainPath[0] == '\\' && plainPath[1] == '\\') {
        const std::string rest = plainPath.substr(2);
        AddUniqueFolderMatchPath(paths, std::string(LongUncPrefix()) + rest);
        // Legacy rows written by the old folding bug lost one leading backslash.
        AddUniqueFolderMatchPath(paths, std::string(R"(\)") + rest);
    }
#else
    (void)paths;
    (void)plainPath;
#endif
}

// Every textual spelling that identifies the same folder root.
inline std::vector<std::string> BuildFolderSubtreeMatchPaths(const std::string& canonicalPath,
                                                            const std::string& rawPath) {
    std::vector<std::string> paths;
    paths.reserve(8);
    const std::string canonicalPlain = NormalizeFolderPathPreserveCaseForDbMatch(canonicalPath);
    const std::string rawPlain = NormalizeFolderPathPreserveCaseForDbMatch(rawPath);
    AddUniqueFolderMatchPath(paths, canonicalPath);
    AddUniqueFolderMatchPath(paths, rawPath);
    AddUniqueFolderMatchPath(paths, canonicalPlain);
    AddUniqueFolderMatchPath(paths, rawPlain);
    AddWindowsPathSpellings(paths, canonicalPlain);
    AddWindowsPathSpellings(paths, rawPlain);
    return paths;
}

}  // namespace HarkenSearch
