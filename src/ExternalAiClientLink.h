#pragma once

#include <string>

// One-click client links for the external AI connector.
//
// The settings page owns the authorization document; this module only turns one
// document into a client entry and back again. Every function takes the config
// file path explicitly so tests can run against a temporary directory instead of
// a real user profile.
namespace HarkenSearch {

enum class ExternalAiClient { Cindy, Codex };

enum class ExternalAiLinkState {
    NotDetected,   // the client's config directory does not exist yet
    Disconnected,  // detected, but no harken entry
    Connected,     // a harken entry already points at this library and connector
    Failed,        // the file exists but cannot be parsed or written
};

struct ExternalAiLinkTarget {
    std::string executablePath;  // HarkenExternalAiMcp.exe beside HarkenSearch.exe
    std::string libraryId;
    std::string databasePath;
    std::string rootPath;
    std::string settingsPath;
};

struct ExternalAiLinkStatus {
    ExternalAiLinkState state = ExternalAiLinkState::NotDetected;
    std::string configPath;
    std::string detail;  // backup path, or the reason a link attempt failed
};

const char* ExternalAiClientKey(ExternalAiClient client);

// Client config file under an explicit home directory:
//   Cindy -> <home>\.pi\agent\mcp.json
//   Codex -> <home>\.codex\config.toml
std::string ExternalAiClientConfigPath(ExternalAiClient client, const std::string& homeDirectory);

// Whether the client looks installed: its config directory exists under <home>.
bool ExternalAiClientDetected(ExternalAiClient client, const std::string& homeDirectory);

// Connector command line shared by the client entry and the copy fallback.
std::string ExternalAiClientCommandLine(const ExternalAiLinkTarget& target);

// Reads the current file state without modifying it. A missing file counts as
// Disconnected: the first link creates it.
ExternalAiLinkStatus QueryExternalAiClientLink(ExternalAiClient client, const std::string& configPath,
                                               const ExternalAiLinkTarget& target);

// Writes only the `harken` entry. The previous file is copied to
// `<configPath>.harken-backup` before the first modification. Entries the user
// added for other servers keep their values; JSON key order is preserved.
bool LinkExternalAiClient(ExternalAiClient client, const std::string& configPath,
                          const ExternalAiLinkTarget& target, ExternalAiLinkStatus& status);

// Removes only the `harken` entry and leaves every other entry untouched.
// Removing when no entry exists succeeds without touching the file.
bool UnlinkExternalAiClient(ExternalAiClient client, const std::string& configPath,
                            ExternalAiLinkStatus& status);

}  // namespace HarkenSearch
