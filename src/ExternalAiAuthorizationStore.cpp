#include "ExternalAiAuthorizationStore.h"

#include "FolderTreePathModePolicy.h"
#include "platform/filesystem.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

namespace HarkenSearch {
namespace {

namespace fs = std::filesystem;

struct CapabilityEntry {
    ExternalAiCapability capability;
    const char* key;
};

constexpr std::array<CapabilityEntry, ExternalAiCapabilityCount> CapabilityKeys{ {
    {ExternalAiCapability::SearchAudio, "searchAudio"},
    {ExternalAiCapability::FindSimilar, "findSimilar"},
    {ExternalAiCapability::FindFolders, "findFolders"},
    {ExternalAiCapability::ReadFileName, "readFileName"},
    {ExternalAiCapability::ReadMetadata, "readMetadata"},
    {ExternalAiCapability::EditMetadata, "editMetadata"},
    {ExternalAiCapability::AiNamingMetadata, "aiNamingMetadata"},
    {ExternalAiCapability::ReadPlaylist, "readPlaylist"},
    {ExternalAiCapability::EditPlaylist, "editPlaylist"},
} };

// Quotes one JSON string. Control characters and malformed input return an
// empty result, so the caller refuses to write rather than emit a document the
// connector would reject.
std::string Quote(const std::string& value)
{
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (const char unit : value) {
        switch (unit) {
        case '\\': quoted += "\\\\"; break;
        case '"': quoted += "\\\""; break;
        case '\n': quoted += "\\n"; break;
        case '\r': quoted += "\\r"; break;
        case '\t': quoted += "\\t"; break;
        default:
            if (static_cast<unsigned char>(unit) < 0x20) {
                return std::string{};
            }
            quoted.push_back(unit);
            break;
        }
    }
    quoted.push_back('"');
    return quoted;
}

bool ValidLibraryId(const std::string& libraryId)
{
    if (libraryId.empty() || libraryId.size() > 64) {
        return false;
    }
    return std::all_of(libraryId.begin(), libraryId.end(), [](char unit) {
        return (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') ||
            unit == '-' || unit == '_';
    });
}

}  // namespace

const char* ExternalAiCapabilityKey(ExternalAiCapability capability)
{
    const auto entry = std::find_if(CapabilityKeys.begin(), CapabilityKeys.end(),
        [capability](const CapabilityEntry& candidate) { return candidate.capability == capability; });
    return entry == CapabilityKeys.end() ? "" : entry->key;
}

const char* ExternalAiAccessValue(ExternalAiAccess access)
{
    switch (access) {
    case ExternalAiAccess::Editable:
        return "edit";
    case ExternalAiAccess::ReadOnly:
        return "read";
    case ExternalAiAccess::Denied:
    default:
        return "none";
    }
}

ExternalAiAccess ExternalAiSettings::capability(ExternalAiCapability value) const
{
    for (std::size_t index = 0; index < ExternalAiCapabilities.size(); ++index) {
        if (ExternalAiCapabilities[index] == value) {
            return capabilities[index];
        }
    }
    return ExternalAiAccess::Denied;
}

std::string SerializeExternalAiSettings(const ExternalAiSettings& settings)
{
    const std::string libraryId = Quote(settings.libraryId);
    if (libraryId.empty()) {
        return std::string{};
    }
    std::string document = "{\n";
    document += "  \"libraryId\": " + libraryId + ",\n";
    document += std::string("  \"enabled\": ") + (settings.enabled ? "true" : "false") + ",\n";
    document += std::string("  \"sampleLibrary\": ") + (settings.sampleLibrary ? "true" : "false") + ",\n";
    document += "  \"roots\": [";
    for (std::size_t index = 0; index < settings.roots.size(); ++index) {
        const std::string root = Quote(settings.roots[index].path);
        if (root.empty()) {
            return std::string{};
        }
        if (index != 0) {
            document += ", ";
        }
        document += "{\"path\": " + root +
            ", \"enabled\": " + (settings.roots[index].enabled ? "true" : "false") + "}";
    }
    document += "],\n";
    const std::string token = Quote(settings.bridgeToken);
    if (token.empty()) {
        return std::string{};
    }
    document += "  \"bridgeToken\": " + token + ",\n";
    document += "  \"capabilities\": {\n";
    for (std::size_t index = 0; index < CapabilityKeys.size(); ++index) {
        document += std::string("    \"") + CapabilityKeys[index].key + "\": \"" +
            ExternalAiAccessValue(settings.capability(CapabilityKeys[index].capability)) + "\"";
        document += index + 1 == CapabilityKeys.size() ? "\n" : ",\n";
    }
    document += "  }\n}\n";
    return document;
}

bool ParseExternalAiSettings(const std::string& json, const std::string& expectedLibraryId,
    ExternalAiSettings& out, std::string& error)
{
    ExternalAiAuthorization authorization;
    if (!ExternalAiAuthorization::Parse(json, authorization, error)) {
        return false;
    }
    if (authorization.libraryId() != expectedLibraryId) {
        error = "authorization document belongs to another library";
        return false;
    }
    ExternalAiSettings settings;
    settings.libraryId = authorization.libraryId();
    settings.enabled = authorization.enabled();
    settings.sampleLibrary = authorization.sampleLibrary();
    settings.roots = authorization.roots();
    settings.bridgeToken = authorization.bridgeToken();
    for (std::size_t index = 0; index < ExternalAiCapabilities.size(); ++index) {
        settings.capabilities[index] = authorization.AccessFor(ExternalAiCapabilities[index]);
    }
    out = std::move(settings);
    return true;
}

std::string ExternalAiSettingsPath(const std::string& settingsDirectory, const std::string& libraryId)
{
    if (settingsDirectory.empty() || !ValidLibraryId(libraryId)) {
        return std::string{};
    }
    fs::path directory = fs::u8path(settingsDirectory).make_preferred();
    directory /= "external-ai";
    directory /= libraryId + ".json";
    return directory.u8string();
}

std::string ExternalAiLibraryIdForPath(const std::string& canonicalDatabasePathUtf8)
{
    if (canonicalDatabasePathUtf8.empty()) {
        return std::string{};
    }
    // Fold case and slash style so the same file keeps one identity on Windows;
    // FNV-1a 64 is deterministic across restarts and app builds.
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char unit : canonicalDatabasePathUtf8) {
        unsigned char folded = unit;
        if (folded == '\\') {
            folded = '/';
        } else if (folded >= 'A' && folded <= 'Z') {
            folded = static_cast<unsigned char>(folded - 'A' + 'a');
        }
        hash ^= folded;
        hash *= 1099511628211ull;
    }
    char buffer[21]{};
    std::snprintf(buffer, sizeof(buffer), "lib-%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer);
}
bool LoadExternalAiSettings(const std::string& path, const std::string& libraryId, ExternalAiSettings& out,
    std::string& error, bool* existed)
{
    error.clear();
    if (existed) {
        *existed = false;
    }
    if (!ValidLibraryId(libraryId)) {
        error = "library identity cannot name an authorization document";
        return false;
    }
    ExternalAiSettings defaults;
    defaults.libraryId = libraryId;
    const auto source = fs::u8path(path);
    if (!source.is_absolute() || source.filename().empty()) {
        out = std::move(defaults);
        error = "authorization document path must be an absolute file path";
        return false;
    }
    std::error_code statusError;
    const auto entry = fs::symlink_status(source, statusError);
    if (entry.type() == fs::file_type::not_found &&
        (!statusError || statusError == std::errc::no_such_file_or_directory)) {
        out = std::move(defaults);
        return true;
    }
    if (existed) {
        *existed = fs::exists(entry);
    }
    if (statusError || !fs::is_regular_file(source, statusError)) {
        out = std::move(defaults);
        error = "authorization document is not a readable file";
        return false;
    }
    std::ifstream input(source, std::ios::binary);
    std::string document;
    char buffer[4096];
    while (input) {
        input.read(buffer, sizeof(buffer));
        document.append(buffer, static_cast<std::size_t>(input.gcount()));
    }
    if (input.bad() || !input.eof()) {
        out = std::move(defaults);
        error = "authorization document could not be read";
        return false;
    }
    if (!ParseExternalAiSettings(document, libraryId, out, error)) {
        out = std::move(defaults);
        return false;
    }
    return true;
}

namespace {

// 32 random bytes as hex. Only the settings page and the connector (which reads
// the same document) ever see it; it is what the write bridge asks for.
std::string NewBridgeToken()
{
    std::random_device device;
    static const char* hex = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (int index = 0; index < 32; ++index) {
        const unsigned int byte = device() & 0xFFu;
        token.push_back(hex[byte >> 4]);
        token.push_back(hex[byte & 0x0Fu]);
    }
    return token;
}

}  // namespace

bool SaveExternalAiSettings(const std::string& path, const ExternalAiSettings& settings, std::string& error)
{
    if (!ValidLibraryId(settings.libraryId)) {
        error = "library identity cannot name an authorization document";
        return false;
    }
    if (settings.roots.size() > 32) {
        error = "at most 32 ticked directories are allowed";
        return false;
    }
    ExternalAiSettings persisted = settings;
    if (persisted.bridgeToken.empty()) {
        persisted.bridgeToken = NewBridgeToken();
    }
    const std::string document = SerializeExternalAiSettings(persisted);
    if (document.empty()) {
        error = "authorization document could not be serialized";
        return false;
    }
    const fs::path target = fs::u8path(path).make_preferred();
    if (!target.is_absolute() || target.filename().empty()) {
        error = "authorization document path must be an absolute file path";
        return false;
    }
    if (!Platform::FileSystem::EnsureDirectoryExists(target.parent_path().u8string())) {
        error = "authorization directory could not be created";
        return false;
    }
    const fs::path pending = fs::u8path(target.u8string() + ".pending");
    std::size_t written = 0;
    {
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "authorization document could not be written";
            return false;
        }
        stream.write(document.data(), static_cast<std::streamsize>(document.size()));
        stream.close();
        written = stream.good() ? document.size() : 0;
    }
    if (written != document.size()) {
        Platform::FileSystem::RemoveFile(pending.u8string());
        error = "authorization document could not be fully written";
        return false;
    }
    if (!Platform::FileSystem::AtomicFileReplace(target.u8string(), pending.u8string())) {
        Platform::FileSystem::RemoveFile(pending.u8string());
        error = "authorization document could not be replaced atomically";
        return false;
    }
    return true;
}

bool SetExternalAiMasterSwitch(const std::string& path, ExternalAiSettings& draft, bool enabled, std::string& error)
{
    error.clear();
    if (!enabled) {
        ExternalAiSettings saved;
        if (!LoadExternalAiSettings(path, draft.libraryId, saved, error)) return false;
        if (saved.enabled) {
            saved.enabled = false;
            if (!SaveExternalAiSettings(path, saved, error)) return false;
        }
    }
    draft.enabled = enabled;
    return true;
}

ExternalAiLinkRoot ExternalAiLinkRootFor(const std::vector<ExternalAiRoot>& roots)
{
    using namespace aura::folder_tree;
    ExternalAiLinkRoot result;
    std::string volume;
    for (const auto& root : roots) {
        if (!root.enabled) continue;
        const auto candidate = NormalizeFolderPathPreserved(root.path);
        const auto normalized = NormalizeFolderPathForCompare(candidate);
        if (normalized.size() < 3 || normalized[0] < 'a' || normalized[0] > 'z' ||
            normalized[1] != ':' || normalized[2] != '\\') {
            return {};
        }
        const auto candidateVolume = normalized.substr(0, 2);
        if (volume.empty()) {
            volume = candidateVolume;
            result.path = candidate;
        } else if (volume != candidateVolume) {
            return {{}, true};
        } else {
            while (result.path.size() > 3 &&
                   !PathIsInsideRoot(normalized, NormalizeFolderPathForCompare(result.path))) {
                const auto parent = result.path.find_last_of('\\');
                if (parent == std::string::npos) return {};
                result.path.resize(std::max<std::size_t>(parent, 3));
            }
        }
    }
    return result;
}

}  // namespace HarkenSearch
