// SoundManager - 工具函数模块
// Utils.cpp - UTF-8转换、路径操作、字符串处理

#include "Utils.h"
#include "platform/database_paths.inl"
#include "platform/file_times.inl"
#include "platform/filesystem.h"
#include "platform/system.h"
#include "platform/harken_user_data_layout.h"
#include "platform/text_encoding.h"
#include "platform/text_encoding.inl"
#include <algorithm>
#include <vector>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string_view>
#include <utility>
// 已改为同步发送，不再需要 thread 和 chrono
// #include <thread>
// #include <chrono>

// UTF-8 <-> Wide String Conversion

std::wstring Utf8ToWide(const std::string& str) {
    return Platform::TextEncoding::Utf8ToWide(str);
}

std::string WideToUtf8(const std::wstring& wstr) {
    return Platform::TextEncoding::WideToUtf8(wstr);
}

fs::path Utf8ToPath(const std::string& str) {
#ifdef _WIN32
    return fs::path(Utf8ToWide(str));
#else
    return fs::path(str);
#endif
}

std::string PathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return WideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

// String Utilities

std::string TrimString(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

std::string TrimCopy(std::string s) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && isSpace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isSpace((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string SanitizeWindowsName(std::string name) {
    // Windows invalid characters: < > : " / \ | ? *
    const std::string invalid = "<>:\"/\\|?*";
    for (char& c : name) {
        if (invalid.find(c) != std::string::npos) c = '_';
    }
    name = TrimCopy(name);
    // Windows also dislikes trailing '.' and ' '
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) name = "_";
    return name;
}

int CompareNaturalUtf8(std::string_view lhs, std::string_view rhs) {
    auto isAsciiDigit = [](unsigned char ch) {
        return ch >= '0' && ch <= '9';
    };

    size_t i = 0;
    size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        const unsigned char a = static_cast<unsigned char>(lhs[i]);
        const unsigned char b = static_cast<unsigned char>(rhs[j]);

        if (isAsciiDigit(a) && isAsciiDigit(b)) {
            const size_t digitStartI = i;
            const size_t digitStartJ = j;
            while (i < lhs.size() && isAsciiDigit(static_cast<unsigned char>(lhs[i]))) ++i;
            while (j < rhs.size() && isAsciiDigit(static_cast<unsigned char>(rhs[j]))) ++j;

            size_t sigI = digitStartI;
            size_t sigJ = digitStartJ;
            while (sigI < i && lhs[sigI] == '0') ++sigI;
            while (sigJ < j && rhs[sigJ] == '0') ++sigJ;

            const size_t sigLenI = i - sigI;
            const size_t sigLenJ = j - sigJ;
            if (sigLenI != sigLenJ) {
                return sigLenI < sigLenJ ? -1 : 1;
            }

            for (size_t offset = 0; offset < sigLenI; ++offset) {
                const unsigned char da = static_cast<unsigned char>(lhs[sigI + offset]);
                const unsigned char db = static_cast<unsigned char>(rhs[sigJ + offset]);
                if (da != db) {
                    return da < db ? -1 : 1;
                }
            }

            const size_t runLenI = i - digitStartI;
            const size_t runLenJ = j - digitStartJ;
            if (runLenI != runLenJ) {
                return runLenI < runLenJ ? -1 : 1;
            }
            continue;
        }

        if (a != b) {
            return a < b ? -1 : 1;
        }
        ++i;
        ++j;
    }

    if (i < lhs.size()) return 1;
    if (j < rhs.size()) return -1;
    return 0;
}

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

static std::string NormalizePathForCompareUtf8(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return "";
    try {
        return ToLowerAscii(PathToUtf8(Utf8ToPath(pathUtf8).lexically_normal()));
    } catch (...) {
        return ToLowerAscii(pathUtf8);
    }
}

static bool SamePathForCompareUtf8(const std::string& lhsUtf8, const std::string& rhsUtf8) {
    return NormalizePathForCompareUtf8(lhsUtf8) == NormalizePathForCompareUtf8(rhsUtf8);
}

// Path Utilities

bool PathExistsUtf8(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return false;
    try {
        return Platform::FileSystem::Exists(pathUtf8);
    } catch (...) {
        return false;
    }
}

std::string GetExeDirUtf8() {
#if defined(_WIN32) || defined(__APPLE__)
    return Platform::GetExeDir();
#else
    return ".";
#endif
}

std::wstring GetDefaultDbDirWide() {
    const std::wstring platformDir = Platform::DatabasePaths::GetDefaultDbDirWide();
#ifdef __APPLE__
    // A failed data layout must never redirect a new library into the app bundle.
    return platformDir;
#else
    if (!platformDir.empty()) return platformDir;
    return Utf8ToWide(GetExeDirUtf8());
#endif
}

std::string GetDefaultDbDirUtf8() {
    return WideToUtf8(GetDefaultDbDirWide());
}

std::string GetDefaultDbPathUtf8() {
#ifdef _WIN32
    const auto layout = Platform::HarkenUserData::EnsureLayout();
    if (layout.kind == Platform::HarkenUserData::LayoutKind::FallbackLegacy &&
        layout.readOnly && !layout.activeDatabaseFile.empty()) {
        return WideToUtf8(layout.activeDatabaseFile);
    }
    return BuildDatabasePackageMainPathUtf8(GetDefaultDbDirUtf8() + "\\Auro.db");
#else
#ifdef __APPLE__
    const auto directory = GetDefaultDbDirUtf8();
    if (directory.empty()) return {};
    return BuildDatabasePackageMainPathUtf8((fs::u8path(directory) / "Harken Search").u8string());
#else
    return BuildDatabasePackageMainPathUtf8(GetExeDirUtf8() + "/Auro.db");
#endif
#endif
}

std::string GetLegacyDefaultDbPathUtf8() {
    const std::wstring platformPath = Platform::DatabasePaths::GetLegacyDefaultDbPathWide();
    if (!platformPath.empty()) return WideToUtf8(platformPath);
#ifdef _WIN32
    return GetExeDirUtf8() + "\\auro.db";
#else
    return GetExeDirUtf8() + "/auro.db";
#endif
}

bool IsLegacyAuroOwnedDatabasePathUtf8(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return false;
    if (SamePathForCompareUtf8(pathUtf8, GetLegacyDefaultDbPathUtf8())) return true;
#ifdef _WIN32
    if (SamePathForCompareUtf8(pathUtf8, GetExeDirUtf8() + "\\soundmanager.db")) return true;
#else
    if (SamePathForCompareUtf8(pathUtf8, GetExeDirUtf8() + "/soundmanager.db")) return true;
#endif
    try {
        fs::path p = Utf8ToPath(pathUtf8).lexically_normal();
        const std::string filename = ToLowerAscii(PathToUtf8(p.filename()));
        const std::string parent = ToLowerAscii(PathToUtf8(p.parent_path().filename()));
        return filename == "auro_dev.db" && parent == "database";
    } catch (...) {
        return false;
    }
}

std::string ResolveCanonicalHarkenSearchDatabasePathUtf8(const std::string& pathUtf8) {
    if (pathUtf8.empty()) {
        return {};
    }
    const std::string repairedPathUtf8 = RepairUtf8MojibakeFromAnsiIfNeeded(pathUtf8);
    if (IsLegacyAuroOwnedDatabasePathUtf8(repairedPathUtf8)) {
        return GetDefaultDbPathUtf8();
    }
    return repairedPathUtf8;
}

std::string RepairUtf8MojibakeFromAnsiIfNeeded(const std::string& valueUtf8) {
    return Platform::TextEncoding::RepairUtf8MojibakeFromSystemAnsi(valueUtf8);
}

void SplitFileNameUtf8(const std::string& pathUtf8, std::string& outStem, std::string& outExtWithDot) {
    fs::path p = Utf8ToPath(pathUtf8);
    outStem = PathToUtf8(p.stem());
    outExtWithDot = PathToUtf8(p.extension());
}

std::string NormalizeFolderPathFromKeyForLookup(const std::string& key) {
    if (key.size() < 3 || key.compare(0, 2, "F|") != 0) return "";
    const std::string rest = key.substr(2);
    if (rest.compare(0, 2, "H|") == 0) return "";
    std::string path = rest;
    if (path.size() >= 4 && path.compare(0, 4, "\\\\?\\") == 0)
        path = path.substr(4);
    else if (path.size() >= 3 && path.compare(0, 3, "\\?\\") == 0)
        path = path.substr(3);
    if (path.size() >= 2 && path[1] == ':') return path;
    return "";
}

bool ShouldHighlightFolderNodeForLocate(const std::string& nodePath, const std::string& highlightPath, int64_t highlightUntilMs, int64_t nowMs) {
    if (highlightPath.empty() || highlightUntilMs <= 0) return false;
    return nodePath == highlightPath && nowMs < highlightUntilMs;
}

void ApplyExportDeclickFade(float* samples, size_t numFrames, size_t channels, size_t globalStartFrame, size_t totalFrames, size_t fadeLenFrames) {
    if (!samples || channels == 0 || totalFrames == 0) return;
    if (fadeLenFrames == 0) return;
    if (fadeLenFrames > totalFrames / 2) fadeLenFrames = totalFrames / 2;
    constexpr float kPi = 3.14159265358979323846f;
    for (size_t f = 0; f < numFrames; ++f) {
        size_t global = globalStartFrame + f;
        float gain = 1.0f;
        if (global < fadeLenFrames) {
            // Raised-cosine fade-in: 0 → 1, smooth at both ends
            float t = (float)(global + 1) / (float)(fadeLenFrames);
            gain = 0.5f * (1.0f - cosf(kPi * t));
        } else if (global >= totalFrames - fadeLenFrames) {
            // Raised-cosine fade-out: 1 → 0
            float t = (float)(totalFrames - global) / (float)(fadeLenFrames);
            gain = 0.5f * (1.0f - cosf(kPi * t));
        }
        for (size_t c = 0; c < channels; ++c)
            samples[f * channels + c] *= gain;
    }
}

// Settings File I/O

std::unordered_map<std::string, std::string> LoadKeyValueFile(const std::string& pathUtf8) {
    std::unordered_map<std::string, std::string> kv;
    std::string contents;
    if (!Platform::FileSystem::ReadFileText(pathUtf8, contents)) return kv;
    std::istringstream in(contents);
    std::string line;
    while (std::getline(in, line)) {
        line = TrimString(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = TrimString(line.substr(0, eq));
        std::string v = TrimString(line.substr(eq + 1));
        if (!k.empty()) kv[k] = v;
    }
    return kv;
}

void SaveKeyValueFile(const std::string& pathUtf8, const std::vector<std::pair<std::string, std::string>>& items) {
    std::ostringstream out;
    out << "# Aris settings (auto-generated)\n";
    for (const auto& it : items) {
        out << it.first << "=" << it.second << "\n";
    }
    Platform::FileSystem::WriteFileText(pathUtf8, out.str());
}

bool ParseBool(const std::string& v, bool defVal) {
    std::string s = v;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return defVal;
}

int ParseInt(const std::string& v, int defVal) {
    try { return std::stoi(v); } catch (...) { return defVal; }
}

float ParseFloat(const std::string& v, float defVal) {
    try { return std::stof(v); } catch (...) { return defVal; }
}

// Time Formatting

std::string FormatDateTimeLocal(int64_t unixSeconds) {
    if (unixSeconds <= 0) return "-";
    std::time_t t = (std::time_t)unixSeconds;
    struct tm tmLocal;
#ifdef _WIN32
    localtime_s(&tmLocal, &t);
#else
    localtime_r(&t, &tmLocal);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
                  tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec);
    return buf;
}

bool GetFileTimesUtf8(const std::string& filePathUtf8, int64_t& outCreationUnix, int64_t& outModifiedUnix) {
    outCreationUnix = 0;
    outModifiedUnix = 0;
#ifdef _WIN32
    if (filePathUtf8.empty()) return false;
    std::wstring wFilePath = Utf8ToWide(filePathUtf8);
    return Platform::FileTimes::GetFileTimesWide(wFilePath, outCreationUnix, outModifiedUnix);
#else
    (void)filePathUtf8;
    return false;
#endif
}

std::string BuildDatabasePackageMainPathUtf8(const std::string& requestedPathUtf8) {
    if (requestedPathUtf8.empty()) return "";
    try {
        fs::path requested = Utf8ToPath(requestedPathUtf8);
        fs::path parent = requested.parent_path();
        fs::path filename = requested.filename();
        const std::string fileLower = ToLowerAscii(PathToUtf8(filename));
        if (fileLower == "main.db") {
            return PathToUtf8(requested.lexically_normal());
        }

        std::string stem = PathToUtf8(requested.stem());
        stem = SanitizeWindowsName(stem);
        if (stem.empty()) stem = "database";
        return PathToUtf8((parent / stem / "main.db").lexically_normal());
    } catch (...) {
        return requestedPathUtf8;
    }
}

std::string GetDatabasePackageRootUtf8(const std::string& mainDbPathUtf8) {
    if (mainDbPathUtf8.empty()) return "";
    try {
        fs::path p = Utf8ToPath(mainDbPathUtf8);
        const std::string fileLower = ToLowerAscii(PathToUtf8(p.filename()));
        if (fileLower == "main.db") {
            return PathToUtf8(p.parent_path().lexically_normal());
        }
        return PathToUtf8(p.lexically_normal());
    } catch (...) {
        return mainDbPathUtf8;
    }
}

std::string GetDatabaseDisplayNameUtf8(const std::string& dbPathUtf8) {
    if (dbPathUtf8.empty()) return "(none)";
    try {
        const std::string repairedPathUtf8 = RepairUtf8MojibakeFromAnsiIfNeeded(dbPathUtf8);
        fs::path p = Utf8ToPath(repairedPathUtf8);
        std::string stem = PathToUtf8(p.stem());
        if (ToLowerAscii(PathToUtf8(p.filename())) == "main.db") {
            std::string folderName = PathToUtf8(p.parent_path().filename());
            if (!folderName.empty()) return RepairUtf8MojibakeFromAnsiIfNeeded(folderName);
        }
        if (!stem.empty()) return RepairUtf8MojibakeFromAnsiIfNeeded(stem);
        return RepairUtf8MojibakeFromAnsiIfNeeded(PathToUtf8(p.filename()));
    } catch (...) {
        return RepairUtf8MojibakeFromAnsiIfNeeded(dbPathUtf8);
    }
}

// Security & Activation (Hardware ID + DingTalk Sentinel)

namespace {
// 内部轻量级哈希：用于机器码二次脱敏
static std::string SimpleHashHWID(const std::string& input) {
    uint64_t h = 0x12345678ABCDEF01ULL;
    for (char c : input) {
        h = ((h << 5) + h) ^ (uint64_t)(unsigned char)c;
    }
    std::stringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << h;
    return ss.str();
}
} // namespace

std::string GetEncryptedMachineID() {
    return SimpleHashHWID(WideToUtf8(Platform::LegacyHardwareFingerprint()));
}

// Settings Value Encryption (machine-bound XOR)

static std::string bytesToHex(const std::string& data);
static std::string hexToBytes(const std::string& hex);
static std::string DeriveSettingsKey();

namespace {
SettingsProtectionProvider g_settingsProtectionProvider;

static bool IsHexPayload(const std::string& hex) {
    if (hex.empty() || (hex.size() % 2) != 0) return false;
    for (unsigned char c : hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool DecodeHexStrict(const std::string& hex, std::string& output) {
    if (!IsHexPayload(hex)) return false;
    output.clear();
    output.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const auto nibble = [](unsigned char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };
        output.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return true;
}

static SettingsProtectionResult ProtectSettingsValueDefault(const std::string& plaintext) {
    if (plaintext.empty()) return SettingsProtectionResult::Success({});
#ifdef _WIN32
    std::string protectedBytes;
    if (!Platform::ProtectUserData(plaintext, protectedBytes)) {
        return SettingsProtectionResult::Failure("protect_failed");
    }
    return SettingsProtectionResult::Success("DPAPI1:" + bytesToHex(protectedBytes));
#else
    (void)plaintext;
    return SettingsProtectionResult::Failure("platform_unavailable");
#endif
}
} // namespace

ScopedSettingsProtectionProvider::ScopedSettingsProtectionProvider(SettingsProtectionProvider provider)
    : previous_(std::move(g_settingsProtectionProvider)) {
    g_settingsProtectionProvider = std::move(provider);
}

ScopedSettingsProtectionProvider::~ScopedSettingsProtectionProvider() {
    g_settingsProtectionProvider = std::move(previous_);
}

SettingsProtectionResult ProtectSettingsValue(const std::string& plaintext) {
    if (plaintext.empty()) return SettingsProtectionResult::Success({});
    if (g_settingsProtectionProvider) return g_settingsProtectionProvider(plaintext);
    return ProtectSettingsValueDefault(plaintext);
}

SettingsUnprotectionResult UnprotectSettingsValue(const std::string& encodedValue) {
    if (encodedValue.empty()) return SettingsUnprotectionResult::Success({});
    if (encodedValue.rfind("DPAPI1:", 0) == 0) {
        std::string protectedBytes;
        if (!DecodeHexStrict(encodedValue.substr(7), protectedBytes))
            return SettingsUnprotectionResult::Failure("format_invalid");
#ifdef _WIN32
        std::string plaintext;
        if (!Platform::UnprotectUserData(protectedBytes, plaintext)) {
            return SettingsUnprotectionResult::Failure("unprotect_failed");
        }
        return SettingsUnprotectionResult::Success(std::move(plaintext));
#else
        return SettingsUnprotectionResult::Failure("platform_unavailable");
#endif
    }
    if (encodedValue.rfind("ENC:", 0) == 0) {
        const std::string payload = encodedValue.substr(4);
        if (!IsHexPayload(payload)) return SettingsUnprotectionResult::Failure("format_invalid");
        return SettingsUnprotectionResult::Success(DecryptSettingsValue(encodedValue));
    }
    return SettingsUnprotectionResult::Success(encodedValue);
}

SiliconFlowSaveResult SaveSiliconFlowApiKeyTransactional(
    const std::string& settingsPathUtf8,
    const std::string& credentialTarget,
    const std::string& previousSavedPlaintext,
    const std::string& candidateKey) {
    SiliconFlowSaveResult result;
    result.retainedInput = candidateKey;
    result.savedPlaintext = previousSavedPlaintext;
    const auto protectedValue = ProtectSettingsValue(candidateKey);
    if (!protectedValue.success) {
        result.errorKind = protectedValue.errorKind.empty() ? "protect_failed" : protectedValue.errorKind;
        return result;
    }
    std::string previousStored;
    const bool hadPreviousStored =
        !credentialTarget.empty() && LoadWindowsCredential(credentialTarget, previousStored);
    if (!credentialTarget.empty() &&
        !StoreWindowsCredential(credentialTarget, protectedValue.encodedValue)) {
        result.errorKind = "protect_failed";
        return result;
    }
    auto restorePreviousCredential = [&]() {
        if (credentialTarget.empty()) return;
        if (hadPreviousStored) StoreWindowsCredential(credentialTarget, previousStored);
        else DeleteWindowsCredential(credentialTarget);
    };
    if (!settingsPathUtf8.empty()) {
        const auto previousBytes = Platform::FileSystem::ReadFileBytes(settingsPathUtf8);
        std::string contents(previousBytes.begin(), previousBytes.end());
        const std::string keyPrefix = "siliconFlowApiKey=";
        const size_t lineStart = contents.find(keyPrefix);
        if (lineStart != std::string::npos) {
            const size_t lineEnd = contents.find('\n', lineStart);
            const size_t replaceEnd = lineEnd == std::string::npos ? contents.size() : lineEnd;
            contents.replace(lineStart + keyPrefix.size(),
                             replaceEnd - (lineStart + keyPrefix.size()),
                             protectedValue.encodedValue);
        } else if (!protectedValue.encodedValue.empty()) {
            if (!contents.empty() && contents.back() != '\n') contents.push_back('\n');
            contents += keyPrefix + protectedValue.encodedValue + "\n";
        }
        Platform::FileSystem::StreamWriter writer;
        if (!writer.Open(settingsPathUtf8, false, true)) {
            restorePreviousCredential();
            result.errorKind = "settings_write_failed";
            return result;
        }
        const bool wrote = writer.Write(contents) && writer.Flush();
        writer.Close();
        if (!wrote) {
            restorePreviousCredential();
            result.errorKind = "settings_write_failed";
            return result;
        }
    }
    result.success = true;
    result.savedPlaintext = candidateKey;
    return result;
}

// Derive a 32-byte XOR key from machine ID using iterative hashing
static std::string DeriveSettingsKey() {
    std::string mid = GetEncryptedMachineID();
    // SimpleHashHWID-like expand to 32 bytes
    std::string key(32, '\0');
    uint64_t h = 0xA5C3E1D9B7F20461ULL;
    for (char c : mid) h = ((h << 5) + h) ^ (uint64_t)(unsigned char)c;
    for (int i = 0; i < 4; ++i) {
        h ^= (h >> 17);
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= (h >> 31);
        memcpy(&key[i * 8], &h, 8);
    }
    return key;
}

static std::string bytesToHex(const std::string& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char c : data) oss << std::setw(2) << (int)c;
    return oss.str();
}

static std::string hexToBytes(const std::string& hex) {
    std::string result;
    result.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned int byte = 0;
        std::istringstream iss(hex.substr(i, 2));
        iss >> std::hex >> byte;
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

std::string EncryptSettingsValue(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    std::string key = DeriveSettingsKey();
    std::string cipher(plaintext.size(), '\0');
    for (size_t i = 0; i < plaintext.size(); ++i)
        cipher[i] = plaintext[i] ^ key[i % key.size()];
    return "ENC:" + bytesToHex(cipher);
}

std::string DecryptSettingsValue(const std::string& ciphertext) {
    if (ciphertext.size() < 5 || ciphertext.substr(0, 4) != "ENC:") return ciphertext; // plaintext fallback
    std::string cipher = hexToBytes(ciphertext.substr(4));
    std::string key = DeriveSettingsKey();
    std::string plain(cipher.size(), '\0');
    for (size_t i = 0; i < cipher.size(); ++i)
        plain[i] = cipher[i] ^ key[i % key.size()];
    return plain;
}

bool IsValidSiliconFlowApiKey(const std::string& value) {
    if (value.size() < 4 || value.rfind("sk-", 0) != 0) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e;
    });
}

bool StoreWindowsCredential(const std::string& targetName, const std::string& value) {
    if (Platform::HarkenUserData::PreviewIsolationActive()) return false;
#ifndef _WIN32
    (void)targetName;
    (void)value;
    return false;
#else
    if (targetName.empty()) return false;
    if (value.empty()) return DeleteWindowsCredential(targetName);
    return Platform::StoreGenericCredential(targetName, value);
#endif
}

bool LoadWindowsCredential(const std::string& targetName, std::string& outValue) {
    outValue.clear();
    if (Platform::HarkenUserData::PreviewIsolationActive()) return false;

    if (targetName.empty()) return false;
    return Platform::LoadGenericCredential(targetName, outValue);
}

bool DeleteWindowsCredential(const std::string& targetName) {
    if (Platform::HarkenUserData::PreviewIsolationActive()) return false;

    return Platform::DeleteGenericCredential(targetName);
}

bool StoreSiliconFlowApiKey(const std::string& apiKey) {
    if (apiKey.empty()) return DeleteWindowsCredential("Aris/SiliconFlowApiKey");
    const auto protectedValue = ProtectSettingsValue(apiKey);
    if (!protectedValue.success) return false;
    return StoreWindowsCredential("Aris/SiliconFlowApiKey", protectedValue.encodedValue);
}

bool LoadSiliconFlowApiKey(std::string& outApiKey) {
    std::string stored;
    if (!LoadWindowsCredential("Aris/SiliconFlowApiKey", stored)) return false;
    const auto restored = UnprotectSettingsValue(stored);
    if (!restored.success) return false;
    outApiKey = restored.plaintext;
    return true;
}

bool DeleteSiliconFlowApiKey() {
    return DeleteWindowsCredential("Aris/SiliconFlowApiKey");
}

// CJK and pinyin search normalization.

static bool IsCJKChar(wchar_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) ||   // CJK Unified Ideographs
           (c >= 0x3400 && c <= 0x4DBF) ||   // CJK Extension A
           (c >= 0x20000 && c <= 0x2A6DF);   // CJK Extension B
}

// 核心 1：打断连续中文，拯救 SQLite 的单字分词
std::string InjectCJKSpaces(const std::string& utf8_str) {
#ifdef _WIN32
    std::wstring wstr = Utf8ToWide(utf8_str);
#else
    // The legacy non-Windows convenience wrapper preserves raw bytes. Search
    // tokenization needs Unicode code points so each Han character stays whole.
    std::wstring wstr = Platform::TextEncoding::Utf8ToWideStrict(utf8_str);
    if (wstr.empty() && !utf8_str.empty()) return utf8_str;
#endif
    std::wstring result;
    result.reserve(wstr.size() * 2);
    
    for (size_t i = 0; i < wstr.size(); ++i) {
        wchar_t c = wstr[i];
        if (IsCJKChar(c)) {
            // 在中文字符前后强制加入空格，使其成为独立 Token
            if (!result.empty() && result.back() != L' ') result.push_back(L' ');
            result.push_back(c);
            result.push_back(L' ');
        } else {
            result.push_back(c);
        }
    }
#ifdef _WIN32
    return WideToUtf8(result);
#else
    return Platform::TextEncoding::WideToUtf8Strict(result);
#endif
}

// 核心 2：利用 GB2312 一级字库天然按拼音排序的特性，实现 0 开销首字母提取
std::string GetPinyinInitials(const std::string& utf8_str) {
    std::wstring wstr = Utf8ToWide(utf8_str);
    std::string result;
    result.reserve(wstr.size());
    
    for (wchar_t c : wstr) {
        if (c >= L'a' && c <= L'z') { result.push_back((char)c); continue; }
        if (c >= L'A' && c <= L'Z') { result.push_back((char)std::tolower(c)); continue; }
        if (c >= L'0' && c <= L'9') { result.push_back((char)c); continue; }
        
        if (IsCJKChar(c)) {
            const std::string gbk = Platform::TextEncoding::WideCharToCp936(c);
            if (gbk.size() == 2) {
                unsigned char b1 = (unsigned char)gbk[0];
                unsigned char b2 = (unsigned char)gbk[1];
                if (b1 >= 0xB0 && b1 <= 0xD7) { // GB2312 一级字库范围
                    int code = (b1 << 8) | b2;
                    // O(1) 查表映射首字母
                    if (code >= 45217 && code <= 45252) result.push_back('a');
                    else if (code >= 45253 && code <= 45760) result.push_back('b');
                    else if (code >= 45761 && code <= 46317) result.push_back('c');
                    else if (code >= 46318 && code <= 46825) result.push_back('d');
                    else if (code >= 46826 && code <= 47009) result.push_back('e');
                    else if (code >= 47010 && code <= 47296) result.push_back('f');
                    else if (code >= 47297 && code <= 47613) result.push_back('g');
                    else if (code >= 47614 && code <= 48118) result.push_back('h');
                    else if (code >= 48119 && code <= 49061) result.push_back('j');
                    else if (code >= 49062 && code <= 49323) result.push_back('k');
                    else if (code >= 49324 && code <= 49895) result.push_back('l');
                    else if (code >= 49896 && code <= 50370) result.push_back('m');
                    else if (code >= 50371 && code <= 50613) result.push_back('n');
                    else if (code >= 50614 && code <= 50621) result.push_back('o');
                    else if (code >= 50622 && code <= 50905) result.push_back('p');
                    else if (code >= 50906 && code <= 51386) result.push_back('q');
                    else if (code >= 51387 && code <= 51445) result.push_back('r');
                    else if (code >= 51446 && code <= 52217) result.push_back('s');
                    else if (code >= 52218 && code <= 52697) result.push_back('t');
                    else if (code >= 52698 && code <= 52979) result.push_back('w');
                    else if (code >= 52980 && code <= 53688) result.push_back('x');
                    else if (code >= 53689 && code <= 54480) result.push_back('y');
                    else if (code >= 54481 && code <= 55289) result.push_back('z');
                }
            }
        }
    }
    return result;
}

// Audio File Utilities

bool IsAudioFileSupported(const std::string& path) {
    static const std::vector<std::string> exts = {
        ".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".wma", ".opus"
    };
    fs::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (const auto& e : exts) {
        if (ext == e) return true;
    }
    return false;
}
