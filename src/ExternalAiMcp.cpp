#include "ExternalAiAuthorization.h"
#include "ExternalAiLibrary.h"
#include "../thirdparty/nlohmann_json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;
using HarkenSearch::ExternalAiLibrary;

constexpr std::array<std::string_view, 4> ProtocolVersions{
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25",
};
constexpr std::size_t MaxLineBytes = 64 * 1024;
constexpr std::size_t MaxAuthorizationBytes = 256 * 1024;
constexpr int MaxJsonDepth = 32;
constexpr int MaxConsecutiveRejections = 16;
constexpr int MaxQueryLength = 256;
constexpr int DefaultLimit = 5;
constexpr int MaxLimit = 50;
constexpr int MaxCursorOffset = 4096;
constexpr std::size_t MaxCursors = 32;
constexpr auto MaxReferenceId = (std::numeric_limits<std::int64_t>::max)();

enum class ToolKind {
    SearchAudio,
    FindFolders,
    FindSimilar,
    GetMetadata,
    PreviewMetadataUpdate,
    ApplyMetadataUpdate,
    PreviewPlaylistPack,
    ApplyPlaylistPack,
};

struct ToolDefinition {
    const char* name;
    const char* description;
    ToolKind kind;

    const char* requiredArgument() const
    {
        if (kind == ToolKind::FindSimilar) return "reference_id";
        if (kind == ToolKind::SearchAudio || kind == ToolKind::FindFolders) return "query";
        if (kind == ToolKind::PreviewPlaylistPack) return "domain";
        if (kind == ToolKind::ApplyPlaylistPack) return "proposal_id";
        return "";
    }
};

constexpr std::array<ToolDefinition, 8> Tools{{
    {"harken_search_audio", "Find audio metadata by literal text in the authorized library.", ToolKind::SearchAudio},
    {"harken_find_folders", "Find folders by literal text in the authorized library.", ToolKind::FindFolders},
    {"harken_find_similar", "Find similar audio by file ID using the existing index. Match scores are ranking points, not probabilities.", ToolKind::FindSimilar},
    {"harken_get_metadata", "Read the current library metadata for one authorized audio file.", ToolKind::GetMetadata},
    {"harken_preview_metadata_update", "Preview a single audio file metadata proposal. This never writes.", ToolKind::PreviewMetadataUpdate},
    {"harken_apply_metadata_update", "Apply one previously previewed metadata proposal after explicit user confirmation.", ToolKind::ApplyMetadataUpdate},
    {"harken_preview_playlist_pack", "Preview a game SFX Playlist and child Playlist pack. This never writes.", ToolKind::PreviewPlaylistPack},
    {"harken_apply_playlist_pack", "Apply one confirmed game SFX Playlist pack when a safe Playlist write facade is available.", ToolKind::ApplyPlaylistPack},
}};

Json RpcResult(const Json& id, Json result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json RpcError(const Json& id, int code, const char* message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

struct LaunchConfig {
    std::string database;
    std::string root;
    std::string settings;
    std::string libraryId;

    std::string AbsolutePath(const std::string& relative) const
    {
        std::filesystem::path path = std::filesystem::u8path(root);
        if (!relative.empty()) {
            path /= std::filesystem::u8path(relative);
        }
        return path.u8string();
    }
};

HarkenSearch::ExternalAiCapability CapabilityFor(ToolKind kind)
{
    switch (kind) {
    case ToolKind::FindFolders:
        return HarkenSearch::ExternalAiCapability::FindFolders;
    case ToolKind::FindSimilar:
        return HarkenSearch::ExternalAiCapability::FindSimilar;
    case ToolKind::SearchAudio:
    default:
        return HarkenSearch::ExternalAiCapability::SearchAudio;
    }
}

bool CapabilityGranted(const HarkenSearch::ExternalAiAuthorization& authorization,
    HarkenSearch::ExternalAiCapability capability, const LaunchConfig& config)
{
    // A ticked root is always inside itself, so this checks the grant without
    // widening the managed scope to the whole library.
    for (const auto& root : authorization.roots()) {
        if (!root.enabled) continue;
        if (authorization.Authorize(capability, config.libraryId, root.path) != HarkenSearch::ExternalAiAccess::Denied) {
            return true;
        }
    }
    return false;
}

bool LoadAuthorization(const LaunchConfig& config, HarkenSearch::ExternalAiAuthorization& authorization, std::string& error)
{
    std::ifstream stream(std::filesystem::u8path(config.settings), std::ios::binary);
    if (!stream) {
        error = "authorization document is unreadable";
        return false;
    }
    std::string document;
    document.reserve(4096);
    for (int byte = stream.get(); byte != std::char_traits<char>::eof(); byte = stream.get()) {
        if (document.size() == MaxAuthorizationBytes) {
            error = "authorization document is too large";
            return false;
        }
        document.push_back(static_cast<char>(byte));
    }
    if (!HarkenSearch::ExternalAiAuthorization::Parse(document, authorization, error)) {
        return false;
    }
    if (authorization.libraryId() != config.libraryId) {
        error = "authorization document belongs to another library";
        return false;
    }
    return true;
}

bool AuthorizedRow(const HarkenSearch::ExternalAiAuthorization& authorization,
    HarkenSearch::ExternalAiCapability capability, const LaunchConfig& config, const Json& row)
{
    return row.is_object() && row.contains("relative_path") && row["relative_path"].is_string()
        && authorization.Authorize(capability, config.libraryId,
            config.AbsolutePath(row["relative_path"].get<std::string>()))
            != HarkenSearch::ExternalAiAccess::Denied;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char unit) {
        return static_cast<char>(std::tolower(unit));
    });
    return value;
}

bool ContainsAny(std::string_view text, std::initializer_list<std::string_view> terms)
{
    for (const auto term : terms) {
        if (text.find(term) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

Json GameSfxProfile()
{
    return {
        {"version", "game_sfx.v1"},
        {"domain", "game_sfx"},
        {"definition", "Game sounds attached to player, character, combat, world, UI, environment, cinematic or narrative interaction events."},
        {"taxonomy", Json::array({
            "Gameplay Feedback", "Character & Creature", "Combat & Ability", "World & Props",
            "UI & System", "Environment & Foley", "Cinematic & Transition", "Review Needed"})},
        {"style_separate", true},
        {"evidence_precedence", Json::array({
            "pack_vendor_prior", "filename_metadata", "audio_evidence"})},
        {"excluded", Json::array({
            "music", "stems", "BPM", "tempo", "key", "complete tracks"})},
        {"output_fields", Json::array({
            "playlist_path", "confidence", "reason", "source", "needs_review"})},
    };
}

Json ClassifyGameSfx(const Json& row, const Json& metadata)
{
    std::string evidence = LowerAscii(row.value("name", std::string()) + " "
        + row.value("relative_path", std::string()));
    if (metadata.is_object()) {
        const auto current = metadata.value("metadata", Json::object());
        evidence += " " + LowerAscii(current.value("title", std::string()));
        evidence += " " + LowerAscii(current.value("description", std::string()));
        evidence += " " + LowerAscii(current.value("category", std::string()));
        evidence += " " + LowerAscii(current.value("keywords", std::string()));
    }
    if (ContainsAny(evidence, {"soundtrack", "music", "score", "song", "stem", "stems",
        "bpm", "tempo", "key signature", "key_signature", "key-", "key_", "_key", "-key",
        "full track", "complete track", "album"})) {
        return { {"excluded", true}, {"needs_review", false}, {"reason", "Music or music-production evidence excludes this item from game_sfx."} };
    }

    std::string taxonomy = "Review Needed";
    std::string reason = "No sufficiently specific game SFX interaction evidence was found.";
    double confidence = 0.35;
    if (ContainsAny(evidence, {"ui", "menu", "button", "click", "hover", "confirm", "cancel", "select", "notification", "pause", "save", "load", "error", "alert"})) {
        taxonomy = "UI & System";
        reason = "Filename or metadata indicates UI or system feedback.";
        confidence = 0.9;
    } else if (ContainsAny(evidence, {"attack", "weapon", "sword", "gun", "shot", "explosion", "magic", "spell", "ability", "skill", "projectile", "damage", "impact"})) {
        taxonomy = "Combat & Ability";
        reason = "Filename or metadata indicates combat, weapon or ability feedback.";
        confidence = 0.9;
    } else if (ContainsAny(evidence, {"footstep", "footsteps", "jump", "land", "grunt", "breath", "voice", "animal", "monster", "creature", "character"})) {
        taxonomy = "Character & Creature";
        reason = "Filename or metadata indicates a character or creature action.";
        confidence = 0.85;
    } else if (ContainsAny(evidence, {"door", "chest", "switch", "lever", "crate", "wood", "metal", "machine", "vehicle", "prop", "pickup"})) {
        taxonomy = "World & Props";
        reason = "Filename or metadata indicates a world object or interaction.";
        confidence = 0.8;
    } else if (ContainsAny(evidence, {"wind", "rain", "water", "fire", "forest", "cave", "ambience", "ambient", "rustle", "foley"})) {
        taxonomy = "Environment & Foley";
        reason = "Filename or metadata indicates environmental Foley or authored ambience.";
        confidence = 0.8;
    } else if (ContainsAny(evidence, {"whoosh", "transition", "riser", "stinger", "reveal", "cinematic", "swoosh"})) {
        taxonomy = "Cinematic & Transition";
        reason = "Filename or metadata indicates a cinematic or transition cue.";
        confidence = 0.8;
    } else if (ContainsAny(evidence, {"hit", "pickup", "success", "failure", "feedback", "foley"})) {
        taxonomy = "Gameplay Feedback";
        reason = "Filename or metadata indicates gameplay feedback.";
        confidence = 0.7;
    }
    return {{"excluded", false}, {"taxonomy", taxonomy}, {"confidence", confidence},
        {"reason", reason}, {"source", "filename_metadata"}, {"needs_review", taxonomy == "Review Needed"}};
}

std::vector<std::string> DefaultResultFields(ToolKind kind)
{
    if (kind == ToolKind::FindFolders) {
        return {"relative_path", "file_count"};
    }
    if (kind == ToolKind::FindSimilar) {
        return {"file_id", "name", "relative_path", "duration_seconds", "match_score"};
    }
    return {"file_id", "name", "relative_path", "duration_seconds"};
}

std::optional<std::vector<std::string>> ParseResultFields(const Json& arguments, ToolKind kind)
{
    const auto defaults = DefaultResultFields(kind);
    if (!arguments.contains("fields")) {
        return defaults;
    }
    const auto& fields = arguments["fields"];
    if (!fields.is_array() || fields.empty() || fields.size() > defaults.size()) {
        return std::nullopt;
    }
    std::vector<std::string> selected;
    selected.reserve(fields.size());
    for (const auto& field : fields) {
        if (!field.is_string()) {
            return std::nullopt;
        }
        const auto name = field.get<std::string>();
        if (std::find(defaults.begin(), defaults.end(), name) != defaults.end()
            && std::find(selected.begin(), selected.end(), name) == selected.end()) {
            selected.push_back(name);
        } else {
            return std::nullopt;
        }
    }
    return selected;
}

Json ProjectResultFields(Json payload, const std::vector<std::string>& fields)
{
    if (!payload.contains("results") || !payload["results"].is_array()) {
        return payload;
    }
    Json projected = Json::array();
    for (const auto& row : payload["results"]) {
        if (!row.is_object()) {
            continue;
        }
        Json selected = Json::object();
        for (const auto& field : fields) {
            if (row.contains(field)) {
                selected[field] = row[field];
            }
        }
        projected.push_back(std::move(selected));
    }
    payload["results"] = std::move(projected);
    return payload;
}

bool FailedStatus(const Json& payload)
{
    if (!payload.is_object() || !payload.contains("status") || !payload["status"].is_string()) {
        return false;
    }
    const auto& status = payload["status"].get_ref<const std::string&>();
    return status == "query_failed" || status == "not_authorized" || status == "reference_not_authorized"
        || status == "authorization_unavailable" || status == "reference_not_found"
        || status == "proposal_not_found" || status == "proposal_stale"
        || status == "invalid_fields" || status == "write_failed"
        || status == "confirmation_required" || status == "playlist_write_unavailable";
}

bool HasOnlyFields(const Json& object, std::initializer_list<std::string_view> fields)
{
    if (!object.is_object()) {
        return false;
    }
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (std::find(fields.begin(), fields.end(), std::string_view(field.key())) == fields.end()) {
            return false;
        }
    }
    return true;
}

bool ValidParams(const Json& params)
{
    return params.is_object() && (!params.contains("_meta") || params["_meta"].is_object());
}

std::optional<std::int64_t> PositiveInteger(const Json& value, std::int64_t maximum)
{
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number != 0 && number <= static_cast<std::uint64_t>(maximum)) {
            return static_cast<std::int64_t>(number);
        }
    } else if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number > 0 && number <= maximum) {
            return number;
        }
    }
    return std::nullopt;
}

bool ValidQuery(const std::string& query)
{
    int length = 0;
    bool hasText = false;
    for (unsigned char byte : query) {
        if (byte < 0x20 || byte == 0x7f) {
            return false;
        }
        if ((byte & 0xc0) != 0x80 && ++length > MaxQueryLength) {
            return false;
        }
        hasText = hasText || byte != ' ';
    }
    return hasText;
}

bool ValidPlaylistLabel(const std::string& value)
{
    if (value.empty() || value.size() > 128 || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char unit) {
        return unit >= 0x20 && unit != 0x7f && unit != '/' && unit != '\\' && unit != ':';
    });
}

Json ListTools()
{
    Json list = Json::array();
    for (const ToolDefinition& tool : Tools) {
        Json schema = {
            {"type", "object"},
            {"additionalProperties", false},
        };
        Json annotations = {
            {"readOnlyHint", true},
            {"destructiveHint", false},
            {"openWorldHint", false},
        };
        if (tool.kind == ToolKind::SearchAudio || tool.kind == ToolKind::FindFolders) {
            schema["required"] = Json::array({tool.requiredArgument()});
            const auto fields = DefaultResultFields(tool.kind);
            Json fieldNames = Json::array();
            for (const auto& field : fields) {
                fieldNames.push_back(field);
            }
            schema["properties"] = {
                {tool.requiredArgument(), {{"type", "string"}, {"minLength", 1}, {"maxLength", MaxQueryLength}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxLimit}, {"default", DefaultLimit}}},
                {"cursor", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"fields", { {"type", "array"}, {"minItems", 1}, {"maxItems", fields.size()},
                    {"uniqueItems", true}, {"items", {{"type", "string"}, {"enum", std::move(fieldNames)}}} }},
            };
        } else if (tool.kind == ToolKind::FindSimilar) {
            schema["required"] = Json::array({"reference_id"});
            const auto fields = DefaultResultFields(tool.kind);
            Json fieldNames = Json::array();
            for (const auto& field : fields) {
                fieldNames.push_back(field);
            }
            schema["properties"] = {
                {"reference_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxReferenceId}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxLimit}, {"default", DefaultLimit}}},
                {"cursor", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"fields", { {"type", "array"}, {"minItems", 1}, {"maxItems", fields.size()},
                    {"uniqueItems", true}, {"items", {{"type", "string"}, {"enum", std::move(fieldNames)}}} }},
            };
        } else if (tool.kind == ToolKind::PreviewPlaylistPack) {
            schema["required"] = Json::array({"domain", "playlist"});
            schema["properties"] = {
                {"domain", {{"type", "string"}, {"enum", Json::array({"game_sfx"})}}},
                {"style", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}, {"default", "realistic"}}},
                {"theme", {{"type", "string"}, {"maxLength", 128}}},
                {"playlist", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"query", {{"type", "string"}, {"minLength", 1}, {"maxLength", MaxQueryLength}}},
                {"file_ids", {{"type", "array"}, {"minItems", 1}, {"maxItems", MaxLimit},
                    {"uniqueItems", true}, {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxReferenceId}}}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxLimit}, {"default", MaxLimit}}},
            };
        } else if (tool.kind == ToolKind::ApplyPlaylistPack) {
            schema["required"] = Json::array({"proposal_id", "confirm"});
            schema["properties"] = {
                {"proposal_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"confirm", {{"type", "boolean"}}},
            };
            annotations["readOnlyHint"] = false;
            annotations["destructiveHint"] = true;
        } else if (tool.kind == ToolKind::GetMetadata || tool.kind == ToolKind::PreviewMetadataUpdate) {
            schema["required"] = tool.kind == ToolKind::PreviewMetadataUpdate
                ? Json::array({"file_id", "fields"})
                : Json::array({"file_id"});
            schema["properties"] = {
                {"file_id", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxReferenceId}}},
            };
            if (tool.kind == ToolKind::PreviewMetadataUpdate) {
                schema["properties"]["fields"] = {
                    {"type", "object"},
                    {"minProperties", 1},
                    {"maxProperties", 4},
                    {"additionalProperties", {{"type", "string"}, {"maxLength", 4096}}},
                };
            }
        } else if (tool.kind == ToolKind::ApplyMetadataUpdate) {
            schema["required"] = Json::array({"proposal_id"});
            schema["properties"] = {
                {"proposal_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
            };
            annotations["readOnlyHint"] = false;
            annotations["destructiveHint"] = true;
        }
        list.push_back({
            {"name", tool.name},
            {"description", tool.description},
            {"annotations", std::move(annotations)},
            {"inputSchema", std::move(schema)},
        });
    }
    return {{"tools", std::move(list)}};
}

bool ValidMetadataFields(const Json& fields)
{
    static const std::array<std::string_view, 4> allowed{
        "title", "description", "category", "keywords",
    };
    if (!fields.is_object() || fields.empty() || fields.size() > allowed.size()) {
        return false;
    }
    for (auto entry = fields.begin(); entry != fields.end(); ++entry) {
        if (std::find(allowed.begin(), allowed.end(), std::string_view(entry.key())) == allowed.end()
            || !entry.value().is_string()
            || entry.value().get_ref<const std::string&>().size() > 4096) {
            return false;
        }
    }
    return true;
}

bool HasAccess(HarkenSearch::ExternalAiAccess access, bool editable)
{
    return editable ? access == HarkenSearch::ExternalAiAccess::Editable
                    : access != HarkenSearch::ExternalAiAccess::Denied;
}

class Session {
public:
    Session(ExternalAiLibrary& library, const LaunchConfig& config) : library_(library), config_(config) {}

    std::optional<Json> Dispatch(const Json& message)
    {
        if (!message.is_object() || !message.contains("jsonrpc") || message["jsonrpc"] != "2.0"
            || !message.contains("method") || !message["method"].is_string() || message["method"] == "") {
            return RpcError(nullptr, -32600, "Invalid request");
        }

        static const Json noParams = Json::object();
        const Json& params = message.contains("params") ? message["params"] : noParams;
        const auto& method = message["method"].get_ref<const std::string&>();
        const bool validFields = HasOnlyFields(message, {"jsonrpc", "id", "method", "params"});
        const auto id = message.find("id");
        if (id == message.end()) {
            if (method == "notifications/initialized" && phase_ == Phase::AwaitingNotification
                && validFields && ValidParams(params) && HasOnlyFields(params, {"_meta"})) {
                phase_ = Phase::Ready;
            }
            return std::nullopt;
        }
        if (!id->is_string() && !id->is_number_integer()) {
            return RpcError(nullptr, -32600, "Invalid request ID");
        }
        if (!validFields) {
            return RpcError(*id, -32600, "Invalid request");
        }
        if (!ValidParams(params)) {
            return RpcError(*id, -32602, "Invalid parameters");
        }
        if (method == "ping" || method == "tools/list") {
            if (!HasOnlyFields(params, {"_meta"})) {
                return RpcError(*id, -32602, "Invalid parameters");
            }
            if (method == "ping") {
                return RpcResult(*id, Json::object());
            }
        }
        if (method == "initialize") {
            return Initialize(*id, params);
        }
        if (method == "tools/list" || method == "tools/call") {
            if (phase_ != Phase::Ready) {
                return RpcError(*id, -32002, "Server not initialized");
            }
            return method == "tools/list" ? RpcResult(*id, ListTools()) : CallTool(*id, params);
        }
        return RpcError(*id, -32601, "Method not found");
    }

private:
    enum class Phase { AwaitingInitialize, AwaitingNotification, Ready };

    Json Initialize(const Json& id, const Json& params)
    {
        if (phase_ != Phase::AwaitingInitialize) {
            return RpcError(id, -32600, "Server already initialized");
        }
        const auto client = params.find("clientInfo");
        if (!HasOnlyFields(params, {"protocolVersion", "capabilities", "clientInfo", "_meta"})
            || !params.contains("protocolVersion") || !params["protocolVersion"].is_string()
            || params["protocolVersion"] == "" || !params.contains("capabilities") || !params["capabilities"].is_object()
            || client == params.end() || !client->is_object()
            || !client->contains("name") || !(*client)["name"].is_string()
            || !client->contains("version") || !(*client)["version"].is_string()) {
            return RpcError(id, -32602, "Invalid initialization parameters");
        }

        const auto& requested = params["protocolVersion"].get_ref<const std::string&>();
        const auto supported = std::find(ProtocolVersions.begin(), ProtocolVersions.end(), requested);
        const auto selected = supported == ProtocolVersions.end() ? ProtocolVersions.back() : *supported;
        structuredContent_ = selected >= "2025-06-18";
        phase_ = Phase::AwaitingNotification;
        return RpcResult(id, {
            {"protocolVersion", selected},
            {"capabilities", {{"tools", Json::object()}}},
            {"serverInfo", {{"name", "HarkenExternalAiMcp"}, {"version", "1.0.0"}}},
        });
    }

    struct CursorState {
        ToolKind kind;
        std::string key;
        std::vector<std::string> fields;
        int offset = 0;
        Json pending = Json::array();
        bool backendMore = false;
    };

    struct PendingPlaylistSelection {
        std::int64_t fileId = 0;
        std::string relativePath;
        Json metadata;
        Json selection;
    };

    struct PendingPlaylistProposal {
        std::string libraryId;
        std::string domain;
        std::string style;
        std::string theme;
        std::string playlist;
        std::vector<PendingPlaylistSelection> selections;
    };

    std::string CreateCursor(CursorState state)
    {
        if (cursors_.size() >= MaxCursors) {
            cursors_.erase(cursors_.begin());
        }
        const auto token = "cursor-" + std::to_string(nextCursorId_++);
        cursors_.emplace(token, std::move(state));
        return token;
    }

    Json QueryTool(const Json& id, ToolKind kind, const std::string& key, int limit,
        const std::vector<std::string>& fields, const HarkenSearch::ExternalAiAuthorization& authorization,
        HarkenSearch::ExternalAiCapability capability, CursorState state)
    {
        int offset = state.offset;
        Json pending = std::move(state.pending);
        bool backendMore = state.backendMore;
        Json pageRows = Json::array();

        while (pageRows.size() < static_cast<std::size_t>(limit)) {
            Json batch;
            bool fullBatch = false;
            if (pending.is_array() && !pending.empty()) {
                batch = {{"status", "ok"}, {"results", std::move(pending)}};
                pending = Json::array();
                fullBatch = backendMore;
                backendMore = false;
            } else {
                if (offset < 0 || offset > MaxCursorOffset) {
                    return ToolResult(id, {{"status", "query_failed"}, {"results", Json::array()}});
                }
                const auto fetchLimit = (std::min)(MaxLimit, limit + 1);
                if (kind == ToolKind::SearchAudio) {
                    batch = library_.SearchAudio(key, fetchLimit, offset);
                } else if (kind == ToolKind::FindFolders) {
                    batch = library_.FindFolders(key, fetchLimit, offset);
                } else {
                    batch = library_.FindSimilar(std::stoll(key), fetchLimit, offset);
                }
                if (!batch.is_object() || !batch.contains("status") || !batch["status"].is_string()) {
                    return ToolResult(id, {{"status", "query_failed"}, {"results", Json::array()}});
                }
                if (batch["status"] != "ok") {
                    if (batch["status"] == "no_candidates" && !pageRows.empty()) {
                        break;
                    }
                    return ToolResult(id, std::move(batch));
                }
                if (!batch.contains("results") || !batch["results"].is_array()) {
                    return ToolResult(id, {{"status", "query_failed"}, {"results", Json::array()}});
                }
                const auto batchSize = batch["results"].size();
                fullBatch = batchSize == static_cast<std::size_t>(fetchLimit);
                offset += static_cast<int>(batchSize);
            }

            const auto& rows = batch["results"];
            std::size_t leftoverStart = rows.size();
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (pageRows.size() >= static_cast<std::size_t>(limit)) {
                    leftoverStart = index;
                    break;
                }
                if (AuthorizedRow(authorization, capability, config_, rows[index])) {
                    pageRows.push_back(rows[index]);
                }
            }

            if (pageRows.size() >= static_cast<std::size_t>(limit)) {
                Json leftover = Json::array();
                for (std::size_t index = leftoverStart; index < rows.size(); ++index) {
                    leftover.push_back(rows[index]);
                }
                if (!leftover.empty() || fullBatch) {
                    Json payload = {{"status", "ok"}, {"results", std::move(pageRows)}};
                    CursorState next{kind, key, fields, offset, std::move(leftover), fullBatch};
                    payload["next_cursor"] = CreateCursor(std::move(next));
                    return ToolResult(id, ProjectResultFields(std::move(payload), fields));
                }
                break;
            }
            if (!fullBatch) {
                break;
            }
        }

        Json payload = {{"status", "ok"}, {"results", std::move(pageRows)}};
        return ToolResult(id, ProjectResultFields(std::move(payload), fields));
    }

    Json PreviewPlaylistPack(const Json& id, const Json& arguments)
    {
        if (!HasOnlyFields(arguments, {"domain", "style", "theme", "playlist", "query", "file_ids", "limit"})
            || !arguments.contains("domain") || !arguments["domain"].is_string()
            || arguments["domain"] != "game_sfx" || !arguments.contains("playlist")
            || !arguments["playlist"].is_string()
            || !ValidPlaylistLabel(arguments["playlist"].get_ref<const std::string&>())) {
            return RpcError(id, -32602, "Invalid game_sfx Playlist preview arguments");
        }
        if (!arguments.contains("query") && !arguments.contains("file_ids")) {
            return RpcError(id, -32602, "game_sfx preview requires query or file_ids");
        }
        if (arguments.contains("query") && (!arguments["query"].is_string()
            || !ValidQuery(arguments["query"].get_ref<const std::string&>()))) {
            return RpcError(id, -32602, "Query must be non-empty text of at most 256 characters");
        }
        if (arguments.contains("file_ids") && (!arguments["file_ids"].is_array()
            || arguments["file_ids"].empty() || arguments["file_ids"].size() > MaxLimit)) {
            return RpcError(id, -32602, "file_ids must contain one to 50 positive integers");
        }
        const auto style = arguments.value("style", std::string("realistic"));
        const auto theme = arguments.value("theme", std::string());
        if (!ValidPlaylistLabel(style) || (!theme.empty() && theme.size() > 128)) {
            return RpcError(id, -32602, "Invalid style or theme");
        }
        int limit = MaxLimit;
        if (arguments.contains("limit")) {
            const auto inputLimit = PositiveInteger(arguments["limit"], MaxLimit);
            if (!inputLimit) {
                return RpcError(id, -32602, "Limit must be an integer from 1 to 50");
            }
            limit = static_cast<int>(*inputLimit);
        }

        HarkenSearch::ExternalAiAuthorization authorization;
        std::string authorizationError;
        if (!LoadAuthorization(config_, authorization, authorizationError)) {
            return ToolResult(id, {{"status", "authorization_unavailable"}});
        }
        if (!CapabilityGranted(authorization, HarkenSearch::ExternalAiCapability::ReadPlaylist, config_)) {
            return ToolResult(id, {{"status", "not_authorized"}, {"reason", "readPlaylist is not granted"}});
        }
        if (arguments.contains("query") && !CapabilityGranted(
                authorization, HarkenSearch::ExternalAiCapability::SearchAudio, config_)) {
            return ToolResult(id, {{"status", "not_authorized"}, {"reason", "searchAudio is not granted"}});
        }

        std::map<std::int64_t, Json> candidates;
        if (arguments.contains("query")) {
            int searchOffset = 0;
            while (candidates.size() < static_cast<std::size_t>(limit)
                && searchOffset <= MaxCursorOffset) {
                const auto payload = library_.SearchAudio(
                    arguments["query"].get_ref<const std::string&>(), MaxLimit, searchOffset);
                if (payload.value("status", "") != "ok") {
                    return ToolResult(id, payload);
                }
                const auto rows = payload.value("results", Json::array());
                for (const auto& row : rows) {
                    if (row.is_object() && row.contains("file_id") && row["file_id"].is_number_integer()
                        && AuthorizedRow(authorization, HarkenSearch::ExternalAiCapability::SearchAudio,
                            config_, row)) {
                        candidates[row["file_id"].get<std::int64_t>()] = row;
                    }
                }
                if (rows.size() < static_cast<std::size_t>(MaxLimit)) {
                    break;
                }
                searchOffset += static_cast<int>(rows.size());
            }
        }
        if (arguments.contains("file_ids")) {
            for (const auto& value : arguments["file_ids"]) {
                const auto fileId = PositiveInteger(value, MaxReferenceId);
                if (!fileId) {
                    return RpcError(id, -32602, "file_ids must contain positive integers");
                }
                std::string relativePath;
                if (library_.RelativePath(*fileId, relativePath)) {
                    candidates[*fileId] = {{"file_id", *fileId}, {"relative_path", relativePath}};
                }
            }
        }

        PendingPlaylistProposal proposal;
        proposal.libraryId = config_.libraryId;
        proposal.domain = "game_sfx";
        proposal.style = style;
        proposal.theme = theme;
        proposal.playlist = arguments["playlist"].get_ref<const std::string&>();
        Json selections = Json::array();
        for (const auto& candidate : candidates) {
            if (proposal.selections.size() >= static_cast<std::size_t>(limit)) {
                break;
            }
            const auto fileId = candidate.first;
            std::string relativePath;
            if (!library_.RelativePath(fileId, relativePath)) {
                continue;
            }
            const auto target = config_.AbsolutePath(relativePath);
            if (authorization.Authorize(HarkenSearch::ExternalAiCapability::ReadFileName,
                    config_.libraryId, target) == HarkenSearch::ExternalAiAccess::Denied
                || authorization.Authorize(HarkenSearch::ExternalAiCapability::ReadMetadata,
                    config_.libraryId, target) == HarkenSearch::ExternalAiAccess::Denied
                || authorization.Authorize(HarkenSearch::ExternalAiCapability::ReadPlaylist,
                    config_.libraryId, target) == HarkenSearch::ExternalAiAccess::Denied) {
                continue;
            }
            const auto metadata = library_.GetMetadata(fileId);
            if (metadata.value("status", "") != "ok"
                || metadata.value("relative_path", "") != relativePath) {
                continue;
            }
            Json row = candidate.second;
            row["file_id"] = fileId;
            row["relative_path"] = relativePath;
            row["name"] = metadata.value("name", row.value("name", std::string()));
            const auto classification = ClassifyGameSfx(row, metadata);
            if (classification.value("excluded", false)) {
                continue;
            }
            Json selection = {
                {"file_id", fileId},
                {"name", row.value("name", std::string())},
                {"relative_path", relativePath},
                {"playlist_path", proposal.playlist + "/" + classification.value("taxonomy", "Review Needed")},
                {"confidence", classification.value("confidence", 0.35)},
                {"reason", classification.value("reason", std::string())},
                {"source", classification.value("source", "filename_metadata")},
                {"needs_review", classification.value("needs_review", true)},
            };
            selections.push_back(selection);
            proposal.selections.push_back({fileId, relativePath, metadata.value("metadata", Json::object()), std::move(selection)});
        }

        const std::string proposalId = "playlist-pack-proposal-" + std::to_string(nextPlaylistProposalId_++);
        if (playlistProposals_.size() >= 16) {
            playlistProposals_.erase(playlistProposals_.begin());
        }
        playlistProposals_[proposalId] = std::move(proposal);
        return ToolResult(id, {
            {"status", "ok"},
            {"confirmation", "required"},
            {"proposal_id", proposalId},
            {"domain", "game_sfx"},
            {"profile", GameSfxProfile()},
            {"style", style},
            {"theme", theme},
            {"playlist", arguments["playlist"]},
            {"selections", std::move(selections)},
        });
    }

    // Pipe name must match ExternalAiBridgeServer: same FNV-1a fold, 16 hex digits.
    static std::string BridgeHashLibraryId(const std::string& libraryId)
    {
        std::uint64_t value = 1469598103934665603ull;
        for (const char unit : libraryId) {
            value ^= static_cast<unsigned char>(unit);
            value *= 1099511628211ull;
        }
        static const char* hex = "0123456789abcdef";
        std::string out(16, '0');
        for (int index = 15; index >= 0; --index) {
            out[static_cast<std::size_t>(index)] = hex[value & 0x0F];
            value >>= 4;
        }
        return out;
    }

    // One request, one response over the Harken-side pipe. False when Harken is
    // closed or does not answer in time, so the caller stays fail-closed.
    static bool CallPlaylistBridge(const std::string& libraryId, const std::string& requestBody,
                                   std::string& responseBody)
    {
#ifdef _WIN32
        const std::string pipePath = "\\\\.\\pipe\\harken-external-ai-" + BridgeHashLibraryId(libraryId);
        const std::wstring name(pipePath.begin(), pipePath.end());
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            return false;
        }
        auto transfer = [&](bool writing, std::string& buffer, DWORD& moved) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) {
                return false;
            }
            const BOOL started = writing
                ? WriteFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &moved, &overlapped)
                : ReadFile(pipe, &buffer[0], static_cast<DWORD>(buffer.size()), &moved, &overlapped);
            bool ok = false;
            if (!started && GetLastError() == ERROR_IO_PENDING) {
                ok = WaitForSingleObject(overlapped.hEvent, 15000) == WAIT_OBJECT_0
                     && GetOverlappedResult(pipe, &overlapped, &moved, FALSE);
            } else {
                ok = started != FALSE;
            }
            CloseHandle(overlapped.hEvent);
            return ok && moved > 0;
        };
        std::string payload = requestBody + "\n";
        DWORD written = 0;
        std::string incoming(8192, '\0');
        DWORD read = 0;
        bool received = transfer(true, payload, written) && transfer(false, incoming, read);
        if (received) {
            incoming.resize(read);
            const auto newline = incoming.find('\n');
            if (newline != std::string::npos) {
                incoming.resize(newline);
            }
            responseBody = incoming;
        }
        CloseHandle(pipe);
        return received;
#else
        (void)libraryId;
        (void)requestBody;
        (void)responseBody;
        return false;
#endif
    }

    Json ApplyPlaylistPack(const Json& id, const Json& arguments)
    {
        if (!HasOnlyFields(arguments, {"proposal_id", "confirm"})
            || !arguments.contains("proposal_id") || !arguments["proposal_id"].is_string()
            || arguments["proposal_id"].get_ref<const std::string&>().empty()
            || arguments["proposal_id"].get_ref<const std::string&>().size() > 128
            || !arguments.contains("confirm") || !arguments["confirm"].is_boolean()) {
            return RpcError(id, -32602, "Invalid Playlist apply arguments");
        }
        if (!arguments["confirm"].get<bool>()) {
            return ToolResult(id, {{"status", "confirmation_required"}});
        }
        HarkenSearch::ExternalAiAuthorization authorization;
        std::string authorizationError;
        if (!LoadAuthorization(config_, authorization, authorizationError)) {
            return ToolResult(id, {{"status", "authorization_unavailable"}});
        }
        const auto proposalId = arguments["proposal_id"].get_ref<const std::string&>();
        const auto proposal = playlistProposals_.find(proposalId);
        if (proposal == playlistProposals_.end()) {
            return ToolResult(id, {{"status", "proposal_not_found"}});
        }
        auto& pending = proposal->second;
        if (pending.libraryId != config_.libraryId || pending.domain != "game_sfx") {
            playlistProposals_.erase(proposal);
            return ToolResult(id, {{"status", "proposal_stale"}});
        }
        if (!CapabilityGranted(authorization, HarkenSearch::ExternalAiCapability::ReadPlaylist, config_)
            || !CapabilityGranted(authorization, HarkenSearch::ExternalAiCapability::EditPlaylist, config_)) {
            return ToolResult(id, {{"status", "not_authorized"}});
        }
        for (const auto& selection : pending.selections) {
            const auto target = config_.AbsolutePath(selection.relativePath);
            if (authorization.Authorize(HarkenSearch::ExternalAiCapability::ReadPlaylist,
                    config_.libraryId, target) == HarkenSearch::ExternalAiAccess::Denied
                || authorization.Authorize(HarkenSearch::ExternalAiCapability::EditPlaylist,
                    config_.libraryId, target) != HarkenSearch::ExternalAiAccess::Editable
                || authorization.Authorize(HarkenSearch::ExternalAiCapability::ReadMetadata,
                    config_.libraryId, target) == HarkenSearch::ExternalAiAccess::Denied) {
                return ToolResult(id, {{"status", "not_authorized"}});
            }
            std::string currentPath;
            const auto current = library_.GetMetadata(selection.fileId);
            if (!library_.RelativePath(selection.fileId, currentPath)
                || currentPath != selection.relativePath
                || current.value("status", "") != "ok"
                || current.value("metadata", Json::object()) != selection.metadata) {
                playlistProposals_.erase(proposal);
                return ToolResult(id, {{"status", "proposal_stale"}});
            }
        }
        // Hand the confirmed batch to Harken: it owns the database, its dedupe and
        // its ordering rules, so the connector only asks and reports the receipt.
        Json requestedIds = Json::array();
        for (const auto& selection : pending.selections) {
            requestedIds.push_back(selection.fileId);
        }
        const Json bridgeRequest = {
            {"token", authorization.bridgeToken()},
            {"requestId", proposalId},
            {"libraryId", config_.libraryId},
            {"playlistPath", pending.playlist},
            {"fileIds", requestedIds},
        };
        std::string bridgeResponse;
        if (!CallPlaylistBridge(config_.libraryId, bridgeRequest.dump(), bridgeResponse)) {
            // Harken is closed or did not answer: nothing was written.
            return ToolResult(id, {
                {"status", "playlist_write_unavailable"},
                {"proposal_id", proposalId},
                {"blocker", "harken_write_bridge_unavailable"},
                {"message", "Open Harken with this library, then confirm again."},
            });
        }
        Json receipt = Json::parse(bridgeResponse, nullptr, false);
        if (receipt.is_discarded() || !receipt.is_object()) {
            return ToolResult(id, {{"status", "write_failed"}, {"proposal_id", proposalId}});
        }
        receipt["proposal_id"] = proposalId;
        if (receipt.value("status", std::string()) == "ok") {
            playlistProposals_.erase(proposal);
        }
        return ToolResult(id, receipt);
    }

    Json CallTool(const Json& id, const Json& params)
    {
        if (!HasOnlyFields(params, {"name", "arguments", "_meta"})
            || !params.contains("name") || !params["name"].is_string() || !params.contains("arguments")) {
            return RpcError(id, -32602, "Invalid tool parameters");
        }
        const auto& name = params["name"].get_ref<const std::string&>();
        const auto tool = std::find_if(Tools.begin(), Tools.end(), [&](const ToolDefinition& definition) {
            return definition.name == name;
        });
        if (tool == Tools.end()) {
            return RpcError(id, -32602, "Unknown tool");
        }

        const Json& arguments = params["arguments"];
        if (!arguments.is_object()) {
            return RpcError(id, -32602, "Invalid tool arguments");
        }

        if (tool->kind == ToolKind::PreviewPlaylistPack) {
            return PreviewPlaylistPack(id, arguments);
        }
        if (tool->kind == ToolKind::ApplyPlaylistPack) {
            return ApplyPlaylistPack(id, arguments);
        }

        if (tool->kind == ToolKind::GetMetadata ||
            tool->kind == ToolKind::PreviewMetadataUpdate ||
            tool->kind == ToolKind::ApplyMetadataUpdate) {
            HarkenSearch::ExternalAiAuthorization authorization;
            std::string authorizationError;
            if (!LoadAuthorization(config_, authorization, authorizationError)) {
                return ToolResult(id, {{"status", "authorization_unavailable"}});
            }

            if (tool->kind == ToolKind::ApplyMetadataUpdate) {
                if (!HasOnlyFields(arguments, {"proposal_id"}) || !arguments.contains("proposal_id")
                    || !arguments["proposal_id"].is_string()
                    || arguments["proposal_id"].get_ref<const std::string&>().empty()
                    || arguments["proposal_id"].get_ref<const std::string&>().size() > 128) {
                    return RpcError(id, -32602, "Invalid proposal ID");
                }
                const auto proposalId = arguments["proposal_id"].get_ref<const std::string&>();
                const auto proposal = proposals_.find(proposalId);
                if (proposal == proposals_.end()) {
                    return ToolResult(id, {{"status", "proposal_not_found"}});
                }
                const auto& pending = proposal->second;
                const auto target = config_.AbsolutePath(pending.relativePath);
                if (!HasAccess(authorization.Authorize(
                        HarkenSearch::ExternalAiCapability::EditMetadata, config_.libraryId, target), true)
                    || !HasAccess(authorization.Authorize(
                        HarkenSearch::ExternalAiCapability::AiNamingMetadata, config_.libraryId, target), true)) {
                    return ToolResult(id, {{"status", "not_authorized"}});
                }
                const auto current = library_.GetMetadata(pending.fileId);
                if (current.value("status", "") != "ok"
                    || current.value("relative_path", "") != pending.relativePath
                    || current.value("metadata", Json::object()) != pending.before) {
                    proposals_.erase(proposal);
                    return ToolResult(id, {{"status", "proposal_stale"}});
                }
                std::string writeError;
                if (!library_.ApplyMetadata(pending.fileId, pending.fields, writeError)) {
                    return ToolResult(id, {{"status", "write_failed"}});
                }
                Json appliedPayload = {
                    {"status", "applied"},
                    {"proposal_id", proposalId},
                    {"file_id", pending.fileId},
                    {"relative_path", pending.relativePath},
                    {"before", pending.before},
                    {"after", pending.after},
                };
                proposals_.erase(proposal);
                return ToolResult(id, std::move(appliedPayload));
            }

            if (tool->kind == ToolKind::GetMetadata) {
                if (!HasOnlyFields(arguments, {"file_id"}) || !arguments.contains("file_id")) {
                    return RpcError(id, -32602, "Invalid metadata arguments");
                }
                const auto fileId = PositiveInteger(arguments["file_id"], MaxReferenceId);
                if (!fileId) {
                    return RpcError(id, -32602, "File ID must be a positive integer");
                }
                std::string relativePath;
                if (!library_.RelativePath(*fileId, relativePath)) {
                    return ToolResult(id, {{"status", "reference_not_found"}, {"results", Json::array()}});
                }
                if (!HasAccess(authorization.Authorize(
                        HarkenSearch::ExternalAiCapability::ReadMetadata, config_.libraryId,
                        config_.AbsolutePath(relativePath)), false)) {
                    return ToolResult(id, {{"status", "reference_not_authorized"}, {"results", Json::array()}});
                }
                return ToolResult(id, library_.GetMetadata(*fileId));
            }

            if (!HasOnlyFields(arguments, {"file_id", "fields"})
                || !arguments.contains("file_id") || !arguments.contains("fields")
                || !ValidMetadataFields(arguments["fields"])) {
                return ToolResult(id, {{"status", "invalid_fields"}});
            }
            const auto fileId = PositiveInteger(arguments["file_id"], MaxReferenceId);
            if (!fileId) {
                return RpcError(id, -32602, "File ID must be a positive integer");
            }
            std::string relativePath;
            if (!library_.RelativePath(*fileId, relativePath)) {
                return ToolResult(id, {{"status", "reference_not_found"}});
            }
            const auto target = config_.AbsolutePath(relativePath);
            if (!HasAccess(authorization.Authorize(
                    HarkenSearch::ExternalAiCapability::ReadMetadata, config_.libraryId, target), false)
                || !HasAccess(authorization.Authorize(
                    HarkenSearch::ExternalAiCapability::AiNamingMetadata, config_.libraryId, target), false)) {
                return ToolResult(id, {{"status", "not_authorized"}});
            }
            const auto current = library_.GetMetadata(*fileId);
            if (current.value("status", "") != "ok") {
                return ToolResult(id, current);
            }
            Json after = current.value("metadata", Json::object());
            for (auto entry = arguments["fields"].begin(); entry != arguments["fields"].end(); ++entry) {
                after[entry.key()] = entry.value();
            }
            const std::string proposalId = "metadata-proposal-" + std::to_string(nextProposalId_++);
            if (proposals_.size() >= 16) {
                proposals_.erase(proposals_.begin());
            }
            proposals_[proposalId] = PendingMetadataProposal{
                *fileId, relativePath, arguments["fields"], current["metadata"], after};
            return ToolResult(id, {
                {"status", "ok"},
                {"confirmation", "required"},
                {"proposal_id", proposalId},
                {"file_id", *fileId},
                {"relative_path", relativePath},
                {"before", current["metadata"]},
                {"after", std::move(after)},
            });
        }

        const char* required = tool->requiredArgument();
        if (!HasOnlyFields(arguments, {required, "limit", "fields", "cursor"})
            || !arguments.contains(required)) {
            return RpcError(id, -32602, "Invalid tool arguments");
        }
        int limit = DefaultLimit;
        if (arguments.contains("limit")) {
            const auto inputLimit = PositiveInteger(arguments["limit"], MaxLimit);
            if (!inputLimit) {
                return RpcError(id, -32602, "Limit must be an integer from 1 to 50");
            }
            limit = static_cast<int>(*inputLimit);
        }

        const auto parsedFields = ParseResultFields(arguments, tool->kind);
        if (!parsedFields) {
            return RpcError(id, -32602, "Invalid fields");
        }

        HarkenSearch::ExternalAiAuthorization authorization;
        std::string authorizationError;
        if (!LoadAuthorization(config_, authorization, authorizationError)) {
            return ToolResult(id, {{"status", "authorization_unavailable"}, {"results", Json::array()}});
        }
        const auto capability = CapabilityFor(tool->kind);
        if (!CapabilityGranted(authorization, capability, config_)) {
            return ToolResult(id, {{"status", "not_authorized"}, {"results", Json::array()}});
        }

        const Json& input = arguments[required];
        try {
            std::string key;
            if (tool->kind == ToolKind::FindSimilar) {
                const auto reference = PositiveInteger(input, MaxReferenceId);
                if (!reference) {
                    return RpcError(id, -32602, "Reference ID must be a positive integer");
                }
                std::string referenceRelative;
                if (!library_.RelativePath(*reference, referenceRelative)) {
                    return ToolResult(id, {{"status", "reference_not_found"}, {"results", Json::array()}});
                }
                if (authorization.Authorize(capability, config_.libraryId, config_.AbsolutePath(referenceRelative))
                    == HarkenSearch::ExternalAiAccess::Denied) {
                    return ToolResult(id, {{"status", "reference_not_authorized"}, {"results", Json::array()}});
                }
                key = std::to_string(*reference);
            } else {
                if (!input.is_string() || !ValidQuery(input.get_ref<const std::string&>())) {
                    return RpcError(id, -32602, "Query must be non-empty text of at most 256 characters");
                }
                key = input.get_ref<const std::string&>();
            }

            std::vector<std::string> fields = *parsedFields;
            CursorState state{tool->kind, key, fields, 0, Json::array(), true};
            if (arguments.contains("cursor")) {
                if (!arguments["cursor"].is_string()
                    || arguments["cursor"].get_ref<const std::string&>().empty()
                    || arguments["cursor"].get_ref<const std::string&>().size() > 128) {
                    return RpcError(id, -32602, "Invalid cursor");
                }
                const auto cursor = arguments["cursor"].get_ref<const std::string&>();
                const auto found = cursors_.find(cursor);
                if (found == cursors_.end() || found->second.kind != state.kind
                    || found->second.key != state.key
                    || (arguments.contains("fields") && found->second.fields != fields)) {
                    return RpcError(id, -32602, "Invalid or expired cursor");
                }
                state = std::move(found->second);
                cursors_.erase(found);
                if (!arguments.contains("fields")) {
                    fields = state.fields;
                }
            }
            return QueryTool(id, tool->kind, key, limit, fields, authorization, capability, std::move(state));
        } catch (const std::exception&) {
            return ToolResult(id, {{"status", "query_failed"}, {"results", Json::array()}});
        }
    }

    Json ToolResult(const Json& id, Json payload) const
    {
        Json content = {{"type", "text"}, {"text", structuredContent_
            ? "Tool result is available in structuredContent."
            : payload.dump()}};
        Json result = {{"content", Json::array({std::move(content)})}};
        if (FailedStatus(payload)) {
            result["isError"] = true;
        }
        if (structuredContent_) {
            result["structuredContent"] = std::move(payload);
        }
        return RpcResult(id, std::move(result));
    }

    ExternalAiLibrary& library_;
    const LaunchConfig& config_;
    struct PendingMetadataProposal {
        std::int64_t fileId = 0;
        std::string relativePath;
        Json fields;
        Json before;
        Json after;
    };
    std::unordered_map<std::string, PendingMetadataProposal> proposals_;
    std::unordered_map<std::string, PendingPlaylistProposal> playlistProposals_;
    std::unordered_map<std::string, CursorState> cursors_;
    std::uint64_t nextProposalId_ = 1;
    std::uint64_t nextPlaylistProposalId_ = 1;
    std::uint64_t nextCursorId_ = 1;
    Phase phase_ = Phase::AwaitingInitialize;
    bool structuredContent_ = false;
};

enum class Frame { End, Complete, Incomplete, Oversized };

Frame ReadFrame(std::string& line)
{
    line.clear();
    for (int byte = std::cin.get(); byte != std::char_traits<char>::eof(); byte = std::cin.get()) {
        if (byte == '\n') {
            return Frame::Complete;
        }
        if (line.size() == MaxLineBytes) {
            std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
            return Frame::Oversized;
        }
        line.push_back(static_cast<char>(byte));
    }
    return line.empty() ? Frame::End : Frame::Incomplete;
}

std::optional<Json> HandleFrame(Session& session, Frame frame, const std::string& line)
{
    if (frame == Frame::Oversized) {
        return RpcError(nullptr, -32700, "Message too large");
    }
    if (frame == Frame::Incomplete) {
        return RpcError(nullptr, -32700, "Incomplete message");
    }
    if (line.find('\0') != std::string::npos) {
        return RpcError(nullptr, -32700, "Invalid JSON message");
    }

    bool bounded = true;
    const auto message = Json::parse(line, [&](int depth, Json::parse_event_t, Json&) {
        bounded = bounded && depth < MaxJsonDepth;
        return bounded;
    }, false);
    if (message.is_discarded() || !bounded) {
        return RpcError(nullptr, -32700, "Invalid JSON message");
    }
    return session.Dispatch(message);
}

int Serve(ExternalAiLibrary& library, const LaunchConfig& config)
{
    Session session(library, config);
    std::string line;
    line.reserve(MaxLineBytes);
    int consecutiveRejections = 0;
    for (Frame frame = ReadFrame(line); frame != Frame::End; frame = ReadFrame(line)) {
        const auto response = HandleFrame(session, frame, line);
        if (!response) {
            continue;
        }
        std::cout << response->dump() << '\n' << std::flush;
        if (!std::cout) {
            return 1;
        }
        consecutiveRejections = response->contains("error") ? consecutiveRejections + 1 : 0;
        if (consecutiveRejections >= MaxConsecutiveRejections) {
            std::cerr << "HarkenExternalAiMcp: too many rejected requests.\n";
            return 1;
        }
    }
    return 0;
}

bool IsLocalAbsolutePath(const std::filesystem::path& path)
{
    if (!path.is_absolute()) {
        return false;
    }
#ifdef _WIN32
    const auto drive = path.root_name().native();
    if (drive.size() != 2 || drive.back() != L':') {
        return false;
    }
    const auto root = path.root_path().make_preferred();
    const auto type = GetDriveTypeW(root.c_str());
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_CDROM || type == DRIVE_RAMDISK;
#else
    return true;
#endif
}

template <typename Character>
bool ReadLaunchPaths(int argc, Character** argv, LaunchConfig& config)
{
    if (argc != 9) {
        return false;
    }
    for (int index = 1; index < argc; index += 2) {
        const auto flag = std::filesystem::path(argv[index]).u8string();
        const std::filesystem::path value(argv[index + 1]);
        if (flag == "--library-id") {
            const auto id = value.u8string();
            if (id.empty() || id.size() > 512) {
                return false;
            }
            config.libraryId = id;
            continue;
        }
        if (!IsLocalAbsolutePath(value)) {
            return false;
        }
        if (flag == "--library-db" && config.database.empty()) {
            config.database = value.u8string();
        } else if (flag == "--library-root" && config.root.empty()) {
            config.root = value.u8string();
        } else if (flag == "--settings" && config.settings.empty()) {
            config.settings = value.u8string();
        } else {
            return false;
        }
    }
    return !config.database.empty() && !config.root.empty() && !config.settings.empty() && !config.libraryId.empty();
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try {
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
            std::cerr << "HarkenExternalAiMcp: could not configure stdio.\n";
            return 1;
        }
#endif
        std::ios::sync_with_stdio(false);
        std::cin.tie(nullptr);

        LaunchConfig config;
        if (!ReadLaunchPaths(argc, argv, config)) {
            std::cerr << "HarkenExternalAiMcp: --library-db, --library-root, --settings and --library-id are required; paths must be local and absolute.\n";
            return 1;
        }
        HarkenSearch::ExternalAiAuthorization authorization;
        std::string authorizationError;
        if (!LoadAuthorization(config, authorization, authorizationError)) {
            std::cerr << "HarkenExternalAiMcp: " << authorizationError << ".\n";
            return 1;
        }
        ExternalAiLibrary library;
        std::string error;
        if (!library.Open(config.database, config.root, error)) {
            std::cerr << "HarkenExternalAiMcp: could not open the authorized library.\n";
            return 1;
        }
        return Serve(library, config);
    } catch (const std::exception&) {
        std::cerr << "HarkenExternalAiMcp: failed to serve the authorized library.\n";
        return 1;
    }
}
