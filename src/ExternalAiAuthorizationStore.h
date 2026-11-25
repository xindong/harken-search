#pragma once

#include "ExternalAiAuthorization.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace HarkenSearch {

// Number of capabilities in an authorization document. Kept next to the enum
// so the document, the settings page and the tests agree on the set.
constexpr std::size_t ExternalAiCapabilityCount = 9;

constexpr std::array<ExternalAiCapability, ExternalAiCapabilityCount> ExternalAiCapabilities{ {
    ExternalAiCapability::SearchAudio,
    ExternalAiCapability::FindSimilar,
    ExternalAiCapability::FindFolders,
    ExternalAiCapability::ReadFileName,
    ExternalAiCapability::ReadMetadata,
    ExternalAiCapability::EditMetadata,
    ExternalAiCapability::AiNamingMetadata,
    ExternalAiCapability::ReadPlaylist,
    ExternalAiCapability::EditPlaylist,
} };

// Stable document key of one capability, e.g. "searchAudio".
const char* ExternalAiCapabilityKey(ExternalAiCapability capability);
const char* ExternalAiAccessValue(ExternalAiAccess access);

// Editable form of the per-library authorization document. The saved document
// is exactly what the connector parses, so the settings page never keeps a
// second copy of the rules that could drift from the file.
struct ExternalAiSettings {
    std::string libraryId;
    bool enabled = false;
    bool sampleLibrary = false;
    std::vector<ExternalAiRoot> roots;
    // Empty until the page saves once; SaveExternalAiSettings then generates it.
    std::string bridgeToken;
    std::array<ExternalAiAccess, ExternalAiCapabilityCount> capabilities{ {
        ExternalAiAccess::Denied, ExternalAiAccess::Denied, ExternalAiAccess::Denied,
        ExternalAiAccess::Denied, ExternalAiAccess::Denied,
        ExternalAiAccess::Denied, ExternalAiAccess::Denied, ExternalAiAccess::Denied,
        ExternalAiAccess::Denied,
    } };

    ExternalAiAccess capability(ExternalAiCapability value) const;
};

std::string SerializeExternalAiSettings(const ExternalAiSettings& settings);

// Parses a document and checks that it belongs to expectedLibraryId. The result
// uses the same rules the connector applies, so a document the settings page
// can show is a document the connector can load.
bool ParseExternalAiSettings(const std::string& json, const std::string& expectedLibraryId,
    ExternalAiSettings& out, std::string& error);

// File that keeps one library's authorization document:
// <settingsDirectory>/external-ai/<libraryId>.json. Returns an empty string for
// a library identity that cannot name a file, so a bad identity never writes
// somewhere unexpected.
std::string ExternalAiSettingsPath(const std::string& settingsDirectory, const std::string& libraryId);
// Stable identity for one database file, shown on the settings page and passed
// to the connector as --library-id. Derived from the canonical database path so
// it needs no database open and no network round trip; the same file keeps the
// same identity across restarts and app versions. Path case and slash style are
// folded, so respelling the same location does not create a second identity.
// Returns an empty string for an empty path.
std::string ExternalAiLibraryIdForPath(const std::string& canonicalDatabasePathUtf8);

// A missing file is not an error: the library starts disabled with no ticked
// directory and every capability closed. A read or parse error returns false.
// existed (optional) reports whether a filesystem entry was found.
bool LoadExternalAiSettings(const std::string& path, const std::string& libraryId, ExternalAiSettings& out,
    std::string& error, bool* existed = nullptr);

// Writes the document atomically next to the target file.
bool SaveExternalAiSettings(const std::string& path, const ExternalAiSettings& settings, std::string& error);

bool SetExternalAiMasterSwitch(const std::string& path, ExternalAiSettings& draft, bool enabled, std::string& error);

struct ExternalAiLinkRoot {
    std::string path;
    bool spansVolumes = false;
};

ExternalAiLinkRoot ExternalAiLinkRootFor(const std::vector<ExternalAiRoot>& roots);

}  // namespace HarkenSearch
