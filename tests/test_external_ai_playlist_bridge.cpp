// Rules of the Harken-side Playlist write bridge, exercised without a pipe or a
// database: token, library, capability, per-file validation and idempotency.

#include "ExternalAiPlaylistBridge.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using HarkenSearch::ExternalAiAccess;
using HarkenSearch::ExternalAiAuthorization;
using HarkenSearch::ExternalAiCapability;
using HarkenSearch::ExternalAiPlaylistReceiptStore;
using HarkenSearch::ExternalAiPlaylistWriteReceipt;
using HarkenSearch::ExternalAiPlaylistWriteRequest;
using HarkenSearch::ExternalAiPlaylistWriter;

int failures = 0;

void Check(bool condition, const char* description)
{
    std::cout << (condition ? "PASS: " : "FAIL: ") << description << '\n';
    failures += !condition;
}

// Records what the bridge asked for and answers from a fixed script.
class FakeWriter final : public ExternalAiPlaylistWriter {
public:
    std::vector<int64_t> deniedFiles;
    bool applySucceeds = true;
    int addedPerCall = 3;
    int skippedPerCall = 1;
    int applyCalls = 0;
    std::vector<int64_t> lastBatch;

    bool ValidateFile(int64_t fileId, std::string& reason) override
    {
        for (const int64_t denied : deniedFiles) {
            if (denied == fileId) {
                reason = "path_outside_ticked_root";
                return false;
            }
        }
        return true;
    }

    int rollbacks = 0;

    void RollbackCreated() override { ++rollbacks; }

    bool Apply(const std::string&, const std::vector<int64_t>& fileIds, int& added,
               int& skippedExisting, std::string& reason) override
    {
        ++applyCalls;
        lastBatch = fileIds;
        if (!applySucceeds) {
            reason = "transaction_failed";
            return false;
        }
        added = addedPerCall;
        skippedExisting = skippedPerCall;
        return true;
    }
};

std::string EscapeJson(const std::string& value)
{
    std::string escaped;
    for (const char unit : value) {
        if (unit == '\\' || unit == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(unit);
    }
    return escaped;
}

std::string Document(const std::string& capabilities, const std::string& token,
                     const std::string& root)
{
    return std::string("{\"libraryId\":\"library-a\",\"enabled\":true,\"sampleLibrary\":false,")
        + "\"bridgeToken\":\"" + token + "\","
        + "\"roots\":[{\"path\":\"" + EscapeJson(root) + "\",\"enabled\":true}],"
        + "\"capabilities\":" + capabilities + "}";
}

const std::string kOpen = "{\"searchAudio\":\"read\",\"findSimilar\":\"read\",\"findFolders\":\"read\","
                          "\"readFileName\":\"read\",\"readMetadata\":\"read\",\"editMetadata\":\"read\","
                          "\"aiNamingMetadata\":\"read\",\"readPlaylist\":\"read\",\"editPlaylist\":\"edit\"}";

ExternalAiAuthorization Parse(const std::string& json)
{
    ExternalAiAuthorization authorization;
    std::string error;
    const bool ok = ExternalAiAuthorization::Parse(json, authorization, error);
    Check(ok, ("authorization parses: " + error).c_str());
    return authorization;
}

ExternalAiPlaylistWriteRequest Request(const char* token, const char* playlistPath,
                                       std::vector<int64_t> fileIds = {1, 2, 3})
{
    ExternalAiPlaylistWriteRequest request;
    request.token = token;
    request.requestId = "req-1";
    request.libraryId = "library-a";
    request.playlistPath = playlistPath;
    request.fileIds = std::move(fileIds);
    return request;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path scratch = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
    const std::string root = (scratch / "Sfx").u8string();
    const std::string token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const auto authorization = Parse(Document(kOpen, token, root));

    {
        FakeWriter writer;
        ExternalAiPlaylistReceiptStore receipts;
        auto request = Request(token.c_str(), "Game SFX");
        request.clientLink = HarkenSearch::ExternalAiLinkTarget{
            (scratch / "HarkenExternalAiMcp.exe").u8string(), request.libraryId,
            (scratch / "library.db").u8string(), root, (scratch / "grants.json").u8string(),
            (scratch / "playlist-bridge-revoked-client.toml").u8string(), "0123456789abcdef0123456789abcdef"};
        const auto receipt = HarkenSearch::ApplyExternalAiPlaylistWrite(authorization, request, writer, receipts);
        Check(receipt.status == "not_authorized" && receipt.reason == "client_connection_revoked" &&
            writer.applyCalls == 0, "a queued request cannot write after the managed client link was removed");
        request.clientLink.reset();
        Check(HarkenSearch::ApplyExternalAiPlaylistWrite(authorization, request, writer, receipts).status == "ok" &&
            writer.applyCalls == 1, "client-specific revocation does not disable a separately authorized manual client");
    }
    {
        FakeWriter writer;
        ExternalAiPlaylistReceiptStore receipts;
        const auto receipt = HarkenSearch::ApplyExternalAiPlaylistWrite(
            authorization, Request("wrong-token", "Game SFX"), writer, receipts);
        Check(receipt.status == "denied" && receipt.reason == "bridge_token_mismatch",
            "wrong bridge token is denied");
        Check(writer.applyCalls == 0, "denied request never reaches the writer");
    }
    {
        FakeWriter writer;
        ExternalAiPlaylistReceiptStore receipts;
        auto request = Request(token.c_str(), "Game SFX");
        request.libraryId = "library-b";
        const auto receipt = HarkenSearch::ApplyExternalAiPlaylistWrite(authorization, request, writer, receipts);
        Check(receipt.status == "stale", "another library is refused as stale");
    }
    {
        FakeWriter writer;
        ExternalAiPlaylistReceiptStore receipts;
        const auto receipt = HarkenSearch::ApplyExternalAiPlaylistWrite(
            authorization, Request(token.c_str(), "../escape"), writer, receipts);
        Check(receipt.status == "invalid", "traversal in the playlist path is refused");
    }
    {
        const auto closed = Parse(Document(
            "{\"readPlaylist\":\"read\",\"editPlaylist\":\"none\"}", token, root));
        FakeWriter writer;
        ExternalAiPlaylistReceiptStore receipts;
        const auto receipt = HarkenSearch::ApplyExternalAiPlaylistWrite(
            closed, Request(token.c_str(), "Game SFX"), writer, receipts);
        Check(receipt.status == "not_authorized" && receipt.reason == "playlist_capability_closed",
            "playlist write capability closed refuses");
        Check(writer.applyCalls == 0, "closed capability never reaches the writer");
    }
    {
        FakeWriter writer;
        writer.deniedFiles = {2};
        ExternalAiPlaylistReceiptStore receipts;
        const auto receipt = HarkenSearch::ApplyExternalAiPlaylistWrite(
            authorization, Request(token.c_str(), "Game SFX", {1, 2, 3}), writer, receipts);
        Check(receipt.status == "not_authorized" && receipt.reason == "path_outside_ticked_root",
            "one unauthorized file refuses the whole batch");
        Check(writer.applyCalls == 0, "unauthorized batch writes nothing");
    }
    {
        FakeWriter writer;
        writer.applySucceeds = false;
        ExternalAiPlaylistReceiptStore receipts;
        const auto receipt = HarkenSearch::ApplyExternalAiPlaylistWrite(
            authorization, Request(token.c_str(), "Game SFX"), writer, receipts);
        Check(receipt.status == "failed" && receipt.reason == "transaction_failed",
            "writer failure is reported as failed");
        Check(receipts.Latest() == nullptr, "a failed write records no receipt");
    }
    {
        FakeWriter writer;
        ExternalAiPlaylistReceiptStore receipts;
        const auto first = HarkenSearch::ApplyExternalAiPlaylistWrite(
            authorization, Request(token.c_str(), "Game SFX/Combat & Ability"), writer, receipts);
        Check(first.status == "ok" && first.added == 3 && first.skippedExisting == 1,
            "confirmed write reports what it added");
        Check(writer.lastBatch == std::vector<int64_t>({1, 2, 3}), "writer receives the whole batch");

        const auto again = HarkenSearch::ApplyExternalAiPlaylistWrite(
            authorization, Request(token.c_str(), "Game SFX/Combat & Ability"), writer, receipts);
        Check(again.status == "ok" && again.added == 0 && again.repeated,
            "a repeated request reports no new files");
        Check(again.receiptId == first.receiptId, "a repeated request keeps the original receipt");
        Check(writer.applyCalls == 1, "a repeated request does not reach the writer again");

        const std::filesystem::path receiptPath = scratch / "receipts.json";
        Check(receipts.Save(receiptPath.u8string()), "receipts save to disk");
        ExternalAiPlaylistReceiptStore reloaded;
        reloaded.Load(receiptPath.u8string());
        Check(reloaded.Find("req-1") != nullptr, "receipts reload with the request id");
        const auto afterReload = HarkenSearch::ApplyExternalAiPlaylistWrite(
            authorization, Request(token.c_str(), "Game SFX/Combat & Ability"), writer, reloaded);
        Check(afterReload.added == 0 && afterReload.repeated,
            "idempotency survives a restart");
    }

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
