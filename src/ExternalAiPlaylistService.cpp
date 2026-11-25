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
        bool parentCreatedHere = false;
        if (parentId <= 0) {
            parentId = database_.createPlaylist(segments[0]);
            if (parentId <= 0) {
                reason = "playlist_create_failed";
                return false;
            }
            createdPlaylists_.push_back(parentId);
            parentCreatedHere = true;
        }
        if (parentCreatedHere) {
            // Ours, brand new: promoting it is not a change to existing data.
            if (!database_.isFolder(parentId) && !database_.markAsFolder(parentId)) {
                reason = "playlist_folder_failed";
                return false;
            }
        } else if (!database_.isFolder(parentId)) {
            // Converting an existing playlist into a folder silently changes what
            // the user already had. Refuse instead of mutating it.
            reason = "playlist_parent_not_folder";
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
        reason = result.error;
        return false;
    }
    added = result.addedFiles;
    skippedExisting = result.alreadyInPlaylist;
    return true;
}

void ExternalAiDatabasePlaylistWriter::RollbackCreated()
{
    // Reverse order: a child goes before the folder that holds it.
    for (auto it = createdPlaylists_.rbegin(); it != createdPlaylists_.rend(); ++it) {
        database_.deletePlaylist(*it);
    }
    createdPlaylists_.clear();
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
try {
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

    auto refuse = [&respond](const char* reason) {
        ExternalAiPlaylistWriteReceipt refused;
        refused.status = "invalid";
        refused.reason = reason;
        return respond(refused);
    };

    const json request = json::parse(requestJson, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        return refuse("request_not_an_object");
    }
    // Every field is type-checked before it is read: a malformed request from any
    // local process must return `invalid`, never throw into the UI thread.
    for (const char* field : {"token", "requestId", "libraryId", "playlistPath"}) {
        if (!request.contains(field) || !request[field].is_string()) {
            return refuse(field);
        }
    }
    ExternalAiPlaylistWriteRequest write;
    write.token = request["token"].get<std::string>();
    write.requestId = request["requestId"].get<std::string>();
    write.libraryId = request["libraryId"].get<std::string>();
    write.playlistPath = request["playlistPath"].get<std::string>();
    if (const auto clientLink = request.find("clientLink"); clientLink != request.end()) {
        if (!clientLink->is_object()) return refuse("clientLink");
        for (const char* field : {"executable", "database", "root", "config", "id"}) {
            if (!clientLink->contains(field) || !(*clientLink)[field].is_string()) {
                return refuse("clientLink");
            }
        }
        const auto& config = (*clientLink)["config"].get_ref<const std::string&>();
        const auto configPath = fs::u8path(config);
        if (!configPath.is_absolute()) return refuse("clientLink");
#ifdef _WIN32
        if (configPath.root_name().native().size() != 2 || configPath.root_name().native()[1] != L':') {
            return refuse("clientLink");
        }
#endif
        write.clientLink = ExternalAiLinkTarget{
            (*clientLink)["executable"].get<std::string>(), write.libraryId,
            (*clientLink)["database"].get<std::string>(), (*clientLink)["root"].get<std::string>(),
            authorizationPath, config, (*clientLink)["id"].get<std::string>(),
        };
    }
    if (request.contains("items")) {
        if (!request["items"].is_array()) {
            return refuse("items");
        }
        for (const auto& item : request["items"]) {
            if (!item.is_object() || !item.contains("fileId") || !item["fileId"].is_number_integer()
                || !item.contains("playlistPath") || !item["playlistPath"].is_string()) {
                return refuse("items");
            }
            write.items.push_back({item["fileId"].get<int64_t>(),
                item["playlistPath"].get<std::string>()});
        }
    }
    if (request.contains("fileIds")) {
        if (!request["fileIds"].is_array()) {
            return refuse("file_ids");
        }
        for (const auto& item : request["fileIds"]) {
            if (!item.is_number_integer()) {
                return refuse("file_ids");
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
} catch (...) {
    return R"({"status":"invalid","reason":"request_processing_failed"})";
}

}  // namespace HarkenSearch
