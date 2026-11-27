#include "ExternalAiClientLink.h"
#include "ExternalAiClientConfigAccess.h"
#include "ExternalAiClientToml.h"
#include "platform/filesystem.h"

#include "../thirdparty/nlohmann_json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <random>
#include <utility>


namespace HarkenSearch {

namespace {

namespace fs = std::filesystem;
namespace fileSystem = Platform::FileSystem;
using ordered_json = nlohmann::ordered_json;

constexpr const char* kServerKey = "harken";
constexpr const char* kBackupSuffix = ".harken-backup";
constexpr const char* kLinkSuffix = ".harken-link";
constexpr const char* kDescription =
    "Harken library search: find sounds, folders and same-kind substitutes (read-only)";

std::string NormalizeSlashes(std::string value) {
    for (char& unit : value) {
        if (unit == '/') unit = '\\';
    }
    while (value.size() > 3 && value.back() == '\\') value.pop_back();
    return value;
}

bool ReadFileUtf8(const std::string& path, std::string& out, std::string& error) {
    const auto kind = fileSystem::QueryFilePathKind(path, fileSystem::PathLinkMode::Follow);
    if (kind == fileSystem::FilePathKind::Missing) {
        out.clear();
        return true;
    }
    if (kind == fileSystem::FilePathKind::RegularFile && fileSystem::ReadFileBinaryText(path, out)) return true;
    error = "cannot open " + path;
    return false;
}


std::string NewLinkId() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(32);
    for (int index = 0; index < 16; ++index) {
        const unsigned int byte = random() & 0xFFu;
        value.push_back(digits[byte >> 4]);
        value.push_back(digits[byte & 0x0Fu]);
    }
    return value;
}

bool ValidLinkId(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char unit) {
        return (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'f');
    });
}

std::string ReadLinkId(const std::string& configPath) {
    if (configPath.empty()) return {};
    const auto path = configPath + kLinkSuffix;
    if (fileSystem::QueryFilePathKind(path, fileSystem::PathLinkMode::NoFollow) !=
            fileSystem::FilePathKind::RegularFile) return {};
    const auto bytes = fileSystem::ReadFileBytes(path, 32);
    const std::string id(bytes.begin(), bytes.end());
    return ValidLinkId(id) ? id : std::string{};
}

bool ReadForEdit(const std::string& path, ClientConfigAccess::Snapshot& source, std::string& error) {
    if (ClientConfigAccess::ReadSnapshot(fs::u8path(path), source)) return true;
    error = "cannot open " + path;
    return false;
}

bool BackupOnce(const std::string& path, const ClientConfigAccess::Snapshot& source,
                std::string& backupPath, std::string& error) {
    if (source.state == ClientConfigAccess::State::Missing) return true;
    backupPath = path + kBackupSuffix;
    const auto backup = fs::u8path(backupPath);
    const auto existing = fileSystem::QueryFilePathKind(backupPath, fileSystem::PathLinkMode::NoFollow);
    if (existing == fileSystem::FilePathKind::RegularFile &&
        ClientConfigAccess::BackupMatches(source, backup)) return true;
    if (existing == fileSystem::FilePathKind::Missing) {
        if (ClientConfigAccess::Write(backup, fs::u8path(backupPath + ".harken-pending-" + NewLinkId()),
                                      source.bytes, false, &source)) return true;
    }
    error = "cannot back up " + path;
    return false;
}

bool WriteFileUtf8(const std::string& path, const std::string& content, std::string& error,
                   const ClientConfigAccess::Snapshot* source = nullptr) {
    const auto target = fs::u8path(path);
    if ((!target.has_parent_path() || fileSystem::EnsureDirectoryExists(target.parent_path().u8string())) &&
        ClientConfigAccess::Write(target, fs::u8path(path + ".harken-pending-" + NewLinkId()),
                                  content, true, source)) return true;
    error = "cannot write " + path;
    return false;
}

struct ConfigEntry {
    bool present = false;
    bool enabled = true;
    ordered_json command;
    ordered_json args;
};

std::string Trim(std::string value) {
    const auto notSpace = [](unsigned char unit) { return !std::isspace(unit); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

// --- JSON client (Cindy reads Pi's mcp.json) ---------------------------------

bool ParseJson(const std::string& text, ordered_json& out, std::string& error) {
    if (Trim(text).empty()) {
        out = ordered_json::object();
        return true;
    }
    out = ordered_json::parse(text, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        error = "existing file is not a JSON object";
        out = ordered_json::object();
        return false;
    }
    return true;
}

ordered_json BuildArgs(const ExternalAiLinkTarget& target) {
    auto args = ordered_json::array({"--library-id", target.libraryId, "--library-db", target.databasePath,
                                     "--library-root", target.rootPath, "--settings", target.settingsPath});
    if (!target.clientConfigPath.empty() && !target.clientLinkId.empty()) {
        args.push_back("--client-config");
        args.push_back(target.clientConfigPath);
        args.push_back("--client-link");
        args.push_back(target.clientLinkId);
    }
    return args;
}

ordered_json BuildServerEntry(const ExternalAiLinkTarget& target) {
    ordered_json entry = ordered_json::object();
    entry["command"] = target.executablePath;
    entry["args"] = BuildArgs(target);
    entry["description"] = kDescription;
    return entry;
}

ConfigEntry JsonEntry(const ordered_json& root) {
    const auto servers = root.find("mcpServers");
    if (servers == root.end()) return {};
    if (!servers->is_object()) return {true, false};
    const auto found = servers->find(kServerKey);
    if (found == servers->end()) return {};
    if (!found->is_object()) return {true, false};
    return {true, !found->contains("enabled") || (*found)["enabled"] == true,
        found->value("command", ordered_json{}), found->value("args", ordered_json{})};
}

std::optional<ExternalAiLinkTarget> HarkenTarget(const ConfigEntry& entry) {
    if (!entry.command.is_string() || !entry.args.is_array() ||
        (entry.args.size() != 8 && entry.args.size() != 12)) return std::nullopt;
    const auto command = entry.command.get<std::string>();
    auto executable = fs::u8path(command).filename().u8string();
    std::transform(executable.begin(), executable.end(), executable.begin(),
        [](unsigned char unit) { return static_cast<char>(std::tolower(unit)); });
    if (executable != "harkenexternalaimcp.exe" && executable != "harkenexternalaimcp") return std::nullopt;
    for (const auto& arg : entry.args) {
        if (!arg.is_string() || arg.get_ref<const std::string&>().empty()) return std::nullopt;
    }
    ExternalAiLinkTarget target{command, entry.args[1], entry.args[3], entry.args[5], entry.args[7]};
    if (entry.args.size() == 12) {
        target.clientConfigPath = entry.args[9];
        target.clientLinkId = entry.args[11];
        if (!ValidLinkId(target.clientLinkId)) return std::nullopt;
    }
    return entry.args == BuildArgs(target) ? std::optional<ExternalAiLinkTarget>{target} : std::nullopt;
}

// --- TOML client (Codex reads config.toml) ----------------------------------

std::string AddHarkenTable(std::string text, const ClientToml::OwnedTable& table,
                           const ExternalAiLinkTarget& target) {
    const auto newline = text.find('\n', table.begin);
    const std::string eol = newline != std::string::npos && newline > 0 && text[newline - 1] == '\r'
        ? "\r\n" : "\n";
    const std::string section = std::string("[mcp_servers.") + kServerKey + "]" + eol +
        "command = " + ordered_json(target.executablePath).dump() + eol +
        "args = " + BuildArgs(target).dump() + eol;
    if (table.present) {
        text.replace(table.begin, table.end - table.begin, section);
    } else {
        if (!text.empty() && text.back() != '\n') text += eol;
        text += section;
    }
    return text;
}

ConfigEntry TomlEntry(const ClientToml::OwnedTable& table) {
    return {table.present, table.enabled, ordered_json(table.command), ordered_json(table.args)};
}

bool ReadEntry(ExternalAiClient client, const std::string& path, ConfigEntry& entry, std::string& error) {
    std::string content;
    if (!ReadFileUtf8(path, content, error)) return false;
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        if (!ParseJson(content, root, error)) return false;
        entry = JsonEntry(root);
    } else {
        const auto table = ClientToml::Parse(content);
        if (!table) {
            error = "existing config uses a form this writer will not edit";
            return false;
        }
        entry = TomlEntry(*table);
    }
    return true;
}

bool MatchesTarget(const ExternalAiLinkTarget& stored, const ExternalAiLinkTarget& target) {
    return NormalizeSlashes(stored.executablePath) == NormalizeSlashes(target.executablePath) &&
        BuildArgs(stored) == BuildArgs(target);
}

}  // namespace

const char* ExternalAiClientKey(ExternalAiClient client) {
    return client == ExternalAiClient::Cindy ? "cindy" : "codex";
}

std::string ExternalAiClientCommandLine(const ExternalAiLinkTarget& target) {
    std::string out = "\"" + target.executablePath + "\"";
    const std::pair<const char*, std::string> pairs[] = {
        {"--library-id", target.libraryId}, {"--library-db", target.databasePath},
        {"--library-root", target.rootPath}, {"--settings", target.settingsPath}};
    for (const auto& pair : pairs) {
        out += std::string(" ") + pair.first + " \"" + pair.second + "\"";
    }
    return out;
}

ExternalAiLinkStatus QueryExternalAiClientLink(ExternalAiClient client, const std::string& configPath,
                                               const ExternalAiLinkTarget&) {
    ExternalAiLinkStatus status;
    status.configPath = configPath;
    if (configPath.empty()) return status;
    const auto linkId = ReadLinkId(configPath);
    status.canDisconnect = !linkId.empty();
    ConfigEntry entry;
    if (!ReadEntry(client, configPath, entry, status.detail)) {
        status.state = ExternalAiLinkState::Failed;
        return status;
    }
    const auto stored = HarkenTarget(entry);
    if (entry.present && !stored) {
        status.state = ExternalAiLinkState::Conflict;
        status.detail = "harken is already used by an unrecognized entry";
        return status;
    }
    status.canDisconnect = status.canDisconnect || stored.has_value();
    const bool active = stored && entry.enabled && !linkId.empty() && stored->clientLinkId == linkId &&
        NormalizeSlashes(stored->clientConfigPath) == NormalizeSlashes(configPath);
    status.state = active ? ExternalAiLinkState::Connected : ExternalAiLinkState::Disconnected;
    status.needsRelink = stored.has_value() && !active;
    return status;
}

bool ExternalAiClientLinkAuthorized(const ExternalAiLinkTarget& target) {
    if (!ValidLinkId(target.clientLinkId) || target.clientConfigPath.empty()) return false;
    const auto extension = fs::u8path(target.clientConfigPath).extension().u8string();
    if (extension != ".toml" && extension != ".json") return false;
    const auto client = extension == ".toml" ? ExternalAiClient::Codex : ExternalAiClient::Cindy;
    ConfigEntry entry;
    std::string error;
    if (!ReadEntry(client, target.clientConfigPath, entry, error) || !entry.enabled) return false;
    const auto stored = HarkenTarget(entry);
    return stored && MatchesTarget(*stored, target) &&
        ReadLinkId(target.clientConfigPath) == target.clientLinkId;
}

bool LinkExternalAiClient(ExternalAiClient client, const std::string& configPath,
                          const ExternalAiLinkTarget& target, ExternalAiLinkStatus& status) {
    status = QueryExternalAiClientLink(client, configPath, target);
    if (status.state == ExternalAiLinkState::Conflict || status.state == ExternalAiLinkState::Failed) return false;
    if (configPath.empty() || target.executablePath.empty() || target.libraryId.empty() ||
        target.databasePath.empty() || target.rootPath.empty() || target.settingsPath.empty()) return false;
    auto bound = target;
    bound.clientConfigPath = configPath;
    bound.clientLinkId = ReadLinkId(configPath);
    if (ExternalAiClientLinkAuthorized(bound)) return true;
    bound.clientLinkId = NewLinkId();
    std::string error;
    std::string backup;
    ClientConfigAccess::Snapshot original;
    if (!ReadForEdit(configPath, original, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    std::string updated;
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        if (!ParseJson(original.bytes, root, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        const auto entry = JsonEntry(root);
        if (entry.present && !HarkenTarget(entry)) {
            status.state = ExternalAiLinkState::Conflict;
            status.detail = "harken is already used by an unrecognized entry";
            return false;
        }
        if (!root.contains("mcpServers")) {
            root["mcpServers"] = ordered_json::object();
        }
        root["mcpServers"][kServerKey] = BuildServerEntry(bound);
        updated = root.dump(2) + "\n";
    } else {
        const auto table = ClientToml::Parse(original.bytes);
        if (!table) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = "existing config uses a form this writer will not edit";
            return false;
        }
        const auto entry = TomlEntry(*table);
        if (entry.present && !HarkenTarget(entry)) {
            status.state = ExternalAiLinkState::Conflict;
            status.detail = "harken is already used by an unrecognized entry";
            return false;
        }
        updated = AddHarkenTable(original.bytes, *table, bound);
    }
    if (!BackupOnce(configPath, original, backup, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    if (!WriteFileUtf8(configPath + kLinkSuffix, bound.clientLinkId, error) ||
        !WriteFileUtf8(configPath, updated, error, &original)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    status = QueryExternalAiClientLink(client, configPath, bound);
    return status.state == ExternalAiLinkState::Connected && ExternalAiClientLinkAuthorized(bound);
}

bool UnlinkExternalAiClient(ExternalAiClient client, const std::string& configPath,
                            ExternalAiLinkStatus& status) {
    status = ExternalAiLinkStatus{};
    status.configPath = configPath;
    if (configPath.empty()) return false;
    const auto marker = configPath + kLinkSuffix;
    const auto markerStatus = fileSystem::QueryFilePathKind(marker, fileSystem::PathLinkMode::NoFollow);
    std::string removeError;
    if (markerStatus != fileSystem::FilePathKind::Missing && markerStatus != fileSystem::FilePathKind::Error) {
        if (markerStatus != fileSystem::FilePathKind::RegularFile || ReadLinkId(configPath).empty()) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = "cannot verify the client link marker";
            return false;
        }
        status.revoked = fileSystem::RemoveFile(marker, &removeError);
    }
    if (markerStatus == fileSystem::FilePathKind::Error || !removeError.empty()) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = "cannot revoke the client link";
        return false;
    }
    std::string error;
    std::string backup;
    ClientConfigAccess::Snapshot original;
    if (!ReadForEdit(configPath, original, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        if (!ParseJson(original.bytes, root, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        const auto entry = JsonEntry(root);
        if (!HarkenTarget(entry)) {
            status.state = entry.present && !status.revoked ? ExternalAiLinkState::Conflict : ExternalAiLinkState::Disconnected;
            return status.state == ExternalAiLinkState::Disconnected;
        }
        if (!BackupOnce(configPath, original, backup, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        root["mcpServers"].erase(kServerKey);
        if (!WriteFileUtf8(configPath, root.dump(2) + "\n", error, &original)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        status.state = ExternalAiLinkState::Disconnected;
        status.detail = backup;
        return true;
    }
    const auto table = ClientToml::Parse(original.bytes);
    if (!table) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = "existing config uses a form this writer will not edit";
        return false;
    }
    const auto entry = TomlEntry(*table);
    if (!HarkenTarget(entry)) {
        status.state = entry.present && !status.revoked ? ExternalAiLinkState::Conflict : ExternalAiLinkState::Disconnected;
        return status.state == ExternalAiLinkState::Disconnected;
    }
    if (!BackupOnce(configPath, original, backup, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    auto updated = original.bytes;
    updated.erase(table->begin, table->end - table->begin);
    if (!WriteFileUtf8(configPath, updated, error, &original)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    status.state = ExternalAiLinkState::Disconnected;
    status.detail = backup;
    return true;
}

}  // namespace HarkenSearch
