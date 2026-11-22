#pragma once

#include "ExternalAiAuthorization.h"

#include <cstdint>
#include <string>
#include <vector>

namespace HarkenSearch {

// Harken-side execution of one confirmed Playlist write.
//
// The connector never touches the database: it sends a request over a local pipe
// and this module decides. Everything except the actual database call lives here
// so the rules (token, library, capability, idempotency) can be unit tested
// without a pipe, a database or a running UI.
// One confirmed file and the Playlist it was classified into.
struct ExternalAiPlaylistTarget {
    int64_t fileId = 0;
    std::string playlistPath;
};

struct ExternalAiPlaylistWriteRequest {
    std::string token;
    std::string requestId;
    std::string libraryId;
    std::string playlistPath;  // "Game SFX" or "Game SFX/Combat & Ability"
    std::vector<int64_t> fileIds;                    // legacy: one playlist for all
    std::vector<ExternalAiPlaylistTarget> items;     // preferred: per-file playlist
};

struct ExternalAiPlaylistWriteReceipt {
    // ok | denied | not_authorized | stale | invalid | failed
    std::string status;
    int added = 0;
    int skippedExisting = 0;
    int failed = 0;
    std::string receiptId;
    std::string reason;
    bool repeated = false;
};

// The database-facing half. Implementations must validate before writing and
// apply the whole batch in one transaction.
class ExternalAiPlaylistWriter {
public:
    virtual ~ExternalAiPlaylistWriter() = default;

    // True only when the file exists in the open library and its path sits inside
    // a ticked, read-authorized root.
    virtual bool ValidateFile(int64_t fileId, std::string& reason) = 0;

    virtual bool Apply(const std::string& playlistPath, const std::vector<int64_t>& fileIds,
                       int& added, int& skippedExisting, std::string& reason) = 0;

    // Undo every playlist this request created, so a failed batch leaves nothing.
    virtual void RollbackCreated() = 0;
};

// Keeps the last write receipts so a retried request cannot add a second copy.
class ExternalAiPlaylistReceiptStore {
public:
    static constexpr std::size_t kMaxReceipts = 64;

    void Load(const std::string& path);
    bool Save(const std::string& path) const;

    const ExternalAiPlaylistWriteReceipt* Find(const std::string& requestId) const;
    void Record(const std::string& requestId, const ExternalAiPlaylistWriteReceipt& receipt);
    const ExternalAiPlaylistWriteReceipt* Latest() const;

private:
    struct Entry {
        std::string requestId;
        ExternalAiPlaylistWriteReceipt receipt;
    };
    std::vector<Entry> entries_;
};

// Decides one request. Returns `ok` and the receipt of the write, or a refusal
// with the reason. Nothing is written unless every file validates first.
ExternalAiPlaylistWriteReceipt ApplyExternalAiPlaylistWrite(
    const ExternalAiAuthorization& authorization,
    const ExternalAiPlaylistWriteRequest& request,
    ExternalAiPlaylistWriter& writer,
    ExternalAiPlaylistReceiptStore& receipts);

// "parent/child" shape check shared with the connector: one or two non-empty
// segments, no traversal, no absolute path, bounded length.
bool IsValidExternalAiPlaylistPath(const std::string& playlistPath, std::string& reason);

}  // namespace HarkenSearch
