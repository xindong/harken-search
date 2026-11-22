#include "ExternalAiPlaylistBridge.h"

#include "nlohmann_json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace HarkenSearch {

namespace {

namespace fs = std::filesystem;

std::string NewReceiptId()
{
    static unsigned int counter = 0;
    return "playlist-receipt-" + std::to_string(++counter);
}

ExternalAiPlaylistWriteReceipt Refusal(const char* status, const std::string& reason)
{
    ExternalAiPlaylistWriteReceipt receipt;
    receipt.status = status;
    receipt.reason = reason;
    return receipt;
}

}  // namespace

bool IsValidExternalAiPlaylistPath(const std::string& playlistPath, std::string& reason)
{
    if (playlistPath.empty() || playlistPath.size() > 512) {
        reason = "playlist path must be a non-empty bounded string";
        return false;
    }
    std::size_t segments = 0;
    std::size_t start = 0;
    while (start <= playlistPath.size()) {
        const std::size_t slash = playlistPath.find('/', start);
        const std::string segment = playlistPath.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment.empty() || segment == "." || segment == "..") {
            reason = "playlist path must be one or two plain names";
            return false;
        }
        if (segment.find('\\') != std::string::npos || segment.find(':') != std::string::npos) {
            reason = "playlist path must not look like a filesystem path";
            return false;
        }
        ++segments;
        if (segments > 2) {
            reason = "playlist path has at most a parent and a child name";
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

void ExternalAiPlaylistReceiptStore::Load(const std::string& path)
{
    entries_.clear();
    std::ifstream stream(fs::u8path(path), std::ios::binary);
    if (!stream) {
        return;
    }
    nlohmann::json document = nlohmann::json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object() || !document.contains("receipts") ||
        !document["receipts"].is_array()) {
        return;
    }
    for (const auto& item : document["receipts"]) {
        if (!item.is_object() || !item.contains("requestId") || !item["requestId"].is_string()) {
            continue;
        }
        ExternalAiPlaylistWriteReceipt receipt;
        receipt.status = item.value("status", std::string());
        receipt.added = item.value("added", 0);
        receipt.skippedExisting = item.value("skippedExisting", 0);
        receipt.failed = item.value("failed", 0);
        receipt.receiptId = item.value("receiptId", std::string());
        receipt.reason = item.value("reason", std::string());
        entries_.push_back({item["requestId"].get<std::string>(), receipt});
    }
}

bool ExternalAiPlaylistReceiptStore::Save(const std::string& path) const
{
    nlohmann::json document;
    document["receipts"] = nlohmann::json::array();
    for (const auto& entry : entries_) {
        document["receipts"].push_back({
            {"requestId", entry.requestId},
            {"status", entry.receipt.status},
            {"added", entry.receipt.added},
            {"skippedExisting", entry.receipt.skippedExisting},
            {"failed", entry.receipt.failed},
            {"receiptId", entry.receipt.receiptId},
            {"reason", entry.receipt.reason},
        });
    }
    std::error_code code;
    const fs::path target = fs::u8path(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), code);
    }
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << document.dump(2) << "\n";
    return static_cast<bool>(stream.flush());
}

const ExternalAiPlaylistWriteReceipt* ExternalAiPlaylistReceiptStore::Find(const std::string& requestId) const
{
    for (const auto& entry : entries_) {
        if (entry.requestId == requestId) {
            return &entry.receipt;
        }
    }
    return nullptr;
}

void ExternalAiPlaylistReceiptStore::Record(const std::string& requestId,
                                            const ExternalAiPlaylistWriteReceipt& receipt)
{
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                       [&](const Entry& entry) { return entry.requestId == requestId; }),
        entries_.end());
    entries_.push_back({requestId, receipt});
    if (entries_.size() > kMaxReceipts) {
        entries_.erase(entries_.begin(),
            entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - kMaxReceipts));
    }
}

const ExternalAiPlaylistWriteReceipt* ExternalAiPlaylistReceiptStore::Latest() const
{
    return entries_.empty() ? nullptr : &entries_.back().receipt;
}

ExternalAiPlaylistWriteReceipt ApplyExternalAiPlaylistWrite(
    const ExternalAiAuthorization& authorization,
    const ExternalAiPlaylistWriteRequest& request,
    ExternalAiPlaylistWriter& writer,
    ExternalAiPlaylistReceiptStore& receipts)
{
    if (authorization.bridgeToken().empty() || request.token != authorization.bridgeToken()) {
        return Refusal("denied", "bridge_token_mismatch");
    }
    if (request.libraryId.empty() || request.libraryId != authorization.libraryId()) {
        return Refusal("stale", "library_mismatch");
    }
    if (!authorization.enabled()) {
        return Refusal("not_authorized", "switch_off");
    }
    if (request.requestId.empty() || request.requestId.size() > 128) {
        return Refusal("invalid", "request_id");
    }
    const bool grouped = !request.items.empty();
    if (grouped ? request.items.size() > 200 : (request.fileIds.empty() || request.fileIds.size() > 200)) {
        return Refusal("invalid", "file_ids");
    }
    std::string pathReason;
    if (!grouped && !IsValidExternalAiPlaylistPath(request.playlistPath, pathReason)) {
        return Refusal("invalid", pathReason);
    }
    if (grouped) {
        for (const auto& item : request.items) {
            if (!IsValidExternalAiPlaylistPath(item.playlistPath, pathReason)) {
                return Refusal("invalid", pathReason);
            }
        }
    }
    if (authorization.AccessFor(ExternalAiCapability::ReadPlaylist) == ExternalAiAccess::Denied ||
        authorization.AccessFor(ExternalAiCapability::EditPlaylist) != ExternalAiAccess::Editable) {
        return Refusal("not_authorized", "playlist_capability_closed");
    }

    // A retried request must not add a second copy: report the stored receipt.
    if (const auto* previous = receipts.Find(request.requestId)) {
        ExternalAiPlaylistWriteReceipt repeat = *previous;
        repeat.added = 0;
        repeat.repeated = true;
        return repeat;
    }

    // Validate every file before touching anything, so a single bad entry cannot
    // leave a half-applied batch.
    std::vector<int64_t> allFiles;
    if (grouped) {
        for (const auto& item : request.items) allFiles.push_back(item.fileId);
    } else {
        allFiles = request.fileIds;
    }
    for (const int64_t fileId : allFiles) {
        std::string reason;
        if (!writer.ValidateFile(fileId, reason)) {
            return Refusal("not_authorized", reason.empty() ? "file_not_authorized" : reason);
        }
    }

    // Group by the Playlist each file was classified into, so a confirmed preview
    // really lands in its child Playlists instead of one flat list.
    std::vector<std::pair<std::string, std::vector<int64_t>>> batches;
    if (grouped) {
        for (const auto& item : request.items) {
            auto found = std::find_if(batches.begin(), batches.end(),
                [&](const auto& batch) { return batch.first == item.playlistPath; });
            if (found == batches.end()) {
                batches.push_back({item.playlistPath, {item.fileId}});
            } else {
                found->second.push_back(item.fileId);
            }
        }
    } else {
        batches.push_back({request.playlistPath, request.fileIds});
    }

    int added = 0;
    int skippedExisting = 0;
    std::string reason;
    for (const auto& batch : batches) {
        int batchAdded = 0;
        int batchSkipped = 0;
        if (!writer.Apply(batch.first, batch.second, batchAdded, batchSkipped, reason)) {
            // Anything created earlier in this request goes away with the failure.
            writer.RollbackCreated();
            return Refusal("failed", reason.empty() ? "playlist_write_failed" : reason);
        }
        added += batchAdded;
        skippedExisting += batchSkipped;
    }

    ExternalAiPlaylistWriteReceipt receipt;
    receipt.status = "ok";
    receipt.added = added;
    receipt.skippedExisting = skippedExisting;
    receipt.failed = 0;
    receipt.receiptId = NewReceiptId();
    receipts.Record(request.requestId, receipt);
    return receipt;
}

}  // namespace HarkenSearch
