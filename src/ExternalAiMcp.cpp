#include "ExternalAiLibrary.h"
#include "../thirdparty/nlohmann_json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
constexpr int MaxJsonDepth = 32;
constexpr int MaxConsecutiveRejections = 16;
constexpr int MaxQueryLength = 256;
constexpr int DefaultLimit = 10;
constexpr int MaxLimit = 50;
constexpr auto MaxReferenceId = (std::numeric_limits<std::int64_t>::max)();

enum class ToolKind { SearchAudio, FindFolders, FindSimilar };

struct ToolDefinition {
    const char* name;
    const char* description;
    ToolKind kind;

    const char* requiredArgument() const
    {
        return kind == ToolKind::FindSimilar ? "reference_id" : "query";
    }
};

constexpr std::array<ToolDefinition, 3> Tools{{
    {"harken_search_audio", "Find audio metadata by literal text in the authorized library.", ToolKind::SearchAudio},
    {"harken_find_folders", "Find folders by literal text in the authorized library.", ToolKind::FindFolders},
    {"harken_find_similar", "Find similar audio by file ID using the existing index. Match scores are ranking points, not probabilities.", ToolKind::FindSimilar},
}};

Json RpcResult(const Json& id, Json result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json RpcError(const Json& id, int code, const char* message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
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

Json ListTools()
{
    Json list = Json::array();
    for (const ToolDefinition& tool : Tools) {
        Json requiredSchema = tool.kind == ToolKind::FindSimilar
            ? Json{{"type", "integer"}, {"minimum", 1}, {"maximum", MaxReferenceId}}
            : Json{{"type", "string"}, {"minLength", 1}, {"maxLength", MaxQueryLength}};
        list.push_back({
            {"name", tool.name},
            {"description", tool.description},
            {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"openWorldHint", false}}},
            {"inputSchema", {
                {"type", "object"},
                {"additionalProperties", false},
                {"required", Json::array({tool.requiredArgument()})},
                {"properties", {
                    {tool.requiredArgument(), std::move(requiredSchema)},
                    {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxLimit}, {"default", DefaultLimit}}},
                }},
            }},
        });
    }
    return {{"tools", std::move(list)}};
}

class Session {
public:
    explicit Session(ExternalAiLibrary& library) : library_(library) {}

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
        const char* required = tool->requiredArgument();
        if (!HasOnlyFields(arguments, {required, "limit"}) || !arguments.contains(required)) {
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

        const Json& input = arguments[required];
        try {
            if (tool->kind == ToolKind::FindSimilar) {
                const auto reference = PositiveInteger(input, MaxReferenceId);
                if (!reference) {
                    return RpcError(id, -32602, "Reference ID must be a positive integer");
                }
                return ToolResult(id, library_.FindSimilar(*reference, limit));
            }
            if (!input.is_string() || !ValidQuery(input.get_ref<const std::string&>())) {
                return RpcError(id, -32602, "Query must be non-empty text of at most 256 characters");
            }
            const auto& query = input.get_ref<const std::string&>();
            return ToolResult(id, tool->kind == ToolKind::SearchAudio
                ? library_.SearchAudio(query, limit)
                : library_.FindFolders(query, limit));
        } catch (const std::exception&) {
            return ToolResult(id, {{"status", "query_failed"}, {"results", Json::array()}});
        }
    }

    Json ToolResult(const Json& id, Json payload) const
    {
        Json content = {{"type", "text"}, {"text", payload.dump()}};
        Json result = {{"content", Json::array({std::move(content)})}};
        if (payload["status"] == "query_failed") {
            result["isError"] = true;
        }
        if (structuredContent_) {
            result["structuredContent"] = std::move(payload);
        }
        return RpcResult(id, std::move(result));
    }

    ExternalAiLibrary& library_;
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

int Serve(ExternalAiLibrary& library)
{
    Session session(library);
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
bool ReadLaunchPaths(int argc, Character** argv, std::string& database, std::string& root)
{
    if (argc != 5) {
        return false;
    }
    for (int index = 1; index < argc; index += 2) {
        const auto flag = std::filesystem::path(argv[index]).u8string();
        const std::filesystem::path value(argv[index + 1]);
        if (!IsLocalAbsolutePath(value)) {
            return false;
        }
        if (flag == "--library-db" && database.empty()) {
            database = value.u8string();
        } else if (flag == "--library-root" && root.empty()) {
            root = value.u8string();
        } else {
            return false;
        }
    }
    return !database.empty() && !root.empty();
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

        std::string database;
        std::string root;
        if (!ReadLaunchPaths(argc, argv, database, root)) {
            std::cerr << "HarkenExternalAiMcp: both --library-db and --library-root require explicit local absolute paths.\n";
            return 1;
        }
        ExternalAiLibrary library;
        std::string error;
        if (!library.Open(database, root, error)) {
            std::cerr << "HarkenExternalAiMcp: could not open the authorized library read-only.\n";
            return 1;
        }
        return Serve(library);
    } catch (const std::exception&) {
        std::cerr << "HarkenExternalAiMcp: failed to serve the authorized library.\n";
        return 1;
    }
}
