#include "ExternalAiClientLink.h"
#include "ExternalAiClientConfigLocation.h"

#include "../thirdparty/nlohmann_json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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
    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Cindy, config.u8string(), target, status),
          "cindy: reconnect succeeds");
    Check(ReadAll(config) != linked, "cindy: reconnect issues a different launch authorization");
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
    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), target, status),
          "codex: reconnect succeeds");
    Check(ReadAll(config) != linked, "codex: reconnect issues a different launch authorization");
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

void TestConfigLocations(const fs::path& root)
{
    const auto home = root / "locations" / "home";
    const auto appData = home / "AppData" / "Roaming";
    const auto desktop = appData / "Cindy" / "codex-home";
    const auto standalone = home / ".codex";
    const auto overrideHome = home / "cli-override";
    const auto unrelated = appData / "Cindy" / "not-the-codex-home";
    fs::create_directories(desktop);
    fs::create_directories(standalone);
    fs::create_directories(overrideHome);
    fs::create_directories(unrelated);
    fs::create_directories(home / ".pi" / "agent");
    HarkenSearch::ExternalAiClientConfigInputs inputs{home.u8string(), appData.u8string()};
    const auto resolve = [&](ExternalAiClient client) {
        return HarkenSearch::ResolveExternalAiClientConfigLocation(client, inputs);
    };
    const auto isConfig = [](const HarkenSearch::ExternalAiClientConfigLocation& location, const fs::path& directory) {
        return location.detected && fs::u8path(location.configPath) == fs::canonical(directory) / "config.toml";
    };
    Check(isConfig(resolve(ExternalAiClient::Cindy), desktop), "location: Cindy uses its own Codex home");
    Check(isConfig(resolve(ExternalAiClient::Codex), standalone), "location: standalone Codex uses its default home");
    inputs.codexHome = desktop.u8string();
    Check(isConfig(resolve(ExternalAiClient::Cindy), desktop) &&
          isConfig(resolve(ExternalAiClient::Codex), standalone), "location: inherited Cindy CODEX_HOME is not the standalone config");
    inputs.codexHome = overrideHome.u8string();
    Check(isConfig(resolve(ExternalAiClient::Cindy), desktop) &&
          isConfig(resolve(ExternalAiClient::Codex), overrideHome), "location: a standalone override cannot redirect Cindy");
    inputs.codexHome = unrelated.u8string();
    Check(isConfig(resolve(ExternalAiClient::Cindy), desktop), "location: arbitrary Cindy subdirectory is not a managed Codex home");
    inputs.codexHome = (home / "missing-override").u8string();
    Check(resolve(ExternalAiClient::Codex).configPath.empty(), "location: invalid explicit override does not fall back");
    inputs.appData = (home / "without-cindy").u8string();
    Check(!resolve(ExternalAiClient::Cindy).detected, "location: standalone Pi is not Cindy desktop");
    inputs.isolated = true;
    inputs.isolationRoot = (root / "isolation").u8string();
    const auto isolatedProfile = fs::u8path(inputs.isolationRoot) / "profile";
    const auto isolatedHome = isolatedProfile / "home";
    const auto isolatedDesktop = isolatedProfile / "Roaming" / "Cindy" / "codex-home";
    fs::create_directories(isolatedHome / ".codex");
    fs::create_directories(isolatedHome / ".pi" / "agent");
    inputs.appData = appData.u8string();
    inputs.codexHome = overrideHome.u8string();
    Check(resolve(ExternalAiClient::Cindy).configPath.empty(), "location: isolated standalone Pi is not Cindy desktop");
    fs::create_directories(isolatedDesktop);
    Check(isConfig(resolve(ExternalAiClient::Cindy), isolatedDesktop) &&
          isConfig(resolve(ExternalAiClient::Codex), isolatedHome / ".codex"),
          "location: isolation only selects explicit fixture configs");
}

void TestDisconnectWithoutCurrentTarget(const fs::path& root, const ExternalAiLinkTarget& target)
{
    const auto config = root / "stale-draft" / "config.toml";
    ExternalAiLinkStatus status;
    Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), target, status),
          "stale: initial link succeeds");
    auto original = target;
    original.clientConfigPath = config.u8string();
    original.clientLinkId = ReadAll(fs::u8path(original.clientConfigPath + ".harken-link"));
    Check(HarkenSearch::ExternalAiClientLinkAuthorized(original), "stale: initial launch authorization is valid");
    auto currentDraft = target;
    currentDraft.rootPath.clear();
    Check(HarkenSearch::QueryExternalAiClientLink(ExternalAiClient::Codex, config.u8string(), currentDraft).canDisconnect,
          "stale: clearing the draft root does not hide Disconnect");
    currentDraft.libraryId = "lib-switched";
    currentDraft.databasePath = (root / "new.db").u8string();
    currentDraft.settingsPath = (root / "new.json").u8string();
    Check(HarkenSearch::QueryExternalAiClientLink(ExternalAiClient::Codex, config.u8string(), currentDraft).canDisconnect,
          "stale: switching libraries does not hide Disconnect");
    Check(HarkenSearch::UnlinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), status) &&
          !HarkenSearch::ExternalAiClientLinkAuthorized(original), "stale: disconnect invalidates original launch authorization");
}

void TestForeignEntriesAreUntouched(const fs::path& root, const ExternalAiLinkTarget& target)
{
    for (const auto client : {ExternalAiClient::Cindy, ExternalAiClient::Codex}) {
        const auto config = root / "foreign" / (client == ExternalAiClient::Cindy ? "mcp.json" : "config.toml");
        const auto foreign = client == ExternalAiClient::Cindy
            ? "{\"mcpServers\":{\"harken\":{\"command\":\"other.exe\",\"args\":[]}}}\n"
            : "[mcp_servers.harken]\r\ncommand = \"other.exe\"\r\nargs = []\r\n";
        WriteAll(config, foreign);
        ExternalAiLinkStatus status;
        Check(!HarkenSearch::QueryExternalAiClientLink(client, config.u8string(), target).canDisconnect,
              "foreign: a name collision is not a Harken connection");
        Check(!HarkenSearch::LinkExternalAiClient(client, config.u8string(), target, status) && ReadAll(config) == foreign,
              "foreign: connect refuses to overwrite another program using the harken key");
        HarkenSearch::UnlinkExternalAiClient(client, config.u8string(), status);
        Check(ReadAll(config) == foreign, "foreign: disconnect never removes another program's entry");
    }
}

void TestIndependentTomlClients(const fs::path& root, const ExternalAiLinkTarget& target)
{
    const auto cindyConfig = root / "dual" / "Cindy" / "codex-home" / "config.toml";
    const auto codexConfig = root / "dual" / ".codex" / "config.toml";
    const auto bind = [&](const fs::path& config) {
        ExternalAiLinkStatus status;
        Check(HarkenSearch::LinkExternalAiClient(ExternalAiClient::Codex, config.u8string(), target, status),
              "dual: TOML link succeeds");
        auto launch = target;
        launch.clientConfigPath = config.u8string();
        launch.clientLinkId = ReadAll(fs::u8path(launch.clientConfigPath + ".harken-link"));
        return launch;
    };
    const auto cindy = bind(cindyConfig);
    const auto codex = bind(codexConfig);
    Check(HarkenSearch::ExternalAiClientLinkAuthorized(cindy) &&
          HarkenSearch::ExternalAiClientLinkAuthorized(codex), "dual: both original TOML grants match");
    auto wrongLibrary = cindy;
    wrongLibrary.libraryId = "different-library";
    Check(!HarkenSearch::ExternalAiClientLinkAuthorized(wrongLibrary), "dual: a marker is not transferable to another library");
    ExternalAiLinkStatus status;
    Check(HarkenSearch::UnlinkExternalAiClient(ExternalAiClient::Codex, cindyConfig.u8string(), status) &&
          status.revoked && !HarkenSearch::ExternalAiClientLinkAuthorized(cindy) &&
          HarkenSearch::ExternalAiClientLinkAuthorized(codex), "dual: revoking Cindy does not revoke standalone Codex");
}

int RunFixture(const fs::path& fixture, std::string_view action)
{
    const auto input = nlohmann::json::parse(ReadAll(fixture));
    const auto client = input.at("client") == "cindy" ? ExternalAiClient::Cindy : ExternalAiClient::Codex;
    const auto config = input.at("config").get<std::string>();
    const ExternalAiLinkTarget target{input.at("executable"), input.at("library"), input.at("database"),
        input.at("root"), input.at("settings")};
    ExternalAiLinkStatus status;
    const bool ok = action == "link"
        ? HarkenSearch::LinkExternalAiClient(client, config, target, status)
        : HarkenSearch::UnlinkExternalAiClient(client, config, status);
    if (!ok) std::cerr << status.detail << '\n';
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--fixture") {
        return RunFixture(fs::u8path(argv[2]), argv[3]);
    }
    const fs::path root = fs::path(argc > 1 ? argv[1] : ".") / "external-ai-client-link";
    std::error_code code;
    fs::remove_all(root, code);
    fs::create_directories(root, code);

    const ExternalAiLinkTarget target = Target(root / "app" / "HarkenExternalAiMcp.exe");

    TestConfigLocations(root);
    TestCommandLine(target);
    TestCindyJsonMerge(root, target);
    TestCindyCreatesFile(root, target);
    TestCindyRejectsBrokenJson(root, target);
    TestCodexTomlMerge(root, target);
    TestDisconnectWithoutCurrentTarget(root, target);
    TestForeignEntriesAreUntouched(root, target);
    TestIndependentTomlClients(root, target);

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
