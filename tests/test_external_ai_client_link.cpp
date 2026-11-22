#include "ExternalAiClientLink.h"

#include "../thirdparty/nlohmann_json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using HarkenSearch::ExternalAiClient;
using HarkenSearch::ExternalAiLinkState;
using HarkenSearch::ExternalAiLinkStatus;
using HarkenSearch::ExternalAiLinkTarget;

int failures = 0;

void Check(bool condition, const char* description)
{
    std::cout << (condition ? "PASS: " : "FAIL: ") << description << '\n';
    failures += !condition;
}

std::string ReadAll(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void WriteAll(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

ExternalAiLinkTarget Target(const fs::path& connector)
{
    ExternalAiLinkTarget target;
    target.executablePath = connector.u8string();
    target.libraryId = "lib-0123456789abcdef";
    target.databasePath = (connector.parent_path() / "library" / "main.db").u8string();
    target.rootPath = (connector.parent_path() / "library" / "audio").u8string();
    target.settingsPath = (connector.parent_path() / "settings" / "external-ai.json").u8string();
    return target;
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void TestCindyJsonMerge(const fs::path& root, const ExternalAiLinkTarget& target)
{
    const fs::path config = root / "cindy" / ".pi" / "agent" / "mcp.json";
    WriteAll(config, R"({
  "theme": "dark",
  "mcpServers": {
    "other": { "command": "other.exe", "args": [] }
  }
})");

    ExternalAiLinkStatus status = HarkenSearch::QueryExternalAiClientLink(
        ExternalAiClient::Cindy, config.u8string(), target);
    Check(status.state == ExternalAiLinkState::Disconnected, "cindy: seeded file starts disconnected");

    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Cindy, config.u8string(), target, status),
          "cindy: link succeeds");
    Check(status.state == ExternalAiLinkState::Connected, "cindy: state becomes connected");
    Check(fs::exists(config.u8string() + ".harken-backup"), "cindy: previous file backed up");

    const std::string linked = ReadAll(config);
    Check(Contains(linked, "\"other\""), "cindy: unrelated server kept");
    Check(Contains(linked, "other.exe"), "cindy: unrelated server command kept");
    Check(Contains(linked, "\"theme\""), "cindy: unrelated top-level key kept");
    Check(Contains(linked, "\"harken\""), "cindy: harken entry written");
    const auto parsed = nlohmann::json::parse(linked, nullptr, false);
    const auto entry = parsed.is_object() && parsed.contains("mcpServers") &&
                               parsed["mcpServers"].is_object() && parsed["mcpServers"].contains("harken")
                           ? parsed["mcpServers"]["harken"]
                           : nlohmann::json{};
    std::string joinedArgs;
    if (entry.is_object() && entry.contains("args") && entry["args"].is_array()) {
        for (const auto& item : entry["args"]) {
            if (item.is_string()) joinedArgs += item.get<std::string>(); joinedArgs += '|';
        }
    }
    Check(entry.is_object() && entry.contains("command") &&
              entry["command"].get<std::string>() == target.executablePath,
          "cindy: connector command written");
    Check(Contains(joinedArgs, target.libraryId), "cindy: library id written");
    Check(Contains(joinedArgs, target.settingsPath), "cindy: settings path written");
    Check(Contains(joinedArgs, target.databasePath), "cindy: database path written");

    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Cindy, config.u8string(), target, status),
          "cindy: second link is a no-op success");
    Check(ReadAll(config) == linked, "cindy: repeated link keeps the file unchanged");

    Check(HarkenSearch::UnlinkExternalAiClient(ExternalAiClient::Cindy, config.u8string(), status),
          "cindy: unlink succeeds");
    Check(status.state == ExternalAiLinkState::Disconnected, "cindy: unlink reports disconnected");
    const std::string unlinked = ReadAll(config);
    Check(!Contains(unlinked, "\"harken\""), "cindy: harken entry removed");
    Check(Contains(unlinked, "other.exe"), "cindy: unrelated server survives unlink");

    const std::string beforeSecond = unlinked;
    Check(HarkenSearch::UnlinkExternalAiClient(ExternalAiClient::Cindy, config.u8string(), status),
          "cindy: second unlink is a no-op success");
    Check(ReadAll(config) == beforeSecond, "cindy: second unlink keeps the file unchanged");
}

void TestCindyCreatesFile(const fs::path& root, const ExternalAiLinkTarget& target)
{
    const fs::path config = root / "cindy-new" / ".pi" / "agent" / "mcp.json";
    ExternalAiLinkStatus status = HarkenSearch::QueryExternalAiClientLink(
        ExternalAiClient::Cindy, config.u8string(), target);
    Check(status.state == ExternalAiLinkState::Disconnected, "cindy: missing file counts as disconnected");

    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Cindy, config.u8string(), target, status),
          "cindy: link creates the file");
    Check(fs::exists(config), "cindy: config file exists after link");
    Check(!fs::exists(config.u8string() + ".harken-backup"), "cindy: fresh file has nothing to back up");
}

void TestCindyRejectsBrokenJson(const fs::path& root, const ExternalAiLinkTarget& target)
{
    const fs::path config = root / "cindy-broken" / ".pi" / "agent" / "mcp.json";
    WriteAll(config, "{ this is not json");
    const std::string before = ReadAll(config);

    ExternalAiLinkStatus status;
    Check(!HarkenSearch::LinkExternalAiClient(ExternalAiClient::Cindy, config.u8string(), target, status),
          "cindy: broken json is refused");
    Check(status.state == ExternalAiLinkState::Failed, "cindy: broken json reports failure");
    Check(ReadAll(config) == before, "cindy: broken json left untouched");
}

void TestCodexTomlMerge(const fs::path& root, const ExternalAiLinkTarget& target)
{
    const fs::path config = root / "codex" / ".codex" / "config.toml";
    WriteAll(config,
             "model = \"gpt-5\"\n"
             "\n"
             "[mcp_servers.other]\n"
             "command = \"other.exe\"\n"
             "\n"
             "[profiles.fast]\n"
             "model = \"gpt-5-mini\"\n");

    ExternalAiLinkStatus status = HarkenSearch::QueryExternalAiClientLink(
        ExternalAiClient::Codex, config.u8string(), target);
    Check(status.state == ExternalAiLinkState::Disconnected, "codex: seeded file starts disconnected");

    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), target, status),
          "codex: link succeeds");
    Check(status.state == ExternalAiLinkState::Connected, "codex: state becomes connected");

    const std::string linked = ReadAll(config);
    Check(Contains(linked, "[mcp_servers.harken]"), "codex: harken section written");
    Check(Contains(linked, "other.exe"), "codex: other server kept");
    Check(Contains(linked, "[profiles.fast]"), "codex: unrelated section kept");
    Check(Contains(linked, "model = \"gpt-5\""), "codex: top-level keys kept");
    Check(Contains(linked, target.libraryId), "codex: library id written");

    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), target, status),
          "codex: second link is a no-op success");
    Check(ReadAll(config) == linked, "codex: repeated link keeps the file unchanged");

    Check(HarkenSearch::UnlinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), status),
          "codex: unlink succeeds");
    Check(status.state == ExternalAiLinkState::Disconnected, "codex: unlink reports disconnected");
    const std::string unlinked = ReadAll(config);
    Check(!Contains(unlinked, "[mcp_servers.harken]"), "codex: harken section removed");
    Check(Contains(unlinked, "other.exe"), "codex: other server survives unlink");
    Check(Contains(unlinked, "[profiles.fast]"), "codex: profiles survive unlink");
    Check(!Contains(unlinked, "\n\n\n"), "codex: no blank-line pile-up after unlink");

    const std::string beforeSecond = unlinked;
    Check(HarkenSearch::UnlinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), status),
          "codex: second unlink is a no-op success");
    Check(ReadAll(config) == beforeSecond, "codex: second unlink keeps the file unchanged");
}

void TestPathsAndDetection(const fs::path& root)
{
    const std::string home = root.u8string();
    const std::string cindy = HarkenSearch::ExternalAiClientConfigPath(ExternalAiClient::Cindy, home);
    const std::string codex = HarkenSearch::ExternalAiClientConfigPath(ExternalAiClient::Codex, home);
    Check(Contains(cindy, ".pi\\agent\\mcp.json"), "path: cindy uses pi mcp.json");
    Check(Contains(codex, ".codex\\config.toml"), "path: codex uses config.toml");
    Check(HarkenSearch::ExternalAiClientConfigPath(ExternalAiClient::Cindy, "").empty(),
          "path: empty home yields no path");
    Check(!HarkenSearch::ExternalAiClientDetected(ExternalAiClient::Cindy, root.u8string()),
          "detect: missing .pi means not detected");
    fs::create_directories(root / ".pi");
    Check(HarkenSearch::ExternalAiClientDetected(ExternalAiClient::Cindy, root.u8string()),
          "detect: existing .pi means detected");
}

void TestCommandLine(const ExternalAiLinkTarget& target)
{
    const std::string command = HarkenSearch::ExternalAiClientCommandLine(target);
    Check(Contains(command, "--library-id"), "command: keeps --library-id");
    Check(Contains(command, "--library-db"), "command: keeps --library-db");
    Check(Contains(command, "--library-root"), "command: keeps --library-root");
    Check(Contains(command, "--settings"), "command: keeps --settings");
    Check(Contains(command, target.executablePath), "command: points at the connector executable");
}

}  // namespace

int main(int argc, char** argv)
{
    const fs::path root = fs::path(argc > 1 ? argv[1] : ".") / "external-ai-client-link";
    std::error_code code;
    fs::remove_all(root, code);
    fs::create_directories(root, code);

    const ExternalAiLinkTarget target = Target(root / "app" / "HarkenExternalAiMcp.exe");

    TestPathsAndDetection(root);
    TestCommandLine(target);
    TestCindyJsonMerge(root, target);
    TestCindyCreatesFile(root, target);
    TestCindyRejectsBrokenJson(root, target);
    TestCodexTomlMerge(root, target);

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
