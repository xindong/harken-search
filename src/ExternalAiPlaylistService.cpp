#include "ExternalAiPlaylistService.h"

#include "Database.h"
#include "ExternalAiAuthorizationStore.h"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace HarkenSearch {

namespace {

namespace fs = std::filesystem;

std::vector<std::string> SplitPlaylistPath(const std::string& playlistPath)
{
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= playlistPath.size()) {
        const std::size_t slash = playlistPath.find('/', start);
        segments.push_back(playlistPath.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return segments;
}

int64_t FindTopLevelPlaylist(Database& database, const std::string& name)
{
    for (const auto& playlist : database.getAllPlaylists(false)) {
        if (playlist.name == name && playlist.parent_id < 0) {
            return playlist.id;
        }
    }
    return -1;
}

int64_t FindChildPlaylist(Database& database, int64_t parentId, const std::string& name)
{
    for (const auto& playlist : database.getChildrenOf(parentId)) {
        if (playlist.name == name) {
            return playlist.id;
        }
    }
    return -1;
}

}  // namespace

ExternalAiDatabasePlaylistWriter::ExternalAiDatabasePlaylistWriter(
    Database& database, const ExternalAiAuthorization& authorization, std::string libraryId)
    : database_(database), authorization_(authorization), libraryId_(std::move(libraryId))
{
}

bool ExternalAiDatabasePlaylistWriter::ValidateFile(int64_t fileId, std::string& reason)
{
    if (fileId <= 0) {
        reason = "file_id_invalid";
        return false;
    }
    const AudioRecord record = database_.getFileById(fileId);
    if (record.id <= 0) {
        reason = "file_missing";
        return false;
    }
    // Edit rights on this exact path: inside a ticked root and granted editable.
    if (authorization_.Authorize(ExternalAiCapability::EditPlaylist, libraryId_, record.filePath) !=
        ExternalAiAccess::Editable) {
        reason = "path_outside_ticked_root";
        return false;
    }
    return true;
}

bool ExternalAiDatabasePlaylistWriter::Apply(const std::string& playlistPath,
                                             const std::vector<int64_t>& fileIds, int& added,
                                             int& skippedExisting, std::string& reason)
{
    const auto segments = SplitPlaylistPath(playlistPath);
    if (segments.empty() || segments.size() > 2) {
        reason = "playlist_path_depth";
        return false;
    }

    int64_t targetId = -1;
    if (segments.size() == 1) {
        targetId = FindTopLevelPlaylist(database_, segments[0]);
        if (targetId <= 0) {
            targetId = database_.createPlaylist(segments[0]);
            if (targetId <= 0) {
                reason = "playlist_create_failed";
                return false;
            }
            createdPlaylists_.push_back(targetId);
        }
    } else {
        int64_t parentId = FindTopLevelPlaylist(database_, segments[0]);
        if (parentId <= 0) {
            parentId = database_.createPlaylist(segments[0]);
            if (parentId <= 0) {
                reason = "playlist_create_failed";
                return false;
            }
            createdPlaylists_.push_back(parentId);
        }
        if (!database_.isFolder(parentId) && !database_.markAsFolder(parentId)) {
            reason = "playlist_folder_failed";
            return false;
        }
        targetId = FindChildPlaylist(database_, parentId, segments[1]);
        if (targetId <= 0) {
            targetId = database_.createSubPlaylist(parentId, segments[1]);
            if (targetId <= 0) {
                reason = "playlist_create_failed";
                return false;
            }
            createdPlaylists_.push_back(targetId);
        }
    }

    const auto result = database_.addManyToPlaylist(targetId, fileIds);
    if (!result.error.empty()) {
        // Compensate: drop the playlists this call created so a failed batch does
        // not leave an empty shell behind.
        for (auto it = createdPlaylists_.rbegin(); it != createdPlaylists_.rend(); ++it) {
            database_.deletePlaylist(*it);
        }
        createdPlaylists_.clear();
        reason = result.error;
        return false;
    }
    added = result.addedFiles;
    skippedExisting = result.alreadyInPlaylist;
    createdPlaylists_.clear();
    return true;
}

std::string ExternalAiReceiptsPath(const std::string& settingsDirectory)
{
    if (settingsDirectory.empty()) {
        return {};
    }
    std::string base = settingsDirectory;
    for (char& unit : base) {
        if (unit == '/') unit = '\\';
    }
    return base + "\\external-ai\\receipts.json";
}

std::string HandleExternalAiPlaylistWriteRequest(Database& database, const std::string& libraryId,
                                                 const std::string& authorizationPath,
                                                 const std::string& receiptsPath,
                                                 const std::string& requestJson)
{
    using nlohmann::json;
    auto respond = [](const ExternalAiPlaylistWriteReceipt& receipt) {
        json body = {
            {"status", receipt.status},
            {"added", receipt.added},
            {"skippedExisting", receipt.skippedExisting},
            {"failed", receipt.failed},
            {"receiptId", receipt.receiptId},
        };
        if (!receipt.reason.empty()) body["reason"] = receipt.reason;
        if (receipt.repeated) body["repeated"] = true;
        return body.dump();
    };

    const json request = json::parse(requestJson, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        ExternalAiPlaylistWriteReceipt refused;
        refused.status = "invalid";
        refused.reason = "request_not_an_object";
        return respond(refused);
    }

    ExternalAiPlaylistWriteRequest write;
    write.token = request.value("token", std::string());
    write.requestId = request.value("requestId", std::string());
    write.libraryId = request.value("libraryId", std::string());
    write.playlistPath = request.value("playlistPath", std::string());
    if (request.contains("fileIds") && request["fileIds"].is_array()) {
        for (const auto& item : request["fileIds"]) {
            if (!item.is_number_integer()) {
                ExternalAiPlaylistWriteReceipt refused;
                refused.status = "invalid";
                refused.reason = "file_ids";
                return respond(refused);
            }
            write.fileIds.push_back(item.get<int64_t>());
        }
    }

    std::string document;
    {
        std::ifstream stream(fs::u8path(authorizationPath), std::ios::binary);
        if (!stream) {
            ExternalAiPlaylistWriteReceipt refused;
            refused.status = "not_authorized";
            refused.reason = "authorization_unreadable";
            return respond(refused);
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        document = buffer.str();
    }
    ExternalAiAuthorization authorization;
    std::string error;
    if (!ExternalAiAuthorization::Parse(document, authorization, error)) {
        ExternalAiPlaylistWriteReceipt refused;
        refused.status = "not_authorized";
        refused.reason = "authorization_invalid";
        return respond(refused);
    }

    ExternalAiPlaylistReceiptStore receipts;
    receipts.Load(receiptsPath);
    ExternalAiDatabasePlaylistWriter writer(database, authorization, libraryId);
    const auto receipt = ApplyExternalAiPlaylistWrite(authorization, write, writer, receipts);
    if (receipt.status == "ok" && !receipt.repeated) {
        receipts.Save(receiptsPath);
    }
    return respond(receipt);
}

}  // namespace HarkenSearch
