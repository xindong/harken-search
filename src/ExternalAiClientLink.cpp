#include "ExternalAiClientLink.h"

#include "../thirdparty/nlohmann_json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace HarkenSearch {

namespace {

namespace fs = std::filesystem;
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
    std::error_code code;
    const auto file = fs::u8path(path);
    if (!fs::exists(file, code) && !code) {
        out.clear();
        return true;
    }
    std::ifstream stream(file, std::ios::binary);
    if (code || !stream) {
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
    const auto path = fs::u8path(configPath + kLinkSuffix);
    std::error_code code;
    if (!fs::is_regular_file(fs::symlink_status(path, code)) || code) return {};
    std::ifstream marker(path, std::ios::binary);
    std::string id(33, '\0');
    marker.read(id.data(), static_cast<std::streamsize>(id.size()));
    id.resize(static_cast<std::size_t>(marker.gcount()));
    return ValidLinkId(id) ? id : std::string{};
}

bool WriteFileUtf8(const std::string& path, const std::string& content, std::string& error) {
    std::error_code code;
    const fs::path target = fs::u8path(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), code);
    const fs::path pending = fs::u8path(path + ".harken-pending-" + NewLinkId());
    std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write " + path;
        return false;
    }
    stream << content;
    stream.close();
    bool written = stream.good();
    if (written) {
#ifdef _WIN32
        written = MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        fs::rename(pending, target, code);
        written = !code;
#endif
    }
    if (!written) {
        fs::remove(pending, code);
        error = "cannot write " + path;
        return false;
    }
    return true;
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

// Refuses files whose harken entry cannot be edited without risking the rest of
// the user's TOML (array-of-tables, inline tables). Failing here is safer than
// rewriting a file the user depends on.
bool TomlHasUnsupportedHarkenForm(const std::string& text) {
    if (text.rfind("\xEF\xBB\xBF", 0) == 0 ||
        text.find("\"\"\"") != std::string::npos || text.find("'''") != std::string::npos) return true;
    const auto lines = SplitLines(text);
    for (const auto& line : lines) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;
        if (trimmed.rfind("[[", 0) == 0 || trimmed.rfind("mcp_servers", 0) == 0) {
            return true;
        }
        if (trimmed.rfind("harken", 0) == 0 && trimmed.find('=') != std::string::npos) {
            return true;
        }
        auto key = Trim(trimmed.substr(0, trimmed.find('=')));
        if (trimmed.front() == '[') key = SectionName(trimmed);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char unit) {
            return unit == '"' || unit == '\'' || std::isspace(unit);
        }), key.end());
        if ((key == "mcp_servers" && trimmed.front() != '[') ||
            (IsHarkenSection(key) && (trimmed.front() != '[' || !IsHarkenSection(SectionName(trimmed))))) {
            return true;
        }
    }
    return false;
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
    std::string args = "args = [";
    bool first = true;
    for (const auto& value : BuildArgs(target)) {
        if (!first) args += ", ";
        args += TomlQuote(value.get<std::string>());
        first = false;
    }
    lines.push_back(args + "]");
    return lines;
}

ConfigEntry TomlEntry(const std::string& text) {
    const auto lines = SplitLines(text);
    bool inside = false;
    bool tableSeen = false;
    ConfigEntry entry;
    for (const auto& line : lines) {
        if (IsSectionHeader(line)) {
            const auto section = SectionName(line);
            inside = section == std::string("mcp_servers.") + kServerKey;
            entry.present = entry.present || IsHarkenSection(section);
            if (inside && tableSeen) return {true, false};
            tableSeen = tableSeen || inside;
            continue;
        }
        if (!inside) continue;
        const std::string trimmed = Trim(line);
        const auto separator = trimmed.find('=');
        if (separator == std::string::npos) continue;
        const auto key = Trim(trimmed.substr(0, separator));
        const auto value = Trim(trimmed.substr(separator + 1));
        if (key == "command") {
            if (!entry.command.is_null()) return {true, false};
            entry.command = ordered_json::parse(value, nullptr, false);
        } else if (key == "args") {
            if (!entry.args.is_null()) return {true, false};
            entry.args = ordered_json::parse(value, nullptr, false);
        } else if (key == "enabled" && value != "true") {
            entry.enabled = false;
        }
    }
    return entry;
}

bool ReadEntry(ExternalAiClient client, const std::string& path, ConfigEntry& entry, std::string& error) {
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        bool missing = false;
        if (!ParseJsonFile(path, root, missing, error)) return false;
        entry = JsonEntry(root);
    } else {
        std::string content;
        if (!ReadFileUtf8(path, content, error)) return false;
        if (TomlHasUnsupportedHarkenForm(content)) {
            error = "existing config uses a form this writer will not edit";
            return false;
        }
        entry = TomlEntry(content);
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
    if (!BackupOnce(configPath, backup, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    status.detail = backup;
    std::string updated;
    if (client == ExternalAiClient::Cindy) {
        ordered_json root;
        bool missing = false;
        if (!ParseJsonFile(configPath, root, missing, error)) {
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
        std::string text;
        if (!ReadFileUtf8(configPath, text, error)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = error;
            return false;
        }
        if (TomlHasUnsupportedHarkenForm(text)) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = "existing config uses a form this writer will not edit";
            return false;
        }
        const auto entry = TomlEntry(text);
        if (entry.present && !HarkenTarget(entry)) {
            status.state = ExternalAiLinkState::Conflict;
            status.detail = "harken is already used by an unrecognized entry";
            return false;
        }
        std::vector<std::string> lines = RemoveHarkenSections(SplitLines(text));
        const auto section = HarkenSectionLines(bound);
        if (!lines.empty()) lines.push_back(std::string());
        lines.insert(lines.end(), section.begin(), section.end());
        std::string joined = JoinLines(lines);
        const bool crlfSource = text.find("\r\n") != std::string::npos;
        if (crlfSource) {
            std::string crlf;
            crlf.reserve(joined.size() + lines.size());
            for (const char unit : joined) {
                if (unit == '\n') crlf.push_back('\r');
                crlf.push_back(unit);
            }
            joined.swap(crlf);
        }
        updated = std::move(joined);
    }
    if (!WriteFileUtf8(configPath + kLinkSuffix, bound.clientLinkId, error) ||
        !WriteFileUtf8(configPath, updated, error)) {
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
    std::error_code code;
    const auto marker = fs::u8path(configPath + kLinkSuffix);
    const auto markerStatus = fs::symlink_status(marker, code);
    if (fs::exists(markerStatus)) {
        if (!fs::is_regular_file(markerStatus) || ReadLinkId(configPath).empty()) {
            status.state = ExternalAiLinkState::Failed;
            status.detail = "cannot verify the client link marker";
            return false;
        }
        status.revoked = fs::remove(marker, code);
    }
    if (code && code != std::errc::no_such_file_or_directory) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = "cannot revoke the client link";
        return false;
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
        const auto entry = JsonEntry(root);
        if (!HarkenTarget(entry)) {
            status.state = entry.present && !status.revoked ? ExternalAiLinkState::Conflict : ExternalAiLinkState::Disconnected;
            return status.state == ExternalAiLinkState::Disconnected;
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
    if (TomlHasUnsupportedHarkenForm(text)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = "existing config uses a form this writer will not edit";
        return false;
    }
    const auto entry = TomlEntry(text);
    if (!HarkenTarget(entry)) {
        status.state = entry.present && !status.revoked ? ExternalAiLinkState::Conflict : ExternalAiLinkState::Disconnected;
        return status.state == ExternalAiLinkState::Disconnected;
    }
    if (!BackupOnce(configPath, backup, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    const auto kept = RemoveHarkenSections(SplitLines(text));
    auto joined = JoinLines(kept);
    if (text.find("\r\n") != std::string::npos) {
        std::string crlf;
        crlf.reserve(joined.size() + kept.size());
        for (const char unit : joined) {
            if (unit == '\n') crlf.push_back('\r');
            crlf.push_back(unit);
        }
        joined.swap(crlf);
    }
    if (!WriteFileUtf8(configPath, joined, error)) {
        status.state = ExternalAiLinkState::Failed;
        status.detail = error;
        return false;
    }
    status.state = ExternalAiLinkState::Disconnected;
    status.detail = backup;
    return true;
}

}  // namespace HarkenSearch
