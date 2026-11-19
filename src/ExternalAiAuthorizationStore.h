#pragma once

#include "ExternalAiAuthorization.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace HarkenSearch {

// Number of capabilities in an authorization document. Kept next to the enum
// so the document, the settings page and the tests agree on the set.
constexpr std::size_t ExternalAiCapabilityCount = 6;

constexpr std::array<ExternalAiCapability, ExternalAiCapabilityCount> ExternalAiCapabilities{ {
    ExternalAiCapability::SearchAudio,
    ExternalAiCapability::FindSimilar,
    ExternalAiCapability::FindFolders,
    ExternalAiCapability::ReadFileName,
    ExternalAiCapability::ReadMetadata,
    ExternalAiCapability::EditMetadata,
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
    std::vector<std::string> roots;
    std::array<ExternalAiAccess, ExternalAiCapabilityCount> capabilities{ {
        ExternalAiAccess::Denied, ExternalAiAccess::Denied, ExternalAiAccess::Denied,
        ExternalAiAccess::Denied, ExternalAiAccess::Denied, ExternalAiAccess::Denied,
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

// A missing file is not an error: the library starts disabled with no ticked
// directory and every capability closed. existed (optional) reports whether a
// document was found.
bool LoadExternalAiSettings(const std::string& path, const std::string& libraryId, ExternalAiSettings& out,
    std::string& error, bool* existed = nullptr);

// Writes the document atomically next to the target file.
bool SaveExternalAiSettings(const std::string& path, const ExternalAiSettings& settings, std::string& error);

}  // namespace HarkenSearch