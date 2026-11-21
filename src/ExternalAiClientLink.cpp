#include "ExternalAiClientLink.h"

#include "../thirdparty/nlohmann_json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace HarkenSearch {

namespace {

namespace fs = std::filesystem;
using ordered_json = nlohmann::ordered_json;

constexpr const char* kServerKey = "harken";
constexpr const char* kBackupSuffix = ".harken-backup";
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
    std::error_code code;
    if (!fs::exists(fs::u8path(path), code)) {
        out.clear();
        return true;
    }
    std::ifstream stream(fs::u8path(path), std::ios::binary);
    if (!stream) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

bool BackupOnce(const std::string& path, std::string& backupPath, std::string& error) {
    std::error_code code;
    if (!fs::exists(fs::u8path(path), code)) return true;  // nothing to back up yet
    backupPath = path + kBackupSuffix;
    if (fs::exists(fs::u8path(backupPath), code)) return true;  // keep the oldest copy
    fs::copy_file(fs::u8path(path), fs::u8path(backupPath), fs::copy_options::none, code);
    if (code) {
        error = "cannot back up " + path;
        return false;
    }
    return true;
}

bool WriteFileUtf8(const std::string& path, const std::string& content, std::string& error) {
    std::error_code code;
    const fs::path target = fs::u8path(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), code);
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write " + path;
        return false;
    }
    stream << content;
    if (!stream.flush()) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

std::string Trim(std::string value) {
    const auto notSpace = [](unsigned char unit) { return !std::isspace(unit); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

// --- JSON client (Cindy reads Pi's mcp.json) ---------------------------------

bool ParseJsonFile(const std::string& path, ordered_json& out, bool& missing, std::string& error) {
    std::string text;
    if (!ReadFileUtf8(path, text, error)) return false;
    if (Trim(text).empty()) {
        missing = true;
        out = ordered_json::object();
        return true;
    }
    missing = false;
    out = ordered_json::parse(text, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        error = "existing file is not a JSON object";
        out = ordered_json::object();
        return false;
    }
    return true;
}

ordered_json BuildServerEntry(const ExternalAiLinkTarget& target) {
    ordered_json entry = ordered_json::object();
    entry["command"] = target.executablePath;
    entry["args"] = ordered_json::array({"--library-id", target.libraryId, "--library-db",
                                         target.databasePath, "--library-root", target.rootPath,
                                         "--settings", target.settingsPath});
    entry["description"] = kDescription;
    return entry;
}

bool JsonEntryMatches(const ordered_json& servers, const ExternalAiLinkTarget& target) {
    if (!servers.is_object()) return false;
    const auto found = servers.find(kServerKey);
    if (found == servers.end() || !found->is_object()) return false;
    const ordered_json& entry = *found;
    const auto command = entry.find("command");
    if (command == entry.end() || !command->is_string()) return false;
    if (NormalizeSlashes(command->get<std::string>()) != NormalizeSlashes(target.executablePath)) return false;
    const auto args = entry.find("args");
    if (args == entry.end() || !args->is_array()) return false;
    std::string joined;
    for (const auto& item : *args) {
        if (!item.is_string()) return false;
        joined += item.get<std::string>();
        joined.push_back('\n');
    }
    return joined.find(target.libraryId) != std::string::npos &&
           joined.find(target.settingsPath) != std::string::npos &&
           joined.find(target.databasePath) != std::string::npos;
}

// --- TOML client (Codex reads config.toml) ----------------------------------

// Line-driven section handling: the connector entry is one plain table, so a
// minimal editor is enough and keeps every other byte of the user's file.
bool IsSectionHeader(const std::string& line) {
    const std::string trimmed = Trim(line);
    return !trimmed.empty() && trimmed.front() == '[';
}

std::string SectionName(const std::string& line) {
    const std::string trimmed = Trim(line);
    const auto open = trimmed.find('[');
    const auto close = trimmed.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close <= open) return {};
    return trimmed.substr(open + 1, close - open - 1);
}

bool IsHarkenSection(const std::string& name) {
    const std::size_t base = std::string("mcp_servers.").size();
    if (name.size() < base || name.compare(0, base, "mcp_servers.") != 0) return false;
    const std::string rest = name.substr(base);
    return rest == kServerKey || rest.rfind(std::string(kServerKey) + ".", 0) == 0;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char unit : text) {
        if (unit == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (unit != '\r') {
            current.push_back(unit);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out.push_back('\n');
    }
    return out;
}

std::vector<std::string> RemoveHarkenSections(const std::vector<std::string>& lines) {
    std::vector<std::string> kept;
    bool skipping = false;
    for (const auto& line : lines) {
        if (IsSectionHeader(line)) skipping = IsHarkenSection(SectionName(line));
        if (skipping) continue;
        kept.push_back(line);
    }
    while (!kept.empty() && Trim(kept.back()).empty()) kept.pop_back();
    return kept;
}

std::string TomlQuote(const std::string& value) {
    std::string out = "\"";
    for (const char unit : value) {
        if (unit == '\\') out += "\\\\";
        else if (unit == '"') out += "\\\"";
        else out.push_back(unit);
    }
    out += "\"";
    return out;
}

std::vector<std::string> HarkenSectionLines(const ExternalAiLinkTarget& target) {
    std::vector<std::string> lines;
    lines.push_back(std::string("[mcp_servers.") + kServerKey + "]");
    lines.push_back("command = " + TomlQuote(target.executablePath));
    lines.push_back("args = [" +
                    TomlQuote("--library-id") + ", " + TomlQuote(target.libraryId) + ", " +
                    TomlQuote("--library-db") + ", " + TomlQuote(target.databasePath) + ", " +
                    TomlQuote("--library-root") + ", " + TomlQuote(target.rootPath) + ", " +
                    TomlQuote("--settings") + ", " + TomlQuote(target.settingsPath) + "]");
    return lines;
}

bool TomlFileHasHarkenEntry(const std::string& text, const ExternalAiLinkTarget& target) {
    const auto lines = SplitLines(text);
    bool inside = false;
    bool commandMatches = false;
    bool argsMatch = false;
    const std::string quotedCommand = "command = " + TomlQuote(target.executablePath);
    for (const auto& line : lines) {
        if (IsSectionHeader(line)) {
            inside = SectionName(line) == std::string("mcp_servers.") + kServerKey;
            continue;
        }
        if (!inside) continue;
        const std::string trimmed = Trim(line);
        if (trimmed.rfind("command", 0) == 0) {
            commandMatches = trimmed == quotedCommand;
        } else if (trimmed.rfind("args", 0) == 0) {
            argsMatch = trimmed.find(target.libraryId) != std::string::npos &&
                        trimmed.find(TomlQuote(target.settingsPath)) != std::string::npos &&
                        trimmed.find(TomlQuote(target.databasePath)) != std::string::npos;
        }
    }
    return commandMatches && argsMatch;
}

}  // namespace

const char* ExternalAiClientKey(ExternalAiClient client) {
    return client == ExternalAiClient::Cindy ? "cindy" : "codex";
}

std::string ExternalAiClientConfigPath(ExternalAiClient client, const std::string& homeDirectory) {
    if (homeDirectory.empty()) return {};
    std::string base = NormalizeSlashes(homeDirectory);
    return client == ExternalAiClient::Cindy ? base + "\\.pi\\agent\\mcp.json"
                                             : base + "\\.codex\\config.toml";
}

bool ExternalAiClientDetected(ExternalAiClient client, const std::string& homeDirectory) {
    if (homeDirectory.empty()) return false;
    std::string base = NormalizeSlashes(homeDirectory);
    std::error_code code;
    const fs::path directory = fs::u8path(
        client == ExternalAiClient::Cindy ? base + "\\.pi" : base + "\\.codex");
    return fs::exists(directory, code) && fs::is_directory(directory, code);
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
                                               const ExternalAiLinkTarget& target) {
    ExternalAiLinkStatus status;
    status.configPath = configPath;
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        bool missing = false;
        std::string error;
        if (!ParseJsonFile(configPath, root, missing, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return status;
        }
        if (missing) {
            status.state = ExternalAiLinkState::Disconnected;
            return status;
        }
        const auto servers = root.find("mcpServers");
        status.state = servers != root.end() && JsonEntryMatches(*servers, target)
                           ? ExternalAiLinkState::Connected
                           : ExternalAiLinkState::Disconnected;
        return status;
    }
    std::string text;
    std::string error;
    if (!ReadFileUtf8(configPath, text, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return status;
    }
    status.state = TomlFileHasHarkenEntry(text, target) ? ExternalAiLinkState::Connected
                                                        : ExternalAiLinkState::Disconnected;
    return status;
}

bool LinkExternalAiClient(ExternalAiClient client, const std::string& configPath,
                          const ExternalAiLinkTarget& target, ExternalAiLinkStatus& status) {
    status = QueryExternalAiClientLink(client, configPath, target);
    if (status.state == ExternalAiLinkState::Connected) return true;
    std::string error;
    std::string backup;
    if (!BackupOnce(configPath, backup, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    status.detail = backup;
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        bool missing = false;
        if (!ParseJsonFile(configPath, root, missing, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        if (root.find("mcpServers") == root.end() || !root["mcpServers"].is_object()) {
            root["mcpServers"] = ordered_json::object();
        }
        root["mcpServers"][kServerKey] = BuildServerEntry(target);
        if (!WriteFileUtf8(configPath, root.dump(2) + "\n", error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
    } else {
        std::string text;
        if (!ReadFileUtf8(configPath, text, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        std::vector<std::string> lines = RemoveHarkenSections(SplitLines(text));
        const auto section = HarkenSectionLines(target);
        if (!lines.empty()) lines.push_back(std::string());
        lines.insert(lines.end(), section.begin(), section.end());
        if (!WriteFileUtf8(configPath, JoinLines(lines), error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
    }
    status = QueryExternalAiClientLink(client, configPath, target);
    return status.state == ExternalAiLinkState::Connected;
}

bool UnlinkExternalAiClient(ExternalAiClient client, const std::string& configPath,
                            ExternalAiLinkStatus& status) {
    status = ExternalAiLinkStatus{};
    status.configPath = configPath;
    std::error_code code;
    if (!fs::exists(fs::u8path(configPath), code)) {
        status.state = ExternalAiLinkState::Disconnected;
        return true;
    }
    std::string error;
    std::string backup;
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        bool missing = false;
        if (!ParseJsonFile(configPath, root, missing, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        const auto servers = root.find("mcpServers");
        if (servers == root.end() || !servers->is_object() || servers->find(kServerKey) == servers->end()) {
            status.state = ExternalAiLinkState::Disconnected;
            return true;
        }
        if (!BackupOnce(configPath, backup, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        root["mcpServers"].erase(kServerKey);
        if (!WriteFileUtf8(configPath, root.dump(2) + "\n", error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        status.state = ExternalAiLinkState::Disconnected;
        status.detail = backup;
        return true;
    }
    std::string text;
    if (!ReadFileUtf8(configPath, text, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    const auto lines = SplitLines(text);
    bool hasEntry = false;
    for (const auto& line : lines) {
        if (IsSectionHeader(line) && IsHarkenSection(SectionName(line))) {
            hasEntry = true;
            break;
        }
    }
    if (!hasEntry) {
        status.state = ExternalAiLinkState::Disconnected;
        return true;
    }
    if (!BackupOnce(configPath, backup, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    const auto kept = RemoveHarkenSections(lines);
    if (!WriteFileUtf8(configPath, kept.empty() ? std::string() : JoinLines(kept), error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    status.state = ExternalAiLinkState::Disconnected;
    status.detail = backup;
    return true;
}

}  // namespace HarkenSearch
