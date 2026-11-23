#include "ThemeLibraryFiles.h"
#include "platform/filesystem.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace harken::theme_library {
namespace fs = std::filesystem;

namespace {

std::string ToLowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string TrimSpacesAndDots(std::string value) {
    const auto isEdge = [](char c) { return c == ' ' || c == '.' || c == '\t'; };
    const auto first = std::find_if_not(value.begin(), value.end(), isEdge);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isEdge).base();
    if (first >= last) return {};
    return std::string(first, last);
}

// Caps a UTF-8 name without cutting a multi-byte sequence in half.
std::string CapUtf8Bytes(const std::string& value, size_t maxBytes) {
    if (value.size() <= maxBytes) return value;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) --cut;
    return value.substr(0, cut);
}

uint32_t Fnv1a32(const std::string& value) {
    uint32_t hash = 2166136261u;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

// Windows reserves these as device names whatever the extension is, so
// "CON.json" is still the console device and cannot be saved as a file.
bool IsReservedDeviceName(const std::string& stem) {
    static const char* const kReserved[] = {
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
        "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
        "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string lower = ToLowerAscii(stem);
    for (const char* reserved : kReserved) {
        if (lower == reserved) return true;
    }
    return false;
}

} // namespace

fs::path LibraryDirectory(const fs::path& settingsFile) {
    if (settingsFile.empty() || settingsFile.parent_path().empty()) return {};
    return settingsFile.parent_path() / fs::u8path("themes");
}

fs::path CustomThemePath(const fs::path& settingsFile) {
    if (settingsFile.empty() || settingsFile.parent_path().empty()) return {};
    return settingsFile.parent_path() / fs::u8path("theme_custom.json");
}

std::string FileStemForName(const std::string& nameUtf8) {
    std::string cleaned;
    cleaned.reserve(nameUtf8.size());
    for (char c : nameUtf8) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) continue;
        switch (c) {
            case '<': case '>': case ':': case '"': case '/':
            case '\\': case '|': case '?': case '*':
                continue;
            default:
                break;
        }
        cleaned.push_back(c);
    }
    cleaned = TrimSpacesAndDots(std::move(cleaned));
    if (cleaned.empty()) return "theme";
    std::string stem = std::move(cleaned);
    if (stem.size() > 120) {
        // Two long names can share a prefix; keep the tail distinct instead of
        // silently saving both into one file.
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "-%08x", Fnv1a32(stem));
        std::string head = CapUtf8Bytes(stem, 120 - std::strlen(suffix));
        stem = TrimSpacesAndDots(std::move(head)) + suffix;
    }
    if (IsReservedDeviceName(stem)) return "_" + stem;
    return stem;
}

fs::path ThemePathFor(const fs::path& libraryDir, const std::string& nameUtf8) {
    if (libraryDir.empty()) return {};
    fs::path file = fs::u8path(FileStemForName(nameUtf8));
    file += ".json";
    return libraryDir / file;
}

bool SaveThemeJson(const fs::path& target, const std::string& json, std::string& error) {
    error.clear();
    if (target.empty() || json.empty()) {
        error = "theme path or theme JSON is empty";
        return false;
    }
    const fs::path parent = target.parent_path();
    if (parent.empty() || !Platform::FileSystem::EnsureDirectoryExists(parent.u8string())) {
        error = "theme directory is unavailable";
        return false;
    }
    static std::atomic<unsigned long long> sequence{0};
    fs::path pending = target;
    pending += ".pending-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(++sequence);
    {
        Platform::FileSystem::StreamWriter out;
        if (!out.Open(pending.u8string(), false, true)) {
            error = "cannot open theme staging file";
            return false;
        }
        if (!out.Write(json) || !out.Flush()) {
            error = "cannot write theme staging file";
            out.Close();
            Platform::FileSystem::RemoveFile(pending.u8string());
            return false;
        }
        if (!out.CloseChecked()) {
            error = "cannot close theme staging file";
            Platform::FileSystem::RemoveFile(pending.u8string());
            return false;
        }
    }
    if (!Platform::FileSystem::AtomicFileReplace(target.u8string(), pending.u8string())) {
        error = "cannot replace theme file";
        Platform::FileSystem::RemoveFile(pending.u8string());
        return false;
    }
    return true;
}

bool SaveThemeCopy(const fs::path& libraryDir, const std::string& nameUtf8,
                   const std::string& json, std::string& error) {
    return SaveThemeJson(ThemePathFor(libraryDir, nameUtf8), json, error);
}

std::vector<fs::path> ListThemes(const fs::path& libraryDir) {
    std::vector<fs::path> themes;
    if (libraryDir.empty() || !Platform::FileSystem::IsDirectory(libraryDir.u8string())) {
        return themes;
    }
    std::error_code ec;
    for (fs::directory_iterator it(libraryDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
        const fs::path path = it->path();
        if (ToLowerAscii(path.extension().u8string()) != ".json") continue;
        themes.push_back(path);
    }
    std::sort(themes.begin(), themes.end(), [](const fs::path& left, const fs::path& right) {
        const auto leftKey = ToLowerAscii(left.stem().u8string());
        const auto rightKey = ToLowerAscii(right.stem().u8string());
        if (leftKey != rightKey) return leftKey < rightKey;
        return left.u8string() < right.u8string();
    });
    return themes;
}

bool AdoptLegacyCustomTheme(const fs::path& legacyPath, const fs::path& target,
                            std::string& error) {
    error.clear();
    if (target.empty()) {
        error = "theme target path is empty";
        return false;
    }
    if (Platform::FileSystem::Exists(target.u8string())) return true;
    if (legacyPath.empty()) {
        error = "no legacy theme path";
        return false;
    }
    const auto bytes = Platform::FileSystem::ReadFileBytes(legacyPath.u8string());
    if (bytes.empty()) {
        error = "no legacy theme file";
        return false;
    }
    return SaveThemeJson(target, std::string(bytes.begin(), bytes.end()), error);
}

} // namespace harken::theme_library
