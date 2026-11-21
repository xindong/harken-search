#pragma once

#include "ExternalAiAuthorization.h"
#include "ExternalAiPlaylistBridge.h"

#include <string>
#include <vector>

class Database;  // global type in Database.h

namespace HarkenSearch {

// Database-backed half of the Playlist write bridge.
//
// Lives on the UI thread: the pipe only queues requests, so every database call
// here happens where the rest of the app writes. File authorization is decided
// per file against the same document the connector used, so an unticked or
// revoked directory can never be appended to.
class ExternalAiDatabasePlaylistWriter final : public ExternalAiPlaylistWriter {
public:
    ExternalAiDatabasePlaylistWriter(Database& database, const ExternalAiAuthorization& authorization,
                                     std::string libraryId);

    bool ValidateFile(int64_t fileId, std::string& reason) override;
    bool Apply(const std::string& playlistPath, const std::vector<int64_t>& fileIds,
               int& added, int& skippedExisting, std::string& reason) override;

private:
    Database& database_;
    const ExternalAiAuthorization& authorization_;
    std::string libraryId_;
    // Playlists this call created, so a failed batch can remove them again.
    std::vector<int64_t> createdPlaylists_;
};

// Handles one JSON request end to end: loads the authorization document, runs the
// bridge rules, performs the write and stores the receipt. Returns the response
// JSON the connector expects. Never throws.
std::string HandleExternalAiPlaylistWriteRequest(Database& database, const std::string& libraryId,
                                                 const std::string& authorizationPath,
                                                 const std::string& receiptsPath,
                                                 const std::string& requestJson);

// "<settingsDirectory>/external-ai/receipts.json"
std::string ExternalAiReceiptsPath(const std::string& settingsDirectory);

}  // namespace HarkenSearch
