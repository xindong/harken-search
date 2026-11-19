#include "ExternalAiAuthorization.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;
using HarkenSearch::ExternalAiAccess;
using HarkenSearch::ExternalAiAuthorization;
using HarkenSearch::ExternalAiCapability;

int failures = 0;

void Check(bool condition, const char* description)
{
    std::cout << (condition ? "PASS: " : "FAIL: ") << description << '\n';
    failures += !condition;
}

ExternalAiAuthorization ParseOk(const std::string& json)
{
    ExternalAiAuthorization authorization;
    std::string error;
    const bool ok = ExternalAiAuthorization::Parse(json, authorization, error);
    Check(ok, ("parse: " + json.substr(0, 60)).c_str());
    if (!ok) {
        std::cout << "  error: " << error << '\n';
    }
    return authorization;
}

bool ParseRejected(const std::string& json)
{
    ExternalAiAuthorization authorization;
    std::string error;
    return !ExternalAiAuthorization::Parse(json, authorization, error) && !error.empty();
}

std::string Escape(const std::string& value)
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

std::string Document(const std::string& root, const std::string& capabilities, bool enabled = true,
    bool sample = false, const std::string& libraryId = "library-a", const std::string& rootsJson = "")
{
    const std::string roots = rootsJson.empty() ? "[\"" + Escape(root) + "\"]" : rootsJson;
    return std::string("{\"libraryId\":\"") + libraryId + "\",\"enabled\":" + (enabled ? "true" : "false") +
        ",\"sampleLibrary\":" + (sample ? "true" : "false") + ",\"roots\":" + roots +
        ",\"capabilities\":" + capabilities + "}";
}

}  // namespace

int main(int argc, char** argv)
{
    fs::path scratch = argc > 1 ? fs::u8path(argv[1]) : fs::current_path();
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = scratch / ("external-ai-authorization-" + std::to_string(serial));
    const fs::path root = base / "Sfx";
    const fs::path prefixTwin = base / "Sfx2";
    const fs::path outside = base / "Elsewhere";
    fs::create_directories(root / "wind");
    fs::create_directories(prefixTwin / "wind");
    fs::create_directories(outside / "wind");

    const auto rootText = root.u8string();
    const auto prefixText = prefixTwin.u8string();
    const auto outsideText = outside.u8string();
    const auto insideFile = (root / "wind" / "door.wav").u8string();
    const auto prefixFile = (prefixTwin / "wind" / "door.wav").u8string();
    const auto outsideFile = (outside / "wind" / "door.wav").u8string();

    const std::string allRead = "{\"searchAudio\":\"read\",\"findSimilar\":\"read\",\"findFolders\":\"read\","
        "\"readFileName\":\"read\",\"readMetadata\":\"read\",\"editMetadata\":\"none\"}";

    const auto allowed = ParseOk(Document(rootText, allRead));
    Check(allowed.enabled() && allowed.libraryId() == "library-a", "parsed switch and library identity");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", insideFile) == ExternalAiAccess::ReadOnly,
        "granted root returns read-only");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", rootText) == ExternalAiAccess::ReadOnly,
        "root itself is inside the grant");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", prefixFile) == ExternalAiAccess::Denied,
        "shared name prefix is not inside the root");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", outsideFile) == ExternalAiAccess::Denied,
        "sibling directory is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", "") == ExternalAiAccess::Denied,
        "library-wide query without a target is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", "wind/door.wav") == ExternalAiAccess::Denied,
        "relative path is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-a", rootText + "/../Elsewhere/wind/door.wav") ==
            ExternalAiAccess::Denied,
        "traversal out of the root is denied");
    Check(allowed.Authorize(ExternalAiCapability::SearchAudio, "library-b", insideFile) == ExternalAiAccess::Denied,
        "another library identity is denied");

    const auto disabled = ParseOk(Document(rootText, allRead, false));
    Check(disabled.Authorize(ExternalAiCapability::FindFolders, "library-a", rootText) == ExternalAiAccess::Denied,
        "switch off denies every capability");

    const auto noRoots = ParseOk(Document(rootText, allRead, true, false, "library-a", "[]"));
    Check(noRoots.Authorize(ExternalAiCapability::ReadMetadata, "library-a", insideFile) == ExternalAiAccess::Denied,
        "no ticked directory denies access");

    const auto separateReads = ParseOk(Document(rootText,
        "{\"readFileName\":\"read\",\"readMetadata\":\"none\",\"editMetadata\":\"edit\"}"));
    Check(separateReads.Authorize(ExternalAiCapability::ReadFileName, "library-a", insideFile) ==
            ExternalAiAccess::ReadOnly,
        "file name read stays independent");
    Check(separateReads.Authorize(ExternalAiCapability::ReadMetadata, "library-a", insideFile) == ExternalAiAccess::Denied,
        "metadata read stays closed when only file names are granted");
    Check(separateReads.Authorize(ExternalAiCapability::EditMetadata, "library-a", insideFile) ==
            ExternalAiAccess::Editable,
        "metadata edit is granted only when configured");

    const auto reversedReads = ParseOk(Document(rootText, "{\"readFileName\":\"none\",\"readMetadata\":\"read\"}"));
    Check(reversedReads.Authorize(ExternalAiCapability::ReadFileName, "library-a", insideFile) == ExternalAiAccess::Denied,
        "file name read can be closed while metadata stays readable");

    const auto sample = ParseOk(Document(rootText, "{\"editMetadata\":\"edit\"}", true, true));
    Check(sample.Authorize(ExternalAiCapability::EditMetadata, "library-a", insideFile) == ExternalAiAccess::ReadOnly,
        "sample library never becomes editable");

    const auto revoked = ParseOk(Document(rootText, "{\"searchAudio\":\"read\",\"editMetadata\":\"none\"}"));
    Check(revoked.Authorize(ExternalAiCapability::EditMetadata, "library-a", insideFile) == ExternalAiAccess::Denied,
        "lowered capability applies to the next request");

    const auto shrunk = ParseOk(Document(rootText, "{\"searchAudio\":\"read\"}", true, false, "library-a",
        "[\"" + Escape(prefixText) + "\"]"));
    Check(shrunk.Authorize(ExternalAiCapability::SearchAudio, "library-a", insideFile) == ExternalAiAccess::Denied,
        "removing a root revokes it immediately");

    Check(ParseRejected("{\"libraryId\":\"a\"}"), "missing enabled is rejected");
    Check(ParseRejected("{\"libraryId\":\"\",\"enabled\":true}"), "empty library id is rejected");
    Check(ParseRejected(Document(rootText, "{\"searchAudio\":\"write\"}")), "unknown access level is rejected");
    Check(ParseRejected(Document(rootText, "{\"searchAudioo\":\"read\"}")), "misspelled capability is rejected");
    Check(ParseRejected(Document(rootText, "{}", true, false, "library-a", "[\"\"]")), "empty root is rejected");
    Check(ParseRejected("{\"libraryId\":\"a\",\"enabled\":true,\"rootz\":[]}"), "unknown field is rejected");

    std::error_code ignored;
    fs::remove_all(base, ignored);
    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
